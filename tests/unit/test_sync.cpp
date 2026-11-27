#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/catalog/indexer.hpp"

namespace {
namespace fs = std::filesystem;
namespace cat = am::catalog;
namespace lib = am::library;
namespace repo = am::repository;
template <class T>
T value(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(r).value();
}
void value(am::Result<void> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
}
void write(const fs::path& p, const std::string& data) {
  std::ofstream out(am::paths::native(p), std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!out) throw std::runtime_error("Test write failed");
}
lib::OpenOptions options(lib::Mode mode = lib::Mode::read_write) {
  lib::OpenOptions out;
  out.mode = mode;
  out.capabilities.emplace();
  out.capabilities->mode = repo::StorageMode::full;
  return out;
}
fs::path fresh() {
  auto root = fs::path(AM_LIBRARY_TEST_ROOT) / am::Uuid::v7().to_string();
  value(lib::Library::init(root));
  return root;
}
fs::path path(const fs::path& root, const std::string& id) { return root / "assets" / id.substr(34) / (id + ".zip"); }
std::string scalar(lib::Library& l, const std::string& sql) {
  auto c = am::db::Connection::borrow(l.database(lib::DatabaseKind::index));
  auto statement = value(c.prepare(sql));
  value(statement.step());
  return value(statement.text(0));
}
using Rows = std::map<std::string, std::vector<std::string>>;
// Logical rows include every column, SQL type, and NULL, but no FTS shadow rowids.
Rows snapshot(lib::Library& library, const std::set<std::string>& exclude = {}, bool meta = false) {
  Rows out;
  auto* db = library.database(lib::DatabaseKind::index);
  for (const auto* table : {"assets", "files", "input_entries", "input_objects", "sources", "asset_tags",
                            "asset_categories", "asset_fields", "image_hashes", "mesh_features", "asset_minhash",
                            "fts_assets", "fts_names", "fts_docs", "suggestions", "meta"}) {
    if (std::string_view(table) == "meta" && !meta) continue;
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, ("SELECT * FROM " + std::string(table)).c_str(), -1, &raw, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db));
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
    auto& rows = out[table];
    int rc = 0;
    while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
      std::string row;
      bool skip = false;
      for (int i = 0; i < sqlite3_column_count(raw); ++i) {
        const auto type = sqlite3_column_type(raw, i);
        const auto* data = static_cast<const char*>(sqlite3_column_blob(raw, i));
        const auto size = sqlite3_column_bytes(raw, i);
        const std::string cell = data ? std::string(data, static_cast<std::size_t>(size)) : std::string{};
        if (std::string_view(sqlite3_column_name(raw, i)) == "asset_id" && exclude.contains(cell)) skip = true;
        row += std::to_string(type) + ":" + std::to_string(size) + ":" + cell + ";";
      }
      if (!skip) rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    std::sort(rows.begin(), rows.end());
  }
  return out;
}
struct World {
  fs::path root = fresh();
  lib::Library library = value(lib::Library::open(root, options()));
  std::size_t opens = 0;
  cat::IndexerHooks hooks;
  std::unique_ptr<cat::Indexer> index;
  World() { reset(); }
  void reset() {
    hooks.before_read = [this](const fs::path&) -> am::Result<void> {
      ++opens;
      return {};
    };
    index = std::make_unique<cat::Indexer>(root, library, hooks);
  }
  repo::ChangeResult create(const std::string& title) {
    repo::CreateRequest request;
    request.metadata = repo::Json{{"title", title}, {"tags", repo::Json::array({"sync-tag"})}};
    repo::AddFiles add;
    add.input = {{"id", am::Uuid::v4().to_string()},
                 {"imported_at", "2026-11-16T00:00:00.000Z"},
                 {"mode", "strict"},
                 {"completeness", "complete"},
                 {"objects", repo::Json::array()},
                 {"entries", repo::Json::array()}};
    repo::FileInput file;
    file.path = "sync-payload.bin";
    file.source = root / (am::Uuid::v4().to_string() + ".bin");
    write(file.source, "synthetic sync payload");
    add.files.push_back(std::move(file));
    request.files.push_back(std::move(add));
    return value(library.assets().create(request));
  }
  void title(const repo::ChangeResult& asset, const std::string& title) {
    repo::ChangeRequest request;
    request.ref = asset.ref;
    request.expected_rev = asset.revision;
    repo::SetMetadata set;
    set.field = "title";
    set.value.emplace(title);
    request.ops.push_back(std::move(set));
    value(library.assets().change(request));
  }
};
std::string install_fixture(const fs::path& root, const std::string& name) {
  repo::ContainerContents contents;
  repo::ContainerVerifyOptions o;
  o.streams = false;
  auto source = fs::path(AM_GOLDEN_FIXTURES) / name / "asset.zip";
  value(repo::verify_container(source, o, &contents));
  auto id = contents.document->known()["asset"]["id"].get<std::string>();
  auto dest = path(root, id);
  fs::create_directories(dest.parent_path());
  fs::copy_file(source, dest, fs::copy_options::overwrite_existing);
  return id;
}
}  // namespace

TEST(Sync, NewLibraryHasCompleteEmptyIndexAndSyncNeedsNoRebuild) {
  RecordProperty("req", "K-7|K-122");
  World w;
  EXPECT_FALSE(value(w.index->rebuild_required()));
  EXPECT_EQ(w.library.database_info(lib::DatabaseKind::index).rebuild_required, false);
  const auto report = value(w.index->sync());
  EXPECT_EQ(report.added, 0u);
  EXPECT_EQ(report.updated, 0u);
  EXPECT_EQ(report.unchanged, 0u);
  EXPECT_TRUE(report.missing.empty());
  EXPECT_TRUE(report.skipped.empty());
  EXPECT_EQ(w.opens, 0u);
  EXPECT_FALSE(value(w.index->rebuild_required()));
}

TEST(Sync, UnchangedDoesNotOpenZipAndFullRereads) {
  RecordProperty("req", "K-7|ADR-0007");
  World w;
  for (int i = 0; i < 3; ++i) w.create("Unchanged " + std::to_string(i));
  value(w.index->rebuild());
  const auto before = snapshot(w.library, {}, true);
  w.opens = 0;
  const auto report = value(w.index->sync());
  EXPECT_EQ(report.unchanged, 3u);
  EXPECT_EQ(report.added + report.updated, 0u);
  EXPECT_EQ(w.opens, 0u);
  EXPECT_EQ(snapshot(w.library, {}, true), before);
  EXPECT_FALSE(report.full);
  EXPECT_GE(report.duration_ms, 0);
  const auto full = value(w.index->sync({true}));
  EXPECT_TRUE(full.full);
  EXPECT_EQ(full.updated, 3u);
  EXPECT_EQ(full.unchanged, 0u);
  EXPECT_EQ(w.opens, 3u);
  EXPECT_EQ(snapshot(w.library, {}, true), before);
}
TEST(Sync, AddsExternalCopyUpdatesRevisionFtsAndTrashPath) {
  RecordProperty("req", "K-7|ADR-0007|ADR-0037");
  World w;
  const auto a = w.create("OriginalSyncTitle");
  value(w.index->rebuild());
  World external;
  const auto b = external.create("ExternalSyncTitle");
  const auto target = path(w.root, b.ref.asset_id);
  fs::create_directories(target.parent_path());
  fs::copy_file(path(external.root, b.ref.asset_id), target);
  w.title(a, "RevisedSyncTitle");
  auto report = value(w.index->sync());
  EXPECT_EQ(report.added, 1u);
  EXPECT_EQ(report.updated, 1u);
  EXPECT_EQ(scalar(w.library, "SELECT revision FROM assets WHERE asset_id='" + a.ref.asset_id + "'"), "2");
  EXPECT_EQ(scalar(w.library, "SELECT count(*) FROM fts_assets WHERE fts_assets MATCH 'RevisedSyncTitle'"), "1");
  EXPECT_EQ(scalar(w.library, "SELECT count(*) FROM fts_assets WHERE fts_assets MATCH 'OriginalSyncTitle'"), "0");
  fs::rename(target, w.root / "trash" / target.filename());
  report = value(w.index->sync());
  EXPECT_EQ(report.updated, 1u);
  EXPECT_EQ(report.unchanged, 1u);
  EXPECT_EQ(scalar(w.library, "SELECT rel_path FROM assets WHERE in_trash=1"), "trash/" + target.filename().string());
  EXPECT_EQ(value(w.index->list()).size(), 1u);
  EXPECT_EQ(value(w.index->list(true)).size(), 2u);
}
TEST(Sync, MissingRetainsAllOtherColumnsChildrenAndRestoresSameStat) {
  RecordProperty("req", "K-7|ADR-0031");
  World w;
  const auto a = w.create("MissingSyncTitle");
  value(w.index->rebuild());
  const auto before = snapshot(w.library);
  const auto file = path(w.root, a.ref.asset_id);
  const auto hidden = w.root / "saved.zip";
  fs::rename(file, hidden);
  const auto report = value(w.index->sync());
  EXPECT_EQ(report.missing, std::vector<std::string>{a.ref.asset_id});
  EXPECT_EQ(scalar(w.library, "SELECT health_detail FROM assets"), "am.catalog.asset_missing");
  auto marked = snapshot(w.library);
  EXPECT_EQ(scalar(w.library, "SELECT title FROM assets"), "MissingSyncTitle");
  EXPECT_EQ(scalar(w.library, "SELECT count(*) FROM files"), "1");
  marked.erase("assets");
  auto children = before;
  children.erase("assets");
  EXPECT_EQ(marked, children);
  const auto repeated = value(w.index->sync());
  EXPECT_TRUE(repeated.missing.empty());
  EXPECT_EQ(repeated.unchanged, 0u);
  fs::rename(hidden, file);
  w.opens = 0;
  EXPECT_EQ(value(w.index->sync()).updated, 1u);
  EXPECT_EQ(w.opens, 1u);
  EXPECT_EQ(snapshot(w.library), before);
}
TEST(Sync, UnreadableRetainsSummaryAndChildrenCachesStatAndRetriesNewFile) {
  RecordProperty("req", "K-7|ADR-0007");
  World w;
  const auto a = w.create("PreservedSyncTitle");
  value(w.index->rebuild());
  auto before = snapshot(w.library);
  before.erase("assets");
  write(path(w.root, a.ref.asset_id), "not a zip");
  auto report = value(w.index->sync());
  ASSERT_EQ(report.skipped.size(), 1u);
  EXPECT_EQ(report.updated, 1u);
  EXPECT_EQ(scalar(w.library, "SELECT health FROM assets"), "corrupt");
  EXPECT_EQ(scalar(w.library, "SELECT health_detail FROM assets"), report.skipped[0].message_id);
  EXPECT_EQ(scalar(w.library, "SELECT zip_size FROM assets"), "9");
  EXPECT_EQ(scalar(w.library, "SELECT title FROM assets"), "PreservedSyncTitle");
  auto after = snapshot(w.library);
  after.erase("assets");
  EXPECT_EQ(after, before);
  w.opens = 0;
  EXPECT_EQ(value(w.index->sync()).unchanged, 1u);
  EXPECT_EQ(w.opens, 0u);
  const auto unknown = am::Uuid::v4().to_string();
  const auto file = path(w.root, unknown);
  fs::create_directories(file.parent_path());
  write(file, "bad");
  for (int i = 0; i < 2; ++i) {
    w.opens = 0;
    EXPECT_EQ(value(w.index->sync()).skipped.size(), 1u);
    EXPECT_EQ(w.opens, 1u);
    EXPECT_EQ(value(w.index->count()), 1);
  }
}
TEST(Sync, SharedCandidateRulesDuplicatesIgnoredAndIdMismatch) {
  RecordProperty("req", "K-7|ADR-0007");
  World w;
  const auto a = w.create("Winner");
  value(w.index->rebuild());
  fs::copy_file(path(w.root, a.ref.asset_id), w.root / "trash" / (a.ref.asset_id + ".zip"));
  write(w.root / "assets/note.txt", "ignored");
  fs::create_directories(w.root / "trash/folder.zip");
  const auto other = am::Uuid::v4().to_string();
  const auto mismatch = path(w.root, other);
  fs::create_directories(mismatch.parent_path());
  fs::copy_file(path(w.root, a.ref.asset_id), mismatch);
  const auto sync = value(w.index->sync());
  const auto before = snapshot(w.library);
  const auto rebuild = value(w.index->rebuild());
  EXPECT_EQ(snapshot(w.library), before);
  ASSERT_EQ(sync.skipped.size(), 2u);
  ASSERT_EQ(rebuild.skipped.size(), sync.skipped.size());
  for (std::size_t i = 0; i < sync.skipped.size(); ++i) {
    EXPECT_EQ(sync.skipped[i].path, rebuild.skipped[i].path);
    EXPECT_EQ(sync.skipped[i].reason, rebuild.skipped[i].reason);
    EXPECT_EQ(sync.skipped[i].message_id, rebuild.skipped[i].message_id);
  }
  EXPECT_EQ(sync.skipped.front().reason, "id_mismatch");
  EXPECT_EQ(sync.skipped.back().reason, "duplicate_id");
  EXPECT_EQ(sync.ignored, rebuild.ignored);
  EXPECT_EQ(sync.ignored.size(), 2u);
  EXPECT_EQ(scalar(w.library, "SELECT in_trash FROM assets"), "0");
}
TEST(Sync, FullDetectsEqualSizeAndRestoredMtime) {
  RecordProperty("req", "K-7|ADR-0007");
  World w;
  const auto a = w.create("HiddenChange");
  value(w.index->rebuild());
  const auto file = path(w.root, a.ref.asset_id);
  const auto time = fs::last_write_time(file);
  const auto size = fs::file_size(file);
  write(file, std::string(static_cast<std::size_t>(size), 'x'));
  std::error_code ec;
  fs::last_write_time(file, time, ec);
  if (ec || fs::last_write_time(file) != time) GTEST_SKIP() << "Filesystem cannot restore mtime exactly";
  w.opens = 0;
  EXPECT_EQ(value(w.index->sync()).unchanged, 1u);
  EXPECT_EQ(w.opens, 0u);
  const auto full = value(w.index->sync({true}));
  EXPECT_EQ(w.opens, 1u);
  EXPECT_EQ(full.skipped.size(), 1u);
  EXPECT_EQ(scalar(w.library, "SELECT health FROM assets"), "corrupt");
}
TEST(Sync, CompleteSnapshotMatchesRebuildOnCopiedDiskExceptRetainedMarks) {
  RecordProperty("req", "K-7|ADR-0007|ADR-0037");
  World w;
  const auto changed = w.create("Before");
  const auto moved = w.create("Trash");
  const auto missing = w.create("Missing");
  const auto bad = w.create("Corrupted");
  install_fixture(w.root, "pos-full");
  // DrvFS may round timestamps when setting them. Use an exactly representable
  // fixture time so the copied disk has identical stat, including nanoseconds.
  const fs::file_time_type stamp = std::chrono::time_point_cast<std::chrono::seconds>(fs::file_time_type::clock::now());
  const auto normalize_times = [&] {
    for (const auto* dir : {"assets", "trash"})
      for (const auto& entry : fs::recursive_directory_iterator(w.root / dir))
        if (entry.is_regular_file()) fs::last_write_time(entry.path(), stamp);
  };
  normalize_times();
  value(w.index->rebuild());
  w.title(changed, "After");
  w.create("Added");
  fs::rename(path(w.root, moved.ref.asset_id), w.root / "trash" / (moved.ref.asset_id + ".zip"));
  fs::remove(path(w.root, missing.ref.asset_id));
  write(path(w.root, bad.ref.asset_id), "corrupted");
  fs::copy_file(path(w.root, changed.ref.asset_id), w.root / "trash" / (changed.ref.asset_id + ".zip"));
  normalize_times();
  const auto copy = fresh();
  for (const auto* dir : {"assets", "trash"}) {
    for (const auto& entry : fs::recursive_directory_iterator(w.root / dir)) {
      if (!entry.is_regular_file()) continue;
      const auto destination = copy / entry.path().lexically_relative(w.root);
      fs::create_directories(destination.parent_path());
      fs::copy_file(entry.path(), destination);
      fs::last_write_time(destination, fs::last_write_time(entry.path()));
      EXPECT_EQ(fs::last_write_time(destination), fs::last_write_time(entry.path()));
    }
  }
  const auto synced = value(w.index->sync());
  EXPECT_EQ(synced.added, 1u);
  EXPECT_EQ(synced.updated, 3u);
  EXPECT_EQ(synced.missing, std::vector<std::string>{missing.ref.asset_id});
  EXPECT_EQ(synced.skipped.size(), 2u);
  auto other = value(lib::Library::open(copy, options()));
  cat::Indexer rebuilt(copy, other);
  value(rebuilt.rebuild());
  const std::set<std::string> retained{missing.ref.asset_id, bad.ref.asset_id};
  EXPECT_EQ(snapshot(w.library, retained), snapshot(other, retained));
  EXPECT_EQ(scalar(w.library, "SELECT count(*) FROM files WHERE asset_id='" + bad.ref.asset_id + "'"), "1");
}
TEST(Sync, FaultRollsBackEveryChangeAndReadersSeeOldStateUntilCommit) {
  RecordProperty("req", "K-7|ADR-0007");
  World w;
  const auto missing = w.create("SoonMissing");
  value(w.index->rebuild());
  const auto before = snapshot(w.library, {}, true);
  w.create("New");
  fs::remove(path(w.root, missing.ref.asset_id));
  auto reader = value(lib::Library::open(w.root, options(lib::Mode::read_only)));
  std::size_t observed = 0;
  w.hooks.after_sync_change = [&](std::size_t n) -> am::Result<void> {
    ++observed;
    EXPECT_EQ(snapshot(reader, {}, true), before);
    if (n == 2) return am::Error::make(am::Errc::io_error, "Injected sync fault").id("am.catalog.error");
    return {};
  };
  w.reset();
  const auto failed = w.index->sync();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().code, am::Errc::io_error);
  EXPECT_EQ(observed, 2u);
  EXPECT_EQ(snapshot(w.library, {}, true), before);
  w.hooks.after_sync_change = {};
  w.reset();
  value(w.index->sync());
  EXPECT_NE(snapshot(reader, {}, true), before);
}
TEST(Sync, RefusesRebuildFlagReadOnlyAndUnavailableRootsButEmptyIsMissing) {
  RecordProperty("req", "K-7|ADR-0031");
  World w;
  value(w.index->mark_rebuild_required());
  auto before = snapshot(w.library, {}, true);
  auto refused = w.index->sync();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, am::Errc::incomplete);
  EXPECT_EQ(refused.error().message_id, "am.catalog.rebuild_required");
  EXPECT_EQ(snapshot(w.library, {}, true), before);
  const auto a = w.create("Available");
  value(w.index->rebuild());
  auto reader = value(lib::Library::open(w.root, options(lib::Mode::read_only)));
  cat::Indexer readonly(w.root, reader);
  auto ro = readonly.sync();
  ASSERT_FALSE(ro);
  EXPECT_EQ(ro.error().code, am::Errc::read_only);
  before = snapshot(w.library, {}, true);
  for (const auto* name : {"assets", "trash"}) {
    fs::rename(w.root / name, w.root / "hidden");
    refused = w.index->sync();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, am::Errc::io_error);
    EXPECT_EQ(snapshot(w.library, {}, true), before);
    fs::rename(w.root / "hidden", w.root / name);
  }
  fs::remove(path(w.root, a.ref.asset_id));
  EXPECT_EQ(value(w.index->sync()).missing.size(), 1u);
  EXPECT_FALSE(value(w.index->rebuild_required()));
}
TEST(Sync, NewerMinorSummaryWorksForRebuildUpdateAndSync) {
  RecordProperty("req", "K-3|K-6|K-7|ADR-0037");
  World w;
  const auto id = install_fixture(w.root, "pos-newer-minor");
  EXPECT_EQ(value(w.index->rebuild()).indexed, 1u);
  const auto check = [&] {
    EXPECT_EQ(scalar(w.library, "SELECT health FROM assets"), "warn");
    EXPECT_EQ(scalar(w.library, "SELECT health_detail FROM assets"), "am.catalog.newer_minor");
    EXPECT_EQ(scalar(w.library, "SELECT primary_preview IS NULL FROM assets"), "1");
    for (const auto* table : {"files", "input_entries", "input_objects", "sources", "asset_tags", "asset_categories",
                              "asset_fields", "fts_names"})
      EXPECT_EQ(scalar(w.library, "SELECT count(*) FROM " + std::string(table)), "0") << table;
    EXPECT_EQ(scalar(w.library, "SELECT count(*) FROM fts_assets"), "1");
  };
  check();
  value(w.index->update({w.library.config().library_id, id}));
  check();
  EXPECT_EQ(value(w.index->sync({true})).updated, 1u);
  check();
}
TEST(Sync, MapExplicitlyHandlesEveryCheckStatusAndRequiredNewerMinorType) {
  RecordProperty("req", "K-3|K-6|K-7|ADR-0037");
  repo::ContainerContents contents;
  repo::ContainerVerifyOptions options;
  options.streams = false;
  auto report =
      value(repo::verify_container(fs::path(AM_GOLDEN_FIXTURES) / "pos-newer-minor/asset.zip", options, &contents));
  const cat::Location location{"test.zip", "1", 0, false};
  const auto baseline = report;
  const auto map = [&](const repo::VerificationReport& r) {
    return cat::Indexer::map(*contents.document, contents.manifest_text, location, r);
  };
  for (auto status : {repo::CheckStatus::not_performed, repo::CheckStatus::passed, repo::CheckStatus::partial,
                      repo::CheckStatus::failed, repo::CheckStatus::not_applicable_newer_minor}) {
    report = baseline;
    report.v1 = status;
    EXPECT_EQ(bool(map(report)), status == repo::CheckStatus::passed);
    report = baseline;
    report.v2 = status;
    EXPECT_EQ(bool(map(report)),
              status == repo::CheckStatus::passed || status == repo::CheckStatus::not_applicable_newer_minor);
  }
  report = baseline;
  report.format_support = repo::FormatSupport::supported;
  EXPECT_FALSE(map(report));
  auto overflow = contents.document->known();
  overflow["files"] = repo::Json::array({{{"size_bytes", "18446744073709551615"}}, {{"size_bytes", "1"}}});
  const auto overflow_raw = overflow.dump();
  auto overflow_doc = value(am::manifest::parse_manifest(overflow_raw));
  const auto overflow_record = value(cat::Indexer::map(overflow_doc, overflow_raw, location, baseline));
  EXPECT_FALSE(overflow_record.summary.content_bytes);
  EXPECT_EQ(overflow_record.health_detail, "am.catalog.newer_minor");
  for (const auto* key : {"/asset/id", "/asset/revision", "/asset/created_at", "/asset/modified_at",
                          "/format/version/major", "/format/version/minor", "/files"}) {
    for (bool remove : {false, true}) {
      auto json = contents.document->known();
      const repo::Json::json_pointer ptr(key);
      if (remove)
        json[ptr.parent_pointer()].erase(ptr.back());
      else
        json[ptr] = nullptr;
      const auto raw = json.dump();
      auto doc = value(am::manifest::parse_manifest(raw));
      auto mapped = cat::Indexer::map(doc, raw, location, baseline);
      ASSERT_FALSE(mapped) << key << " remove=" << remove;
      EXPECT_EQ(mapped.error().message_id, "am.catalog.manifest_invalid");
    }
  }
}
