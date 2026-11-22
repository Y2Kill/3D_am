#include <gtest/gtest.h>
#include "am/storage/library/library.hpp"
#include "am/application/library_service.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include <sqlite3.h>
#include <fstream>
#include <map>
#include <regex>

namespace {
namespace lib = am::library;
namespace fs = std::filesystem;
using am::Result;
using lib::DatabaseKind;
using lib::DatabaseState;
template<class T> T value(Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(r).value();
}
std::string bytes(const fs::path& p) {
  std::ifstream f(am::paths::native(p), std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
void write(const fs::path& p, const std::string& text) {
  std::ofstream f(am::paths::native(p), std::ios::binary); f << text; f.close();
  if (!f) throw std::runtime_error("Test file write failed");
}
fs::path fresh() {
  auto parent = fs::path(AM_LIBRARY_TEST_ROOT); fs::create_directories(parent);
  auto result = parent / am::Uuid::v7().to_string(); return result;
}
fs::path initialized() { auto root = fresh(); (void)value(lib::Library::init(root)); return root; }
lib::OpenOptions options(lib::Mode mode = lib::Mode::read_write) {
  lib::OpenOptions o; o.mode = mode; o.application_version = "test-version";
  o.capabilities.emplace(); o.capabilities->mode = am::repository::StorageMode::full;
  o.capabilities->filesystem = "synthetic test capabilities"; return o;
}
struct Raw {
  sqlite3* db = nullptr;
  explicit Raw(const fs::path& path) {
    const auto u = path.u8string(); const std::string s(u.begin(), u.end());
    if (sqlite3_open(s.c_str(), &db) != SQLITE_OK) throw std::runtime_error("Test database open failed");
  }
  ~Raw() { sqlite3_close(db); }
  Raw(const Raw&) = delete;
  Raw& operator=(const Raw&) = delete;
  void exec(const char* s) { if (sqlite3_exec(db, s, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db)); }
};
std::string scalar(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> cleanup(stmt, sqlite3_finalize);
  const auto rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
  const auto* s = sqlite3_column_text(stmt, 0); return s ? reinterpret_cast<const char*>(s) : "";
}
using Snapshot = std::map<fs::path, std::pair<std::uintmax_t, std::string>>;
Snapshot snapshot(const fs::path& root, bool allow_sidecars) {
  Snapshot result;
  for (const auto& entry : fs::recursive_directory_iterator(root)) if (entry.is_regular_file()) {
    const auto relative = entry.path().lexically_relative(root);
    bool sidecar = false;
    for (const auto* n : {"index", "userdata", "state"}) for (const auto* extension : {"-wal", "-shm"})
      if (relative == fs::path(".am") / (std::string(n) + ".sqlite" + extension) && fs::exists(root / ".am" / (std::string(n) + ".sqlite"))) sidecar = true;
    if (allow_sidecars && sidecar) continue;
    const auto content = bytes(entry.path());
    auto sha = value(am::asset_zip::Sha256::create());
    if (!sha.update(std::as_bytes(std::span(content)))) throw std::runtime_error("Hash update failed");
    const auto digest = value(sha.finish());
    std::string hash;
    constexpr char hex[] = "0123456789abcdef";
    for (auto b : digest) { const auto v = std::to_integer<unsigned>(b); hash += hex[v >> 4]; hash += hex[v & 15]; }
    result.emplace(relative, std::pair{entry.file_size(), hash});
  }
  return result;
}
void replace(std::string& text, const std::string& from, const std::string& to) {
  const auto offset = text.find(from);
  if (offset == std::string::npos) throw std::runtime_error("Test replacement not found");
  text.replace(offset, from.size(), to);
}
}  // namespace
TEST(Library, InitLayoutConfigAndDatabasePragmas) {
  RecordProperty("req", "K-7|ADR-0007");
  auto root = fresh(); lib::InitOptions init; init.name = "Test library";
  auto c = value(lib::Library::init(root, init));
  ASSERT_TRUE(am::Uuid::parse_canonical(c.library_id)); EXPECT_EQ(am::Uuid::parse(c.library_id)->version(), 7);
  EXPECT_EQ(c.name, init.name); EXPECT_TRUE(std::regex_match(c.created_at, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)")));
  for (auto dir : {"assets", "trash", ".am", ".am/cache", ".am/sessions", ".am/.am-tmp", ".am/locks"}) EXPECT_TRUE(fs::is_directory(root / dir));
  EXPECT_TRUE(fs::is_empty(root / "assets")); EXPECT_FALSE(fs::exists(root / ".am/library.lock"));
  auto library = value(lib::Library::open(root, options()));
  EXPECT_EQ(library.config().library_id, c.library_id);
  for (auto kind : {DatabaseKind::index, DatabaseKind::userdata, DatabaseKind::state}) {
    EXPECT_EQ(library.database_info(kind).state, DatabaseState::ok);
    EXPECT_EQ(library.database_info(kind).user_version, kind == DatabaseKind::index ? 2 : 1);
    EXPECT_EQ(scalar(library.database(kind), "PRAGMA journal_mode"), "wal");
    EXPECT_EQ(scalar(library.database(kind), "PRAGMA foreign_keys"), "1");
    EXPECT_EQ(scalar(library.database(kind), "PRAGMA synchronous"), kind == DatabaseKind::index ? "1" : "2");
  }
  EXPECT_EQ(library.database_info(DatabaseKind::index).rebuild_required, true);
  auto repeat = lib::Library::init(root); ASSERT_FALSE(repeat); EXPECT_EQ(repeat.error().message_id, "am.library.not_empty");
  auto nonempty = fresh(); fs::create_directory(nonempty); write(nonempty / "existing.txt", "data");
  repeat = lib::Library::init(nonempty); ASSERT_FALSE(repeat); EXPECT_EQ(repeat.error().code, am::Errc::conflict);
  auto absent = lib::Library::open(nonempty, options()); ASSERT_FALSE(absent); EXPECT_EQ(absent.error().message_id, "am.library.not_found");
}
TEST(Library, ConfigValidationAndUnknownKeys) {
  RecordProperty("req", "K-7|ADR-0007");
  auto root = initialized(); auto original = bytes(root / "am-library.toml");
  auto check = [&](const std::string& text, const char* key, am::Errc code = am::Errc::corrupt_data) {
    write(root / "am-library.toml", text); auto result = lib::Library::open(root, options(lib::Mode::read_only));
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code, code); EXPECT_EQ(result.error().ctx.detail.find(key), 0u);
    EXPECT_FALSE(result.error().allows(am::ErrorAction::open_read_only));
  };
  auto modified = original; replace(modified, "am-library", "other"); check(modified, "format");
  modified = original; replace(modified, "library_id", "unknown_id"); check(modified, "library_id");
}
TEST(Library, ConfigTypesVersionsDatesAndUnknownKeys) {
  RecordProperty("req", "K-7|ADR-0007");
  auto root = initialized(); auto original = bytes(root / "am-library.toml");
  auto original_lib = value(lib::Library::open(root, options(lib::Mode::read_only)));
  const auto id = original_lib.config().library_id, date = original_lib.config().created_at;
  auto invalid_variant = id; invalid_variant[19] = '0';
  for (const auto& [from, to, key] : {std::tuple{id, std::string("invalid-uuid"), "library_id"},
        std::tuple{id, am::Uuid::v4().to_string(), "library_id"},
        std::tuple{id, invalid_variant, "library_id"},
        std::tuple{date, std::string("2026-02-30T00:00:00.000Z"), "created_at"},
        std::tuple{std::string("schema_version = 1"), std::string("schema_version = '1'"), "schema_version"}}) {
    auto modified = original; replace(modified, from, to); write(root / "am-library.toml", modified);
    auto r = lib::Library::open(root, options(lib::Mode::read_only)); ASSERT_FALSE(r); EXPECT_EQ(r.error().message_id, "am.library.config_invalid"); EXPECT_EQ(r.error().ctx.detail, key);
  }
  auto modified = original; replace(modified, "schema_version = 1", "schema_version = 2"); write(root / "am-library.toml", modified);
  auto r = lib::Library::open(root, options()); ASSERT_FALSE(r); EXPECT_EQ(r.error().code, am::Errc::unsupported); EXPECT_FALSE(r.error().allows(am::ErrorAction::open_read_only));
  write(root / "am-library.toml", original + "\nfuture_key = 'preserved'\n");
  const auto before = bytes(root / "am-library.toml");
  ASSERT_TRUE(lib::Library::open(root, options())); EXPECT_EQ(bytes(root / "am-library.toml"), before);
}
TEST(Library, InterruptedInitHasNoConfiguration) {
  RecordProperty("req", "ADR-0007");
  const auto root = fresh(); lib::InitOptions o;
  o.before_config = []() -> Result<void> { return am::Error::make(am::Errc::io_error, "Injected failure"); };
  EXPECT_FALSE(lib::Library::init(root, o)); EXPECT_FALSE(fs::exists(root / "am-library.toml"));
  auto r = lib::Library::open(root, options()); ASSERT_FALSE(r); EXPECT_EQ(r.error().code, am::Errc::not_found);
  auto again = lib::Library::init(root); ASSERT_FALSE(again); EXPECT_EQ(again.error().message_id, "am.library.not_empty");
}
TEST(Library, OwnerLockLifetimeSameProcessAndStaleDiagnostics) {
  RecordProperty("req", "K-123|ADR-0031");
  const auto root = initialized();
  write(root / ".am/library.lock", "{\"pid\":99999999,\"host\":\"old\"}");
  std::optional<lib::Library> first; first.emplace(value(lib::Library::open(root, options())));
  auto second = lib::Library::open(root, options()); ASSERT_FALSE(second);
  EXPECT_EQ(second.error().code, am::Errc::locked); EXPECT_TRUE(second.error().allows(am::ErrorAction::open_read_only));
  EXPECT_NE(second.error().ctx.detail.find("test-version"), std::string::npos);
  auto reader = value(lib::Library::open(root, options(lib::Mode::read_only))); EXPECT_TRUE(reader.owner().locked);
  first.reset(); EXPECT_TRUE(lib::Library::open(root, options()));
  auto free = value(lib::Library::open(root, options(lib::Mode::read_only))); EXPECT_FALSE(free.owner().locked);
}
TEST(Library, ReadOnlyPreservesDataAndRejectsMutation) {
  RecordProperty("req", "ADR-0031");
  const auto root = initialized(); const auto before = snapshot(root, true);
  {
    auto reader = value(lib::Library::open(root, options(lib::Mode::read_only)));
    EXPECT_EQ(scalar(reader.database(DatabaseKind::state), "SELECT count(*) FROM pending_ops"), "0");
    auto create = reader.assets().create({}); ASSERT_FALSE(create); EXPECT_EQ(create.error().code, am::Errc::read_only);
    EXPECT_EQ(sqlite3_exec(reader.database(DatabaseKind::userdata), "CREATE TABLE forbidden(n)", nullptr, nullptr, nullptr), SQLITE_READONLY);
    auto info = value(am::application::inspect_library(root)); EXPECT_EQ(info.value["owner"], "free");
  }
  EXPECT_EQ(snapshot(root, true), before); EXPECT_FALSE(fs::exists(root / ".am/library.lock"));
  fs::remove(root / ".am/index.sqlite"); fs::remove(root / ".am/userdata.sqlite"); fs::remove(root / ".am/state.sqlite");
  const auto missing_before = snapshot(root, false);
  {
    auto reader = value(lib::Library::open(root, options(lib::Mode::read_only)));
    for (auto k : {DatabaseKind::index, DatabaseKind::userdata, DatabaseKind::state}) EXPECT_EQ(reader.database_info(k).state, DatabaseState::missing);
  }
  EXPECT_EQ(snapshot(root, false), missing_before);
}
TEST(Library, ImmutableFallbackAndPendingWal) {
  RecordProperty("req", "ADR-0031|ADR-0007");
  auto root = fresh(); root = fs::path(root.native() + fs::path(u8" библиотека #%").native());
  (void)value(lib::Library::init(root));
  auto o = options(lib::Mode::read_only); o.capabilities->writable = false;
  const auto before = snapshot(root, false);
  {
    auto reader = value(lib::Library::open(root, o));
    for (auto k : {DatabaseKind::index, DatabaseKind::userdata, DatabaseKind::state}) {
      EXPECT_EQ(reader.database_info(k).state, DatabaseState::ok); EXPECT_EQ(reader.database_info(k).read_mode, "immutable");
      EXPECT_EQ(scalar(reader.database(k), "PRAGMA user_version"), k == DatabaseKind::index ? "2" : "1");
    }
  }
  EXPECT_EQ(snapshot(root, false), before);
  Raw db(root / ".am/userdata.sqlite"); db.exec("CREATE TABLE probe(n); INSERT INTO probe VALUES(7)");
  ASSERT_GT(fs::file_size(root / ".am/userdata.sqlite-wal"), 0u);
  const auto pending_before = snapshot(root, false);
  auto reader = value(lib::Library::open(root, o));
  EXPECT_EQ(reader.database_info(DatabaseKind::userdata).state, DatabaseState::unavailable);
  EXPECT_EQ(reader.database_info(DatabaseKind::userdata).reason, "wal_pending");
  EXPECT_EQ(reader.database(DatabaseKind::userdata), nullptr); EXPECT_EQ(snapshot(root, false), pending_before);
}
TEST(Library, WritablePolicyRefusesWithoutDowngrade) {
  RecordProperty("req", "ADR-0031");
  const auto root = initialized(); auto o = options(); o.capabilities->mode = am::repository::StorageMode::read_only_default;
  auto r = lib::Library::open(root, o); ASSERT_FALSE(r); EXPECT_EQ(r.error().message_id, "am.library.storage_read_only");
  EXPECT_TRUE(r.error().allows(am::ErrorAction::open_read_only)); EXPECT_FALSE(fs::exists(root / ".am/library.lock"));
  o = options(); o.capabilities->writable = false; r = lib::Library::open(root, o); ASSERT_FALSE(r);
  EXPECT_EQ(r.error().message_id, "am.library.not_writable"); EXPECT_FALSE(fs::exists(root / ".am/library.lock"));
}
TEST(Library, DatabaseVersionsAndIndexRecreation) {
  RecordProperty("req", "ADR-0007|ADR-0030");
  for (const auto* name : {"userdata", "state"}) {
    const auto root = initialized();
    { Raw db(root / ".am" / (std::string(name) + ".sqlite")); db.exec("PRAGMA user_version=2"); }
    auto r = lib::Library::open(root, options()); ASSERT_FALSE(r); EXPECT_EQ(r.error().message_id, "am.library.db_version"); EXPECT_TRUE(r.error().allows(am::ErrorAction::open_read_only));
    auto reader = value(lib::Library::open(root, options(lib::Mode::read_only)));
    EXPECT_EQ(reader.database_info(std::string(name) == "state" ? DatabaseKind::state : DatabaseKind::userdata).state, DatabaseState::newer);
  }
  for (const auto* variant : {"newer", "corrupt", "missing"}) {
    const auto root = initialized(); const auto index = root / ".am/index.sqlite";
    write(root / "assets/preserved", "asset data"); const auto data = bytes(root / ".am/userdata.sqlite");
    if (std::string(variant) == "newer") { Raw db(index); db.exec("PRAGMA user_version=3"); }
    else if (std::string(variant) == "corrupt") write(index, "corrupt database");
    else fs::remove(index);
    { auto ro = value(lib::Library::open(root, options(lib::Mode::read_only)));
      EXPECT_EQ(ro.database_info(DatabaseKind::index).state, std::string(variant) == "missing" ? DatabaseState::missing : DatabaseState::incompatible); }
    { auto rw = value(lib::Library::open(root, options()));
      EXPECT_EQ(rw.database_info(DatabaseKind::index).user_version, 2); EXPECT_EQ(rw.database_info(DatabaseKind::index).rebuild_required, true); }
    std::size_t stale = 0;
    for (const auto& e : fs::directory_iterator(root / ".am")) if (e.path().filename().string().starts_with("index.sqlite.stale-")) ++stale;
    EXPECT_EQ(stale > 0, std::string(variant) != "missing");
    EXPECT_EQ(bytes(root / "assets/preserved"), "asset data"); EXPECT_EQ(bytes(root / ".am/userdata.sqlite"), data);
  }
}
TEST(Library, MigrationBackupSuccessAndRollback) {
  RecordProperty("req", "ADR-0007");
  for (const bool fail : {false, true}) {
    const auto root = initialized();
    { Raw db(root / ".am/userdata.sqlite"); db.exec("CREATE TABLE probe(n); INSERT INTO probe VALUES(7)"); }
    auto o = options(); lib::Migration step;
    step.apply = [fail](sqlite3* db) -> Result<void> {
      if (sqlite3_exec(db, "UPDATE probe SET n=8; CREATE TABLE added(n)", nullptr, nullptr, nullptr) != SQLITE_OK) return am::Error::make(am::Errc::io_error, "Test SQL failed");
      if (fail) return am::Error::make(am::Errc::io_error, "Injected migration failure");
      return {};
    };
    o.migrations.push_back(step);
    { auto r = lib::Library::open(root, o); ASSERT_EQ(static_cast<bool>(r), !fail); }
    Raw db(root / ".am/userdata.sqlite");
    EXPECT_EQ(scalar(db.db, "PRAGMA user_version"), fail ? "1" : "2"); EXPECT_EQ(scalar(db.db, "SELECT n FROM probe"), fail ? "7" : "8");
    EXPECT_EQ(scalar(db.db, "SELECT count(*) FROM sqlite_schema WHERE name='added'"), fail ? "0" : "1");
    std::size_t count = 0;
    for (const auto& backup : fs::directory_iterator(root / ".am/backup")) {
      ++count; Raw saved(backup.path()); EXPECT_EQ(scalar(saved.db, "PRAGMA user_version"), "1"); EXPECT_EQ(scalar(saved.db, "SELECT n FROM probe"), "7");
    }
    EXPECT_EQ(count, 1u);
  }
}
TEST(Library, EmptyDatabasesInitializeButNonemptyVersionZeroIsRejected) {
  RecordProperty("req", "ADR-0007");
  for (auto kind : {DatabaseKind::userdata, DatabaseKind::state}) {
    const auto root = initialized();
    const auto file = root / ".am" / (kind == DatabaseKind::userdata ? "userdata.sqlite" : "state.sqlite");
    { Raw db(file); db.exec("PRAGMA user_version=0"); }
    const auto before = bytes(file);
    auto rejected = lib::Library::open(root, options()); ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().message_id, "am.library.db_version");
    EXPECT_FALSE(rejected.error().allows(am::ErrorAction::open_read_only)); EXPECT_EQ(bytes(file), before);
    fs::remove(file);
    { Raw empty(file); }
    auto opened = value(lib::Library::open(root, options()));
    EXPECT_EQ(opened.database_info(kind).state, DatabaseState::ok); EXPECT_EQ(opened.database_info(kind).user_version, kind == DatabaseKind::index ? 2 : 1);
  }
}
TEST(Library, IndexRecreationPreservesWalFileSet) {
  RecordProperty("req", "ADR-0007|ADR-0030");
  const auto root = initialized(); const auto path = root / ".am/index.sqlite";
  {
    Raw db(path); int persist = 1;
    ASSERT_EQ(sqlite3_file_control(db.db, "main", SQLITE_FCNTL_PERSIST_WAL, &persist), SQLITE_OK);
    db.exec("PRAGMA user_version=1");
  }
  ASSERT_TRUE(fs::exists(root / ".am/index.sqlite-wal"));
  const bool shm = fs::exists(root / ".am/index.sqlite-shm");
  auto opened = value(lib::Library::open(root, options()));
  std::vector<fs::path> mains;
  for (const auto& entry : fs::directory_iterator(root / ".am")) {
    const auto filename = entry.path().filename().string();
    if (filename.starts_with("index.sqlite.stale-") && !filename.ends_with("-wal") && !filename.ends_with("-shm")) mains.push_back(entry.path());
  }
  ASSERT_EQ(mains.size(), 1u);
  EXPECT_TRUE(fs::exists(fs::path(mains[0].native() + fs::path("-wal").native())));
  if (shm) {
    EXPECT_TRUE(fs::exists(fs::path(mains[0].native() + fs::path("-shm").native())));
  }
  EXPECT_EQ(opened.database_info(DatabaseKind::index).user_version, 2);
}
TEST(Library, JournalAndAssetRepositoryShareLibraryState) {
  RecordProperty("req", "ADR-0006|ADR-0007|ADR-0025|ADR-0031");
  const auto root = initialized(); auto writer = value(lib::Library::open(root, options()));
  auto journal = value(am::db::StateJournal::open(root)); EXPECT_TRUE(value(journal->list()).empty());
  EXPECT_EQ(scalar(writer.database(DatabaseKind::state), "SELECT count(*) FROM pending_ops_new"), "0");
  am::repository::CreateRequest request;
  request.metadata = am::repository::Json{{"title", "From Library"}};
  auto created = value(writer.assets().create(request)); EXPECT_EQ(created.ref.library_id, writer.config().library_id);
  EXPECT_TRUE(fs::exists(root / "assets" / created.ref.asset_id.substr(34) / (created.ref.asset_id + ".zip")));
  auto reader = value(lib::Library::open(root, options(lib::Mode::read_only)));
  auto asset = value(reader.assets().snapshot(created.ref)); EXPECT_EQ(asset.ref(), created.ref); EXPECT_EQ(asset.revision(), 1u);
  EXPECT_TRUE(value(journal->list()).empty());
}
