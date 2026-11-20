#include <gtest/gtest.h>
#include <sqlite3.h>
#include <array>
#include <fstream>
#include "am/application/library_service.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/catalog/indexer.hpp"

namespace {
namespace fs = std::filesystem;
namespace lib = am::library;
namespace cat = am::catalog;
using Kind = lib::DatabaseKind;
template<class T> T take(am::Result<T> result) {
  if (!result) throw std::runtime_error(am::to_log_string(result.error()));
  return std::move(*result);
}
void take(am::Result<void> result) {
  if (!result) throw std::runtime_error(am::to_log_string(result.error()));
}
lib::OpenOptions options(lib::Mode mode = lib::Mode::read_write) {
  lib::OpenOptions out; out.mode = mode; out.capabilities.emplace();
  out.capabilities->mode = am::repository::StorageMode::full; return out;
}
fs::path fresh() {
  const auto root = fs::path(AM_LIBRARY_TEST_ROOT) / am::Uuid::v7().to_string();
  take(lib::Library::init(root));
  { auto catalog = take(cat::Catalog::open(root, options())); take(catalog.indexer().rebuild()); }
  return root;
}
std::string bytes(const fs::path& path) {
  std::ifstream input(am::paths::native(path), std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
// Observe actual PRAGMAs on every connection opened in this test, including recovery's handles.
// The automatic extension is scoped and removed without resetting unrelated SQLite extensions.
thread_local std::array<int,3>* sql_checks = nullptr;
int trace_checks(unsigned, void*, void* statement, void*) {
  const auto* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
  if (!sql_checks || !sql) return 0;
  const std::string_view text(sql);
  if (!text.starts_with("PRAGMA integrity_check") && !text.starts_with("PRAGMA quick_check")) return 0;
  const auto* filename = sqlite3_db_filename(sqlite3_db_handle(static_cast<sqlite3_stmt*>(statement)), "main");
  if (!filename) return 0;
  const std::string_view path(filename);
  const auto kind = path.ends_with("index.sqlite") ? Kind::index : path.ends_with("state.sqlite") ? Kind::state : Kind::userdata;
  ++(*sql_checks)[static_cast<std::size_t>(kind)];
  return 0;
}
int install_trace(sqlite3* db, char**, const sqlite3_api_routines*) {
  return sqlite3_trace_v2(db, SQLITE_TRACE_STMT, trace_checks, nullptr);
}
struct TraceChecks {
  std::array<int,3> counts{};
  TraceChecks() {
    const int rc = sqlite3_auto_extension(reinterpret_cast<void(*)()>(install_trace));
    if (rc != SQLITE_OK) throw std::runtime_error("Trace extension failed");
    sql_checks = &counts;
  }
  ~TraceChecks() {
    (void)sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(install_trace));
    sql_checks = nullptr;
  }
};
void session(const fs::path& root, const char* value) {
  auto library = take(lib::Library::open(root, options()));
  am::db::IndexStore store(library.database(Kind::index)); take(store.set_meta("writer_session", value));
}
// Damage a data B-tree page, leaving the SQLite header, schema and recovery tables readable.
void damage(const fs::path& root, Kind kind) {
  std::int64_t page = 0, size = 0;
  {
    auto library = take(lib::Library::open(root, options()));
    auto connection = am::db::Connection::borrow(library.database(kind));
    take(connection.exec("CREATE TABLE corruption_probe(value TEXT); INSERT INTO corruption_probe VALUES('payload')"));
    auto query = take(connection.prepare("SELECT rootpage FROM sqlite_schema WHERE name='corruption_probe'"));
    take(query.step()); page = query.integer(0);
    auto pragma = take(connection.prepare("PRAGMA page_size")); take(pragma.step()); size = pragma.integer(0);
  }
  const char* name = kind == Kind::index ? "index" : kind == Kind::state ? "state" : "userdata";
  std::fstream file(am::paths::native(root / ".am" / (std::string(name) + ".sqlite")), std::ios::binary | std::ios::in | std::ios::out);
  file.seekp((page - 1) * size); file.put('\0'); file.flush();
  if (!file) throw std::runtime_error("Page corruption failed");
}
}
TEST(DatabaseChecks, CleanOpenSkipsChecksAndUnclosedWriterChecksThreeDatabases) {
  RecordProperty("req", "K-112|K-24");
  const auto root = fresh();
  std::array<int,3> checks{};
  TraceChecks sql;
  auto config = options();
  config.on_database_check = [&](Kind kind, lib::DatabaseCheckLevel level) {
    EXPECT_EQ(level, lib::DatabaseCheckLevel::full); ++checks[static_cast<std::size_t>(kind)];
  };
  { auto library = take(lib::Library::open(root, config)); }
  { auto catalog = take(cat::Catalog::open(root, config)); EXPECT_FALSE(catalog.open_report().previous_session_unclosed); }
  EXPECT_EQ(checks, (std::array<int,3>{0,0,0}));
  EXPECT_EQ(sql.counts, checks);
  for (const auto* flag : {"open", "unknown"}) {
    session(root, flag); checks.fill(0); sql.counts.fill(0);
    config.mode = lib::Mode::read_only;
    { auto catalog = take(cat::Catalog::open(root, config)); EXPECT_TRUE(catalog.open_report().previous_session_unclosed); EXPECT_FALSE(catalog.open_report().warnings.empty()); }
    EXPECT_EQ(checks, (std::array<int,3>{0,0,0}));
    EXPECT_EQ(sql.counts, checks);
    config.mode = lib::Mode::read_write;
    { auto catalog = take(cat::Catalog::open(root, config)); EXPECT_TRUE(catalog.open_report().previous_session_unclosed); }
    EXPECT_EQ(checks, (std::array<int,3>{1,1,1}));
    EXPECT_EQ(sql.counts, checks);
  }
  {
    auto library = take(lib::Library::open(root, options()));
    auto connection = am::db::Connection::borrow(library.database(Kind::index));
    take(connection.exec("DELETE FROM meta WHERE key='writer_session'"));
  }
  checks.fill(0); sql.counts.fill(0);
  { auto catalog = take(cat::Catalog::open(root, config)); }
  EXPECT_EQ(checks, (std::array<int,3>{1,1,1}));
  EXPECT_EQ(sql.counts, checks);
}
TEST(DatabaseChecks, VerifyReportsAllDatabasesAtBothLevels) {
  RecordProperty("req", "K-22");
  const auto root = fresh(); auto catalog = take(cat::Catalog::open(root, options()));
  for (const auto level : {cat::LibraryVerifyLevel::quick, cat::LibraryVerifyLevel::full}) {
    cat::LibraryVerifyOptions config; config.level = level;
    const auto report = take(catalog.verify(config)); ASSERT_EQ(report.databases.size(), 3u);
    EXPECT_FALSE(report.database_failed);
    for (const auto& database : report.databases) {
      EXPECT_EQ(database.status, "ok");
      EXPECT_EQ(database.level, level == cat::LibraryVerifyLevel::full ? lib::DatabaseCheckLevel::full : lib::DatabaseCheckLevel::quick);
    }
  }
}
TEST(DatabaseChecks, DamagedDataPageIsDeferredAndRuntimeErrorsAreCorruptData) {
  RecordProperty("req", "K-112|K-22|K-24");
  for (const auto kind : {Kind::state, Kind::userdata}) {
    const auto root = fresh(); damage(root, kind);
    {
      auto library = take(lib::Library::open(root, options()));
      auto connection = am::db::Connection::borrow(library.database(kind));
      auto query = connection.prepare("SELECT * FROM corruption_probe");
      if (query) {
        auto row = query->step(); ASSERT_FALSE(row); EXPECT_EQ(row.error().code, am::Errc::corrupt_data);
      } else EXPECT_EQ(query.error().code, am::Errc::corrupt_data);
      EXPECT_EQ(take(library.check_database(kind, lib::DatabaseCheckLevel::full)).status, "corrupt");
    }
    auto verified = take(am::application::verify_library(root, false, true, false));
    EXPECT_EQ(verified.status, am::application::ExitCode::check_failed);
    EXPECT_EQ(verified.value["databases"].size(), 3u);
    const char* name = kind == Kind::state ? "state" : "userdata";
    EXPECT_EQ(verified.value["databases"][name]["status"], "corrupt");
    session(root, "open");
    auto opened = cat::Catalog::open(root, options()); ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().message_id, "am.library.db_error");
  }
}
TEST(DatabaseChecks, VerifyMarksDamagedIndexAndNextWriterRebuildsIt) {
  RecordProperty("req", "K-22|K-24");
  const auto root = fresh(); damage(root, Kind::index);
  {
    auto catalog = take(cat::Catalog::open(root, options()));
    const auto report = take(catalog.verify());
    ASSERT_EQ(report.databases.size(), 3u); EXPECT_EQ(report.databases.front().status, "corrupt");
    EXPECT_FALSE(report.database_failed); EXPECT_TRUE(take(catalog.indexer().rebuild_required()));
    EXPECT_FALSE(report.warnings.empty());
  }
  {
    auto catalog = take(cat::Catalog::open(root, options()));
    EXPECT_EQ(take(catalog.library().check_database(Kind::index, lib::DatabaseCheckLevel::full)).status, "ok");
    EXPECT_FALSE(take(catalog.indexer().rebuild_required()));
  }
}
TEST(DatabaseChecks, OldIndexIsUntouchedReadOnlyAndRebuiltFromZipByWriter) {
  RecordProperty("req", "K-112|K-24");
  const auto root = fresh(); std::string id;
  {
    auto catalog = take(cat::Catalog::open(root, options()));
    am::repository::CreateRequest request; request.metadata = am::repository::Json{{"title","Preserved asset"}};
    auto created = take(catalog.create(request)); id = created.change.ref.asset_id;
  }
  {
    auto library = take(lib::Library::open(root, options()));
    auto connection = am::db::Connection::borrow(library.database(Kind::index));
    take(connection.exec("DROP TABLE fts_names_owner; DROP TABLE fts_docs_owner; PRAGMA user_version=1"));
  }
  const auto index = root / ".am/index.sqlite"; const auto before = bytes(index);
  const auto zip = root / "assets" / id.substr(34) / (id + ".zip"); const auto zip_before = bytes(zip);
  {
    auto catalog = take(cat::Catalog::open(root, options(lib::Mode::read_only)));
    EXPECT_EQ(catalog.library().database_info(Kind::index).state, lib::DatabaseState::incompatible);
    EXPECT_FALSE(catalog.open_report().warnings.empty());
  }
  EXPECT_EQ(bytes(index), before);
  {
    auto catalog = take(cat::Catalog::open(root, options()));
    EXPECT_EQ(catalog.library().database_info(Kind::index).user_version, 2);
    EXPECT_EQ(take(catalog.indexer().count()), 1); EXPECT_FALSE(take(catalog.indexer().rebuild_required()));
  }
  EXPECT_EQ(bytes(zip), zip_before);
}

TEST(DatabaseChecks, RuntimeIndexCorruptionMarksRebuildAndUnclosedOpenRepairsDamage) {
  RecordProperty("req", "K-22|K-24");
  for (const bool unclosed : {false, true}) {
    const auto root = fresh();
    if (unclosed) session(root, "open");
    damage(root, Kind::index);
    if (!unclosed) {
      auto library = take(lib::Library::open(root, options()));
      auto connection = am::db::Connection::borrow(library.database(Kind::index));
      auto query = connection.prepare("SELECT * FROM corruption_probe");
      if (query) {
        auto row = query->step(); ASSERT_FALSE(row); EXPECT_EQ(row.error().code, am::Errc::corrupt_data);
      } else EXPECT_EQ(query.error().code, am::Errc::corrupt_data);
      am::db::IndexStore store(library.database(Kind::index)); EXPECT_TRUE(take(store.rebuild_required()));
    }
    auto catalog = take(cat::Catalog::open(root, options()));
    EXPECT_EQ(take(catalog.library().check_database(Kind::index, lib::DatabaseCheckLevel::full)).status, "ok");
    EXPECT_FALSE(take(catalog.indexer().rebuild_required()));
  }
}
