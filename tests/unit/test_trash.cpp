#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <stdexcept>

#include "am/application/library_service.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/storage/asset_zip/writer.hpp"

namespace {
namespace fs = std::filesystem;
namespace cat = am::catalog;
namespace lib = am::library;
namespace repo = am::repository;
namespace commit = am::commit;
namespace db = am::db;
template <class T>
T value(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(r).value();
}
void value(am::Result<void> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
}
std::string bytes(const fs::path& p) {
  std::ifstream f(am::paths::native(p), std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
void write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(am::paths::native(p), std::ios::binary);
  f << s;
  if (!f) throw std::runtime_error("Test write failed");
}
std::string hash(const fs::path& p) {
  const auto s = bytes(p);
  auto sha = value(am::asset_zip::Sha256::create());
  value(sha.update(std::as_bytes(std::span(s))));
  const auto digest = value(sha.finish());
  return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}
lib::OpenOptions options(lib::Mode mode = lib::Mode::read_write) {
  lib::OpenOptions out;
  out.mode = mode;
  out.capabilities.emplace();
  out.capabilities->mode = repo::StorageMode::full;
  return out;
}
fs::path fresh() {
  const auto parent = fs::path(AM_LIBRARY_TEST_ROOT);
  fs::create_directories(parent);
  auto root = parent / am::Uuid::v7().to_string();
  value(lib::Library::init(root));
  return root;
}
fs::path active(const fs::path& root, const std::string& id) { return root / "assets" / id.substr(34) / (id + ".zip"); }
fs::path trashed(const fs::path& root, const std::string& id) { return root / "trash" / (id + ".zip"); }
repo::CreateRequest request(const fs::path& root, const std::string& title = "Trash test") {
  repo::CreateRequest out;
  out.metadata = repo::Json{{"title", title}};
  repo::AddFiles add;
  add.input = {{"id", am::Uuid::v4().to_string()},
               {"imported_at", "2026-11-16T00:00:00.000Z"},
               {"mode", "strict"},
               {"completeness", "complete"},
               {"objects", repo::Json::array()},
               {"entries", repo::Json::array()}};
  repo::FileInput file;
  file.path = "trash-payload.bin";
  file.source = root / (am::Uuid::v4().to_string() + ".bin");
  write(file.source, "synthetic trash payload");
  add.files.push_back(std::move(file));
  out.files.push_back(std::move(add));
  return out;
}
repo::RepositoryOptions raw_options(const lib::Library& library) {
  repo::RepositoryOptions out;
  out.library_id = library.config().library_id;
  out.capabilities.emplace();
  out.capabilities->mode = repo::StorageMode::full;
  return out;
}
struct World {
  fs::path root = fresh();
  cat::Catalog catalog;
  explicit World(cat::IndexerHooks hooks = {}) : catalog(value(cat::Catalog::open(root, options(), std::move(hooks)))) {
    value(catalog.indexer().rebuild());
  }
  repo::AssetRef create(const std::string& title = "Trash test") {
    return value(catalog.create(request(root, title))).change.ref;
  }
  std::shared_ptr<db::StateJournal> journal() { return value(db::StateJournal::open(root)); }
};
am::Result<repo::LifecycleResult> operate(repo::AssetRepository& r, std::string_view kind, const repo::AssetRef& ref) {
  return kind == "trash" ? r.trash(ref) : kind == "restore" ? r.restore(ref) : r.purge(ref);
}
am::Result<cat::CatalogLifecycle> operate(cat::Catalog& r, std::string_view kind, const repo::AssetRef& ref) {
  return kind == "trash" ? r.trash(ref) : kind == "restore" ? r.restore(ref) : r.purge(ref);
}
}  // namespace

TEST(Trash, MoveRestorePreserveBytesRevisionAndOpenSnapshotAndUpdateIndex) {
  RecordProperty("req", "K-3|ADR-0007|ADR-0025");
  World w;
  const auto ref = w.create();
  const auto before = hash(active(w.root, ref.asset_id));
  auto snapshot = value(w.catalog.library().assets().snapshot(ref));
  EXPECT_EQ(snapshot.revision(), 1u);
  EXPECT_TRUE(value(w.catalog.trash(ref)).warnings.empty());
  EXPECT_FALSE(fs::exists(active(w.root, ref.asset_id)));
  EXPECT_EQ(hash(trashed(w.root, ref.asset_id)), before);
  EXPECT_TRUE(value(w.catalog.indexer().list()).empty());
  const auto all = value(w.catalog.indexer().list(true));
  ASSERT_EQ(all.size(), 1u);
  EXPECT_TRUE(all[0].in_trash);
  EXPECT_EQ(all[0].revision, 1);
  EXPECT_EQ(snapshot.revision(), 1u);
  EXPECT_EQ(snapshot.manifest().known()["history"]["entries"].size(), 1u);
  am::asset_zip::ReadBudget budget(UINT64_MAX);
  for (const auto& entry : snapshot.archive().entries()) value(snapshot.archive().verify_entry(entry.index, budget));
  EXPECT_TRUE(value(w.catalog.restore(ref)).warnings.empty());
  EXPECT_EQ(hash(active(w.root, ref.asset_id)), before);
  EXPECT_EQ(value(w.catalog.library().assets().snapshot(ref)).revision(), 1u);
  EXPECT_FALSE(value(w.catalog.indexer().list())[0].in_trash);
  EXPECT_TRUE(value(w.journal()->list()).empty());
}
TEST(Trash, PurgeRequiresTrashRemovesIndexAndMissingIsPerAsset) {
  RecordProperty("req", "K-3|ADR-0025");
  World w;
  const auto ref = w.create();
  auto refused = w.catalog.purge(ref);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().message_id, "am.repo.not_in_trash");
  EXPECT_TRUE(fs::exists(active(w.root, ref.asset_id)));
  value(w.catalog.trash(ref));
  value(w.catalog.purge(ref));
  EXPECT_FALSE(fs::exists(trashed(w.root, ref.asset_id)));
  EXPECT_EQ(value(w.catalog.indexer().count()), 0);
  refused = w.catalog.purge(ref);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, am::Errc::not_found);
  EXPECT_EQ(refused.error().message_id, "am.repo.asset_not_found");
  EXPECT_TRUE(value(w.journal()->list()).empty());
}
TEST(Trash, ConflictsNeverReplaceEitherFile) {
  RecordProperty("req", "K-3|ADR-0006");
  for (const auto* kind : {"trash", "restore"}) {
    World w;
    const auto ref = w.create();
    if (std::string_view(kind) == "restore") value(w.catalog.trash(ref));
    const auto source =
        std::string_view(kind) == "trash" ? active(w.root, ref.asset_id) : trashed(w.root, ref.asset_id);
    const auto target =
        std::string_view(kind) == "trash" ? trashed(w.root, ref.asset_id) : active(w.root, ref.asset_id);
    write(target, "different destination bytes");
    const auto before_source = hash(source), before_target = hash(target);
    auto result = operate(w.catalog, kind, ref);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, am::Errc::conflict);
    EXPECT_EQ(result.error().message_id, std::string("am.repo.") + kind + "_conflict");
    EXPECT_EQ(hash(source), before_source);
    EXPECT_EQ(hash(target), before_target);
    EXPECT_TRUE(value(w.journal()->list()).empty());
    fs::remove(target);
    auto opts = raw_options(w.catalog.library());
    opts.test_hooks.commit.point = [&](commit::FaultPoint p) -> am::Result<void> {
      if (p == commit::FaultPoint::before_replace) write(target, "racing destination");
      return {};
    };
    auto raw = value(repo::AssetRepository::open(w.root, opts));
    auto raced = operate(raw, kind, ref);
    ASSERT_FALSE(raced);
    EXPECT_EQ(raced.error().code, am::Errc::conflict);
    EXPECT_EQ(hash(source), before_source);
    EXPECT_EQ(bytes(target), "racing destination");
    EXPECT_TRUE(value(w.journal()->list()).empty());
  }
}
TEST(Trash, ExternalStatChangeAbortsEachOperationAndRemovesPreparedRow) {
  RecordProperty("req", "K-3|ADR-0006");
  for (const auto* kind : {"trash", "restore", "purge"}) {
    World w;
    const auto ref = w.create();
    if (std::string_view(kind) != "trash") value(w.catalog.trash(ref));
    const auto source =
        std::string_view(kind) == "trash" ? active(w.root, ref.asset_id) : trashed(w.root, ref.asset_id);
    auto opts = raw_options(w.catalog.library());
    bool changed = false;
    opts.test_hooks.commit.point = [&](commit::FaultPoint point) -> am::Result<void> {
      if (point == commit::FaultPoint::before_replace) {
        write(source, "external change");
        changed = true;
      }
      return {};
    };
    auto raw = value(repo::AssetRepository::open(w.root, opts));
    auto result = operate(raw, kind, ref);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, am::Errc::conflict);
    EXPECT_TRUE(changed);
    EXPECT_EQ(bytes(source), "external change");
    EXPECT_TRUE(value(w.journal()->list()).empty());
    opts.test_hooks.commit.point = [&](commit::FaultPoint point) -> am::Result<void> {
      if (point == commit::FaultPoint::before_replace) fs::remove(source);
      return {};
    };
    auto deleting = value(repo::AssetRepository::open(w.root, opts));
    auto removed = operate(deleting, kind, ref);
    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().code, am::Errc::conflict);
    EXPECT_FALSE(fs::exists(source));
    EXPECT_TRUE(value(w.journal()->list()).empty());
  }
}
TEST(Trash, CorruptManifestDoesNotPreventLifecycleAndUsesRebuildPolicy) {
  RecordProperty("req", "K-3|ADR-0025|ADR-0007");
  World w;
  const auto ref = w.create();
  auto sink = std::make_shared<am::asset_zip::MemorySink>();
  auto writer = value(am::asset_zip::ZipWriter::create(sink));
  am::asset_zip::EntryOptions entry_options;
  entry_options.method = am::asset_zip::CompressionMethod::stored;
  entry_options.uncompressed_size = 1;
  auto entry = value(writer.begin_entry("manifest.json", entry_options));
  const std::string broken = "{";
  value(entry.write(std::as_bytes(std::span(broken))));
  value(entry.finish());
  value(writer.finish());
  write(active(w.root, ref.asset_id),
        std::string(reinterpret_cast<const char*>(sink->bytes().data()), sink->bytes().size()));
  const auto original = hash(active(w.root, ref.asset_id));
  auto moved = value(w.catalog.trash(ref));
  EXPECT_FALSE(moved.warnings.empty());
  EXPECT_TRUE(value(w.catalog.indexer().rebuild_required()));
  const auto rebuild = value(w.catalog.indexer().rebuild());
  EXPECT_EQ(rebuild.indexed, 0u);
  ASSERT_EQ(rebuild.skipped.size(), 1u);
  EXPECT_EQ(rebuild.skipped[0].path, "trash/" + ref.asset_id + ".zip");
  EXPECT_FALSE(value(w.catalog.restore(ref)).warnings.empty());
  EXPECT_EQ(hash(active(w.root, ref.asset_id)), original);
  value(w.catalog.trash(ref));
  EXPECT_EQ(hash(trashed(w.root, ref.asset_id)), original);
  value(w.catalog.purge(ref));
  EXPECT_FALSE(fs::exists(trashed(w.root, ref.asset_id)));
}
TEST(Trash, FaultsBeforeAndAfterActionRecoverEveryOperation) {
  RecordProperty("req", "K-3|ADR-0006|ADR-0025");
  for (const auto* kind : {"trash", "restore", "purge"}) {
    for (auto point :
         {commit::FaultPoint::after_prepared, commit::FaultPoint::before_replace, commit::FaultPoint::after_replace,
          commit::FaultPoint::after_committed_journal, commit::FaultPoint::after_indexed_journal}) {
      SCOPED_TRACE(std::string(kind) + ":" + std::string(commit::point_name(point)));
      World w;
      const auto ref = w.create();
      if (std::string_view(kind) != "trash") value(w.catalog.trash(ref));
      commit::LifecycleRequest req;
      req.library_root = w.root;
      req.asset_id = ref.asset_id;
      req.kind = kind;
      req.journal = w.journal();
      req.hooks.point = [point](commit::FaultPoint p) -> am::Result<void> {
        if (p == point) return am::Error::make(am::Errc::io_error, "Injected lifecycle fault").id("am.commit.io_error");
        return {};
      };
      ASSERT_FALSE(commit::commit_lifecycle(req));
      const bool acted = point != commit::FaultPoint::after_prepared && point != commit::FaultPoint::before_replace;
      EXPECT_EQ(value(w.journal()->list()).size(), acted ? 1u : 0u);
      auto recovered = value(w.catalog.library().assets().recover());
      EXPECT_EQ(recovered.size(), acted ? 1u : 0u);
      EXPECT_TRUE(value(w.journal()->list()).empty());
      const bool in_trash = std::string_view(kind) == "trash"     ? acted
                            : std::string_view(kind) == "restore" ? !acted
                                                                  : !acted;
      if (std::string_view(kind) == "purge" && acted) {
        EXPECT_EQ(value(w.catalog.indexer().count()), 0);
      } else {
        const auto rows = value(w.catalog.indexer().list(true));
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0].in_trash, in_trash);
        EXPECT_TRUE(fs::exists(in_trash ? trashed(w.root, ref.asset_id) : active(w.root, ref.asset_id)));
      }
    }
  }
}
TEST(Trash, RecoveryExplicitPresenceMatrixAndFailedRowsRemain) {
  RecordProperty("req", "K-3|ADR-0006");
  for (const auto* kind : {"trash", "restore"})
    for (int presence = 0; presence < 4; ++presence)
      for (auto state : {db::CommitState::prepared, db::CommitState::committed, db::CommitState::indexed}) {
        SCOPED_TRACE(std::string(kind) + ":" + std::to_string(presence) + ":" + std::string(db::state_name(state)));
        World w;
        const auto id = am::Uuid::v4().to_string();
        db::PendingOperation row;
        row.asset_id = id;
        row.op_id = am::Uuid::v4().to_string();
        row.kind = kind;
        row.temp_path = std::string_view(kind) == "trash" ? active(w.root, id) : trashed(w.root, id);
        row.target_path = std::string_view(kind) == "trash" ? trashed(w.root, id) : active(w.root, id);
        if (presence & 1) write(row.temp_path, "source bytes");
        if (presence & 2) write(row.target_path, "target bytes");
        auto journal = w.journal();
        value(journal->insert(row));
        if (state != db::CommitState::prepared)
          value(journal->advance(row.op_id, db::CommitState::prepared, db::CommitState::committed));
        if (state == db::CommitState::indexed) value(journal->advance(row.op_id, db::CommitState::committed, state));
        const auto result = value(w.catalog.library().assets().recover());
        ASSERT_EQ(result.size(), 1u);
        if (presence == 1) {
          EXPECT_EQ(result[0].outcome, commit::RecoveryOutcome::rolled_back);
        } else if (presence == 2) {
          EXPECT_EQ(result[0].outcome, commit::RecoveryOutcome::completed);
        } else {
          EXPECT_EQ(result[0].outcome, commit::RecoveryOutcome::failed);
          auto stored = value(journal->find(row.op_id));
          ASSERT_TRUE(stored);
          EXPECT_EQ(stored->state, db::CommitState::failed);
          EXPECT_EQ(value(w.catalog.library().assets().recover())[0].outcome, commit::RecoveryOutcome::failed);
          EXPECT_EQ(value(journal->list()).size(), 1u);
        }
        if (presence == 1 || presence == 2) {
          EXPECT_TRUE(value(journal->list()).empty());
        }
        if (presence & 1) {
          EXPECT_EQ(bytes(row.temp_path), "source bytes");
        }
        if (presence & 2) {
          EXPECT_EQ(bytes(row.target_path), "target bytes");
        }
      }
}
TEST(Trash, PurgeRecoveryRollsBackExistingFileAndCompletesAbsentFile) {
  RecordProperty("req", "K-3|ADR-0006");
  for (bool exists : {false, true})
    for (auto state : {db::CommitState::prepared, db::CommitState::committed, db::CommitState::indexed}) {
      World w;
      const auto ref = w.create();
      value(w.catalog.trash(ref));
      db::PendingOperation row;
      row.asset_id = ref.asset_id;
      row.op_id = am::Uuid::v4().to_string();
      row.kind = "purge";
      row.target_path = trashed(w.root, ref.asset_id);
      const auto before = hash(row.target_path);
      if (!exists) fs::remove(row.target_path);
      auto journal = w.journal();
      value(journal->insert(row));
      auto c = db::Connection::borrow(w.catalog.library().database(lib::DatabaseKind::state));
      auto query = value(c.prepare("SELECT temp_path IS NULL FROM pending_ops WHERE op_id=?"));
      value(query.bind(1, row.op_id));
      value(query.step());
      EXPECT_EQ(query.integer(0), 1);
      if (state != db::CommitState::prepared)
        value(journal->advance(row.op_id, db::CommitState::prepared, db::CommitState::committed));
      if (state == db::CommitState::indexed) value(journal->advance(row.op_id, db::CommitState::committed, state));
      const auto result = value(w.catalog.library().assets().recover());
      ASSERT_EQ(result.size(), 1u);
      SCOPED_TRACE(result[0].error ? am::to_log_string(*result[0].error) : result[0].detail);
      EXPECT_EQ(result[0].outcome, exists ? commit::RecoveryOutcome::rolled_back : commit::RecoveryOutcome::completed);
      if (exists) {
        EXPECT_EQ(hash(row.target_path), before);
      } else {
        EXPECT_EQ(value(w.catalog.indexer().count()), 0);
      }
      EXPECT_TRUE(value(journal->list()).empty());
    }
}
TEST(Trash, ObserverFailureKeepsMoveAndRebuildRepairsIndex) {
  RecordProperty("req", "K-3|ADR-0007|ADR-0025");
  bool fail = false;
  cat::IndexerHooks hooks;
  hooks.before_update = [&](std::string_view) -> am::Result<void> {
    if (fail) return am::Error::make(am::Errc::io_error, "Injected index fault").id("am.catalog.error");
    return {};
  };
  World w(hooks);
  const auto ref = w.create();
  fail = true;
  auto moved = value(w.catalog.trash(ref));
  ASSERT_FALSE(moved.warnings.empty());
  EXPECT_TRUE(fs::exists(trashed(w.root, ref.asset_id)));
  EXPECT_TRUE(value(w.catalog.indexer().rebuild_required()));
  EXPECT_FALSE(w.catalog.sync());
  value(w.catalog.indexer().rebuild());
  EXPECT_TRUE(value(w.catalog.indexer().list(true))[0].in_trash);
  EXPECT_EQ(value(w.catalog.sync()).unchanged, 1u);
}
TEST(Trash, UserdataMainFileSha256RemainsIdentical) {
  RecordProperty("req", "K-3|ADR-0007");
  const auto root = fresh();
  {
    auto l = value(lib::Library::open(root, options()));
    auto user = db::Connection::borrow(l.database(lib::DatabaseKind::userdata));
    value(user.exec("CREATE TABLE sentinel(n); INSERT INTO sentinel VALUES('preserved')"));
  }
  const auto before = hash(root / ".am/userdata.sqlite");
  {
    auto catalog = value(cat::Catalog::open(root, options()));
    const auto ref = value(catalog.create(request(root))).change.ref;
    value(catalog.trash(ref));
    value(catalog.restore(ref));
    value(catalog.trash(ref));
    value(catalog.purge(ref));
  }
  EXPECT_EQ(hash(root / ".am/userdata.sqlite"), before);
}
TEST(Trash, ChangeInTrashFailsWithoutTouchingBytes) {
  RecordProperty("req", "K-3|ADR-0025");
  World w;
  const auto ref = w.create();
  value(w.catalog.trash(ref));
  const auto before = hash(trashed(w.root, ref.asset_id));
  repo::ChangeRequest change;
  change.ref = ref;
  change.expected_rev = 1;
  repo::SetMetadata metadata;
  metadata.field = "title";
  metadata.value.emplace("Changed");
  change.ops.push_back(std::move(metadata));
  auto result = w.catalog.change(change);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, am::Errc::not_found);
  EXPECT_EQ(hash(trashed(w.root, ref.asset_id)), before);
}
TEST(Trash, SummaryAndInfoUseExactUint64AndReportOverflowOrRequiredRebuild) {
  RecordProperty("req", "K-3|ADR-0037|ADR-0007");
  EXPECT_EQ(value(db::summarize_trash({})).bytes, "0");
  EXPECT_EQ(value(db::summarize_trash({"18446744073709551615", "0"})).bytes, "18446744073709551615");
  EXPECT_FALSE(value(db::summarize_trash({"18446744073709551615", "1"})).bytes);
  EXPECT_FALSE(value(db::summarize_trash({"18446744073709551615", "18446744073709551615"})).bytes);
  EXPECT_FALSE(db::summarize_trash({"invalid"}));
  World w;
  const auto a = w.create("A"), b = w.create("B");
  value(w.catalog.trash(a));
  value(w.catalog.trash(b));
  const auto total = fs::file_size(trashed(w.root, a.asset_id)) + fs::file_size(trashed(w.root, b.asset_id));
  auto info = value(am::application::inspect_library(w.root));
  EXPECT_EQ(info.value["trash"]["assets"], 2);
  EXPECT_EQ(info.value["trash"]["bytes"], std::to_string(total));
  auto index = db::Connection::borrow(w.catalog.library().database(lib::DatabaseKind::index));
  value(index.exec("UPDATE assets SET zip_size='18446744073709551615' WHERE in_trash=1"));
  info = value(am::application::inspect_library(w.root));
  EXPECT_TRUE(info.value["trash"]["bytes"].is_null());
  ASSERT_FALSE(info.diagnostics.empty());
  EXPECT_EQ(info.diagnostics[0].message_id, "am.catalog.trash_size_overflow");
  value(w.catalog.indexer().mark_rebuild_required());
  info = value(am::application::inspect_library(w.root));
  EXPECT_TRUE(info.value["trash"].is_null());
  ASSERT_FALSE(info.diagnostics.empty());
  EXPECT_EQ(info.diagnostics[0].message_id, "am.catalog.rebuild_required");
}
TEST(Trash, AssetLockReadOnlyAndInvalidReferencesRefuseBeforeMutation) {
  RecordProperty("req", "K-3|ADR-0025");
  World w;
  const auto ref = w.create();
  const auto before = hash(active(w.root, ref.asset_id));
  {
    auto lock = value(commit::AssetLock::acquire(w.root, ref.asset_id));
    auto result = w.catalog.trash(ref);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, am::Errc::locked);
  }
  auto raw = raw_options(w.catalog.library());
  raw.mode = repo::LibraryMode::read_only;
  auto reader = value(repo::AssetRepository::open(w.root, raw));
  auto refused = reader.trash(ref);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, am::Errc::read_only);
  auto invalid = ref;
  invalid.library_id = "another-library";
  EXPECT_FALSE(w.catalog.trash(invalid));
  EXPECT_EQ(hash(active(w.root, ref.asset_id)), before);
  EXPECT_TRUE(value(w.journal()->list()).empty());
}
