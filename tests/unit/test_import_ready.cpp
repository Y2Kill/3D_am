#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "am/application/library_service.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/catalog/indexer.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace cat = am::catalog;
namespace lib = am::library;
namespace repo = am::repository;
namespace commit = am::commit;
template <class T>
T value(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(r).value();
}
[[maybe_unused]] void value(am::Result<void> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
}
void write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream f(am::paths::native(p), std::ios::binary);
  f << text;
  if (!f) throw std::runtime_error("Fixture write failed");
}
}  // namespace

#ifdef AM_IMPORT_HELPER
int main(int argc, char** argv) {
  if (argc != 7) return 90;
  try {
    repo::RepositoryOptions options;
    options.library_id = argv[3];
    options.capabilities.emplace();
    options.capabilities->mode = repo::StorageMode::full;
    options.test_hooks.commit.point = [&](commit::FaultPoint point) -> am::Result<void> {
      if (commit::point_name(point) == argv[4]) {
        write(fs::path(argv[5]), "ready");
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return {};
    };
    auto repository = value(repo::AssetRepository::open(fs::path(argv[1]), options));
    repo::ImportReadyRequest request;
    request.source = fs::path(argv[2]);
    if (std::string_view(argv[6]) != "-") request.replace_expected = argv[6];
    value(repository.import_ready(request));
    return 0;
  } catch (...) {
    return 91;
  }
}
#else
#include <gtest/gtest.h>

#include "zip_builder.hpp"

namespace {
std::string bytes(const fs::path& p) {
  std::ifstream f(am::paths::native(p), std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
std::string hash(const std::string& s) {
  auto sha = value(am::asset_zip::Sha256::create());
  value(sha.update(std::as_bytes(std::span(s))));
  auto digest = value(sha.finish());
  std::string out;
  constexpr char hex[] = "0123456789abcdef";
  for (auto b : digest) {
    auto n = std::to_integer<unsigned>(b);
    out += hex[n >> 4];
    out += hex[n & 15];
  }
  return out;
}
std::string file_hash(const fs::path& p) { return hash(bytes(p)); }
lib::OpenOptions options() {
  lib::OpenOptions out;
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
fs::path target(const fs::path& root, const std::string& id) { return root / "assets" / id.substr(34) / (id + ".zip"); }
repo::RepositoryOptions raw_options(const lib::Library& l) {
  repo::RepositoryOptions out;
  out.library_id = l.config().library_id;
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
  repo::ChangeResult create(const std::string& title = "Ready source", const std::optional<std::string>& id = {}) {
    repo::CreateRequest r;
    r.asset_id = id;
    r.metadata = repo::Json{{"title", title}};
    repo::AddFiles add;
    add.input = {{"id", am::Uuid::v4().to_string()},
                 {"imported_at", "2026-11-16T00:00:00.000Z"},
                 {"mode", "strict"},
                 {"completeness", "complete"},
                 {"objects", repo::Json::array()},
                 {"entries", repo::Json::array()}};
    for (int i = 0; i < 2; ++i) {
      repo::FileInput f;
      f.path = "file-" + std::to_string(i) + ".bin";
      f.source = root / (am::Uuid::v4().to_string() + ".bin");
      write(f.source, "content-" + std::to_string(i));
      add.files.push_back(std::move(f));
    }
    r.files.push_back(std::move(add));
    return value(catalog.create(r)).change;
  }
  repo::ChangeResult change(const repo::ChangeResult& current, const std::string& title) {
    repo::ChangeRequest r;
    r.ref = current.ref;
    r.expected_rev = current.revision;
    repo::SetMetadata m;
    m.field = "title";
    m.value.emplace(title);
    r.ops.push_back(std::move(m));
    return value(catalog.change(r)).change;
  }
  fs::path path(const repo::ChangeResult& asset) const { return target(root, asset.ref.asset_id); }
  void clean() {
    EXPECT_TRUE(value(am::db::StateJournal::open(root))->list().value().empty());
    EXPECT_TRUE(fs::is_empty(root / ".am/.am-tmp"));
  }
};
std::vector<zip_test::Entry> entries(const fs::path& source) {
  auto zip = value(am::asset_zip::ZipArchive::open(value(am::asset_zip::file_source(source, true))));
  am::asset_zip::ReadBudget budget(UINT64_MAX);
  std::vector<zip_test::Entry> out;
  for (const auto& entry : zip.entries()) {
    auto stream = value(zip.open_entry(entry.index, budget));
    std::array<std::byte, 65536> buffer{};
    zip_test::Entry next;
    next.name = entry.raw_name;
    next.data.clear();
    for (;;) {
      const auto n = value(stream.read(buffer));
      if (!n) break;
      next.data.append(reinterpret_cast<const char*>(buffer.data()), n);
    }
    out.push_back(std::move(next));
  }
  return out;
}
fs::path built(const fs::path& root, const std::string& name, const std::vector<zip_test::Entry>& data) {
  const auto result = zip_test::build(data);
  const auto path = root / name;
  write(path, std::string(reinterpret_cast<const char*>(result.bytes.data()), result.bytes.size()));
  return path;
}
fs::path altered(World& src, const repo::ChangeResult& asset, const std::string& name,
                 const std::function<void(repo::Json&)>& mutate) {
  auto data = entries(src.path(asset));
  auto doc = repo::Json::parse(data[0].data);
  mutate(doc);
  data[0].data = doc.dump();
  return built(src.root, name, data);
}
void unchanged_import(World& dst, const fs::path& source, const std::string& classification) {
  repo::ContainerContents contents;
  repo::ContainerVerifyOptions verify;
  verify.streams = false;
  value(repo::verify_container(source, verify, &contents));
  const auto id = contents.document->known()["asset"]["id"].get<std::string>();
  const auto before = file_hash(target(dst.root, id)), incoming = file_hash(source);
  const auto result = value(dst.catalog.import_ready({source, {}}));
  EXPECT_EQ(result.status, "conflict");
  EXPECT_EQ(result.classification, classification);
  EXPECT_EQ(file_hash(target(dst.root, id)), before);
  EXPECT_EQ(file_hash(source), incoming);
  EXPECT_TRUE(result.error);
  dst.clean();
}
}  // namespace

TEST(ImportReady, NewRevisionThreeKeepsEveryByteHistoryAndActualIndexRevision) {
  RecordProperty("req", "K-31");
  World src, dst;
  auto asset = src.create();
  asset = src.change(asset, "Revision two");
  asset = src.change(asset, "Revision three");
  const auto input = src.root / "ready asset renamed.zip";
  fs::copy_file(src.path(asset), input);
  const auto before = file_hash(input);
  const auto result = value(dst.catalog.import_ready({input, {}}));
  EXPECT_EQ(result.classification, "new");
  EXPECT_EQ(result.status, "imported");
  EXPECT_TRUE(result.op_id);
  EXPECT_EQ(result.ref.library_id, dst.catalog.library().config().library_id);
  EXPECT_EQ(result.ref.asset_id, asset.ref.asset_id);
  EXPECT_EQ(file_hash(target(dst.root, asset.ref.asset_id)), before);
  EXPECT_EQ(file_hash(input), before);
  auto snap = value(dst.catalog.library().assets().snapshot(result.ref));
  EXPECT_EQ(snap.revision(), 3u);
  EXPECT_EQ(snap.manifest().known()["history"]["entries"].size(), 3u);
  EXPECT_EQ(value(dst.catalog.indexer().list())[0].revision, 3);
  dst.clean();
}
TEST(ImportReady, IdenticalManifestSkipsPublicationAndReportsWholeZipEquality) {
  RecordProperty("req", "K-31");
  World src, dst;
  const auto asset = src.create();
  const auto input = src.path(asset);
  value(dst.catalog.import_ready({input, {}}));
  const auto output = target(dst.root, asset.ref.asset_id);
  const auto before = file_hash(output);
  const auto time = fs::last_write_time(output);
  auto result = value(dst.catalog.import_ready({input, {}}));
  EXPECT_EQ(result.status, "identical");
  EXPECT_EQ(result.zip_bytes_identical, true);
  EXPECT_FALSE(result.op_id);
  auto reordered = entries(input);
  ASSERT_GE(reordered.size(), 3u);
  std::reverse(reordered.begin() + 1, reordered.end());
  const auto other = built(src.root, "reordered.zip", reordered);
  ASSERT_NE(file_hash(other), before);
  result = value(dst.catalog.import_ready({other, {}}));
  EXPECT_EQ(result.status, "identical");
  EXPECT_EQ(result.zip_bytes_identical, false);
  EXPECT_EQ(file_hash(output), before);
  EXPECT_EQ(fs::last_write_time(output), time);
  dst.clean();
}
TEST(ImportReady, EveryConflictClassUsesCurrentFileAndPreservesBothInputs) {
  RecordProperty("req", "K-31");
  World src, dst, branch;
  auto asset = src.create();
  const auto old = src.root / "old.zip";
  fs::copy_file(src.path(asset), old);
  value(dst.catalog.import_ready({old, {}}));
  asset = src.change(asset, "Two");
  asset = src.change(asset, "Three");
  unchanged_import(dst, src.path(asset), "newer_revision");
  auto conflict = value(dst.catalog.import_ready({src.path(asset), {}}));
  ASSERT_TRUE(conflict.current);
  EXPECT_EQ(conflict.current->revision, 1u);
  EXPECT_EQ(conflict.incoming->revision, 3u);
  value(dst.catalog.import_ready({src.path(asset), conflict.current->manifest_sha256}));
  unchanged_import(dst, old, "older_revision");
  auto other = branch.create("Different history", asset.ref.asset_id);
  other = branch.change(other, "Other two");
  other = branch.change(other, "Other three");
  unchanged_import(dst, branch.path(other), "same_revision_different");
  const auto output = target(dst.root, asset.ref.asset_id);
  write(output, "unreadable current");
  const auto before = file_hash(output);
  conflict = value(dst.catalog.import_ready({src.path(asset), {}}));
  EXPECT_EQ(conflict.classification, "current_unreadable");
  EXPECT_EQ(conflict.status, "conflict");
  ASSERT_TRUE(conflict.current);
  EXPECT_EQ(conflict.current->zip_size, "18");
  EXPECT_FALSE(conflict.current->revision);
  EXPECT_FALSE(conflict.current->manifest_sha256);
  EXPECT_EQ(file_hash(output), before);
  fs::remove(output);
  auto restored = value(dst.catalog.import_ready({src.path(asset), {}}));
  value(dst.catalog.trash(restored.ref));
  const auto trash = dst.root / "trash" / (asset.ref.asset_id + ".zip");
  const auto trashed = file_hash(trash);
  conflict = value(dst.catalog.import_ready({src.path(asset), {}}));
  EXPECT_EQ(conflict.classification, "in_trash");
  EXPECT_EQ(conflict.status, "conflict");
  EXPECT_EQ(conflict.current->revision, 3u);
  EXPECT_EQ(file_hash(trash), trashed);
  dst.clean();
}
TEST(ImportReady, ExplicitExpectedHashReplacesNewerOlderAndDifferentAndKeepsUserdata) {
  RecordProperty("req", "K-31");
  World src, branch;
  auto asset = src.create();
  const auto old = src.root / "old.zip";
  fs::copy_file(src.path(asset), old);
  asset = src.change(asset, "Two");
  asset = src.change(asset, "Three");
  const auto root = fresh();
  {
    auto l = value(lib::Library::open(root, options()));
    auto c = am::db::Connection::borrow(l.database(lib::DatabaseKind::userdata));
    value(c.exec("CREATE TABLE sentinel(v); INSERT INTO sentinel VALUES('retained')"));
  }
  const auto userdata = file_hash(root / ".am/userdata.sqlite");
  {
    auto catalog = value(cat::Catalog::open(root, options()));
    value(catalog.indexer().rebuild());
    value(catalog.import_ready({old, {}}));
    const auto output = target(root, asset.ref.asset_id);
    const auto before = file_hash(output);
    auto bad = catalog.import_ready({src.path(asset), std::string(64, '0')});
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().message_id, "am.repo.import_current_changed");
    EXPECT_EQ(file_hash(output), before);
    auto conflict = value(catalog.import_ready({src.path(asset), {}}));
    auto replaced = value(catalog.import_ready({src.path(asset), conflict.current->manifest_sha256}));
    EXPECT_EQ(replaced.status, "replaced");
    EXPECT_EQ(file_hash(output), file_hash(src.path(asset)));
    EXPECT_EQ(value(catalog.indexer().list())[0].revision, 3);
    conflict = value(catalog.import_ready({old, {}}));
    replaced = value(catalog.import_ready({old, conflict.current->manifest_sha256}));
    EXPECT_EQ(replaced.status, "replaced");
    ASSERT_FALSE(replaced.warnings.empty());
    EXPECT_EQ(replaced.warnings[0].message_id, "am.repo.import_older_revision");
    EXPECT_EQ(file_hash(output), file_hash(old));
    EXPECT_EQ(value(catalog.indexer().list())[0].revision, 1);
    auto other = branch.create("Different revision one", asset.ref.asset_id);
    conflict = value(catalog.import_ready({branch.path(other), {}}));
    EXPECT_EQ(conflict.classification, "same_revision_different");
    replaced = value(catalog.import_ready({branch.path(other), conflict.current->manifest_sha256}));
    EXPECT_EQ(replaced.status, "replaced");
    EXPECT_EQ(file_hash(output), file_hash(branch.path(other)));
  }
  EXPECT_EQ(file_hash(root / ".am/userdata.sqlite"), userdata);
}
TEST(ImportReady, ReplacementRejectsNewIdenticalUnreadableAndTrash) {
  RecordProperty("req", "K-31");
  World src, dst;
  const auto asset = src.create();
  const auto input = src.path(asset);
  auto reject = [&] {
    auto r = dst.catalog.import_ready({input, std::string(64, '0')});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, am::Errc::invalid_argument);
    EXPECT_EQ(r.error().message_id, "am.repo.import_replace_not_applicable");
    dst.clean();
  };
  reject();
  auto first = value(dst.catalog.import_ready({input, {}}));
  reject();
  value(dst.catalog.trash(first.ref));
  reject();
  value(dst.catalog.restore(first.ref));
  write(target(dst.root, asset.ref.asset_id), "unreadable");
  reject();
}
TEST(ImportReady, RejectsForeignNotZipCorruptMajorTwoBadShaAndV3WithoutFootprints) {
  RecordProperty("req", "K-31");
  World src;
  const auto asset = src.create();
  const auto foreign = built(src.root, "foreign.zip", {zip_test::Entry{}});
  const auto not_zip = src.root / "plain.bin";
  write(not_zip, "not a ZIP");
  auto corrupt_data = entries(src.path(asset));
  corrupt_data[0].data += ",";
  const auto corrupt = built(src.root, "corrupt.zip", corrupt_data);
  auto major = altered(src, asset, "major-two.zip", [](repo::Json& j) { j["format"]["version"]["major"] = 2; });
  auto bad_data = entries(src.path(asset));
  bad_data[1].data += "changed payload";
  auto bad_sha = built(src.root, "bad-sha.zip", bad_data);
  auto bad_v3 = altered(src, asset, "bad-v3.zip", [](repo::Json& j) { j["files"][1]["id"] = j["files"][0]["id"]; });
  for (const auto& [file, id] :
       std::vector<std::pair<fs::path, std::string> >{{foreign, "am.repo.not_own_format"},
                                                      {not_zip, "am.repo.not_own_format"},
                                                      {corrupt, "am.repo.import_invalid"},
                                                      {major, "am.repo.import_unsupported_version"},
                                                      {bad_sha, "am.repo.import_invalid"},
                                                      {bad_v3, "am.repo.import_invalid"}}) {
    SCOPED_TRACE(file.string());
    World dst;
    repo::ImportReadyResult detail;
    auto result = dst.catalog.import_ready({file, {}}, &detail);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().message_id, id);
    EXPECT_EQ(value(dst.catalog.indexer().count()), 0);
    EXPECT_FALSE(fs::exists(target(dst.root, asset.ref.asset_id)));
    dst.clean();
    if (file == foreign || file == not_zip) {
      EXPECT_EQ(detail.classification, "not_own");
      EXPECT_TRUE(detail.ref.asset_id.empty());
    }
    if (file == bad_sha || file == bad_v3 || file == corrupt) {
      EXPECT_TRUE(detail.verification);
    }
  }
}
TEST(ImportReady, NewerMinorAndPartialPluginSemanticsAreStoredUnchanged) {
  RecordProperty("req", "K-31");
  for (const auto* fixture : {"pos-newer-minor", "pos-full"}) {
    World dst;
    auto source = fs::path(AM_GOLDEN_FIXTURES) / fixture / "asset.zip";
    const auto before = file_hash(source);
    auto result = value(dst.catalog.import_ready({source, {}}));
    EXPECT_EQ(result.status, "imported");
    EXPECT_EQ(file_hash(target(dst.root, result.ref.asset_id)), before);
    if (std::string_view(fixture) == "pos-newer-minor") {
      ASSERT_FALSE(result.warnings.empty());
      EXPECT_EQ(result.warnings[0].message_id, "am.repo.import_newer_minor");
      EXPECT_EQ(value(dst.catalog.indexer().list())[0].health, "warn");
    }
    dst.clean();
  }
  World src, dst;
  const auto asset = src.create();
  auto opts = raw_options(dst.catalog.library());
  opts.semantic_validator = value(repo::repository_invariants());
  auto raw = value(repo::AssetRepository::open(dst.root, opts));
  auto partial = value(raw.import_ready({src.path(asset), {}}));
  ASSERT_TRUE(partial.verification);
  EXPECT_EQ(partial.verification->v3, repo::CheckStatus::partial);
  EXPECT_EQ(partial.status, "imported");
  for (const auto& stream : partial.verification->v4) EXPECT_EQ(stream.status, repo::StreamStatus::verified_sha256);
  EXPECT_EQ(file_hash(target(dst.root, asset.ref.asset_id)), file_hash(src.path(asset)));
  dst.clean();
}
namespace {
class DamagedCopySink final : public am::asset_zip::RandomAccessSink {
 public:
  std::shared_ptr<am::asset_zip::RandomAccessSink> inner;
  am::Result<void> write_at(std::uint64_t offset, std::span<const std::byte> data) override {
    std::vector<std::byte> changed(data.begin(), data.end());
    if (offset == 0 && !changed.empty()) changed[0] ^= std::byte{1};
    return inner->write_at(offset, changed);
  }
  am::Result<void> flush() override { return inner->flush(); }
  am::Result<void> close() override { return inner->close(); }
  am::Result<void> discard() override { return inner->discard(); }
  std::uint64_t size() const noexcept override { return inner->size(); }
};
}  // namespace
TEST(ImportReady, CopiedDigestRejectsDamagedTemporarySinkBeforePublication) {
  RecordProperty("req", "K-31");
  World src, dst;
  const auto asset = src.create();
  auto opts = raw_options(dst.catalog.library());
  opts.test_hooks.commit.create_sink =
      [](const fs::path& p) -> am::Result<std::shared_ptr<am::asset_zip::RandomAccessSink> > {
    auto wrapped = std::make_shared<DamagedCopySink>();
    wrapped->inner = value(am::asset_zip::file_sink(p));
    return std::shared_ptr<am::asset_zip::RandomAccessSink>(wrapped);
  };
  auto raw = value(repo::AssetRepository::open(dst.root, opts));
  auto imported = raw.import_ready({src.path(asset), {}});
  ASSERT_FALSE(imported);
  EXPECT_EQ(imported.error().message_id, "am.repo.import_invalid");
  EXPECT_FALSE(fs::exists(target(dst.root, asset.ref.asset_id)));
  dst.clean();
}
TEST(ImportReady, SourceChangesAfterCopyAndInsideLibraryAreRejected) {
  RecordProperty("req", "K-31");
  World src, dst;
  const auto asset = src.create();
  auto opts = raw_options(dst.catalog.library());
  opts.test_hooks.commit.point = [&](commit::FaultPoint p) -> am::Result<void> {
    if (p == commit::FaultPoint::after_finish) write(src.path(asset), "source was externally changed");
    return {};
  };
  auto raw = value(repo::AssetRepository::open(dst.root, opts));
  auto changed = raw.import_ready({src.path(asset), {}});
  ASSERT_FALSE(changed);
  EXPECT_EQ(changed.error().message_id, "am.repo.import_source_changed");
  EXPECT_FALSE(fs::exists(target(dst.root, asset.ref.asset_id)));
  dst.clean();
  const auto internal = dst.root / "manually-dropped.zip";
  write(internal, "inside");
  const auto before = bytes(internal);
  auto rejected = dst.catalog.import_ready({internal, {}});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, am::Errc::invalid_argument);
  EXPECT_EQ(rejected.error().message_id, "am.repo.import_source_inside_library");
  EXPECT_EQ(bytes(internal), before);
  dst.clean();
}
TEST(ImportReady, NewTargetAppearingBeforeReplacementIsNeverOverwritten) {
  RecordProperty("req", "K-31");
  World src, dst;
  const auto asset = src.create();
  const auto output = target(dst.root, asset.ref.asset_id);
  auto opts = raw_options(dst.catalog.library());
  opts.test_hooks.commit.point = [&](commit::FaultPoint p) -> am::Result<void> {
    if (p == commit::FaultPoint::before_replace) write(output, "external target");
    return {};
  };
  auto raw = value(repo::AssetRepository::open(dst.root, opts));
  auto result = raw.import_ready({src.path(asset), {}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().message_id, "am.commit.base_changed");
  EXPECT_EQ(bytes(output), "external target");
  dst.clean();
}
TEST(ImportReady, IndexFailureDoesNotUndoPublicationAndRebuildRepairs) {
  RecordProperty("req", "K-31");
  World src;
  const auto asset = src.create();
  cat::IndexerHooks hooks;
  hooks.before_update = [](std::string_view) -> am::Result<void> {
    return am::Error::make(am::Errc::io_error, "Index fault").id("am.catalog.error");
  };
  World dst(hooks);
  auto result = value(dst.catalog.import_ready({src.path(asset), {}}));
  EXPECT_EQ(result.status, "imported");
  ASSERT_FALSE(result.warnings.empty());
  EXPECT_TRUE(value(dst.catalog.indexer().rebuild_required()));
  EXPECT_EQ(file_hash(target(dst.root, asset.ref.asset_id)), file_hash(src.path(asset)));
  value(dst.catalog.indexer().rebuild());
  EXPECT_EQ(value(dst.catalog.indexer().count()), 1);
}
TEST(ImportReady, EveryFaultPointForNewAndReplacementRecoversAtomicallyWithIndex) {
  RecordProperty("req", "SC-14");
  World src;
  auto asset = src.create();
  const auto old = src.root / "old.zip";
  fs::copy_file(src.path(asset), old);
  asset = src.change(asset, "Two");
  asset = src.change(asset, "Three");
  for (bool replace : {false, true})
    for (auto point : {commit::FaultPoint::after_lock, commit::FaultPoint::after_prepared,
                       commit::FaultPoint::mid_write, commit::FaultPoint::after_finish,
                       commit::FaultPoint::after_verify, commit::FaultPoint::after_verified_journal,
                       commit::FaultPoint::before_replace, commit::FaultPoint::after_replace,
                       commit::FaultPoint::after_committed_journal, commit::FaultPoint::after_indexed_journal}) {
      SCOPED_TRACE(std::string(commit::point_name(point)) + (replace ? ":replace" : ":new"));
      World dst;
      std::optional<std::string> expected;
      if (replace) {
        value(dst.catalog.import_ready({old, {}}));
        expected = value(dst.catalog.import_ready({src.path(asset), {}})).current->manifest_sha256;
      }
      auto opts = raw_options(dst.catalog.library());
      opts.test_hooks.commit.point = [point](commit::FaultPoint p) -> am::Result<void> {
        if (p == point) return am::Error::make(am::Errc::io_error, "Injected import failure").id("am.commit.io_error");
        return {};
      };
      auto raw = value(repo::AssetRepository::open(dst.root, opts));
      ASSERT_FALSE(raw.import_ready({src.path(asset), expected}));
      value(dst.catalog.library().assets().recover());
      const bool published = point == commit::FaultPoint::after_replace ||
                             point == commit::FaultPoint::after_committed_journal ||
                             point == commit::FaultPoint::after_indexed_journal;
      const auto output = target(dst.root, asset.ref.asset_id);
      if (published) {
        EXPECT_EQ(file_hash(output), file_hash(src.path(asset)));
        const auto rows = value(dst.catalog.indexer().list());
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0].revision, 3);
      } else if (replace) {
        EXPECT_EQ(file_hash(output), file_hash(old));
        const auto rows = value(dst.catalog.indexer().list());
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0].revision, 1);
      } else {
        EXPECT_FALSE(fs::exists(output));
        EXPECT_EQ(value(dst.catalog.indexer().count()), 0);
      }
      dst.clean();
    }
}
#ifdef _WIN32
namespace {
struct Process {
  HANDLE process = nullptr;
  Process(const fs::path& root, const fs::path& source, const std::string& library, commit::FaultPoint point,
          const fs::path& marker, const std::optional<std::string>& expected) {
    const auto wide = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
    const auto exe = fs::path(AM_IMPORT_HELPER_PATH).wstring();
    std::wstring command = L"\"" + exe + L"\" \"" + root.wstring() + L"\" \"" + source.wstring() + L"\" " +
                           wide(library) + L" " + wide(std::string(commit::point_name(point))) + L" \"" +
                           marker.wstring() + L"\" " + wide(expected.value_or("-"));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION p{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &p))
      throw std::runtime_error("Create import helper failed");
    process = p.hProcess;
    CloseHandle(p.hThread);
  }
  ~Process() {
    if (process) {
      if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 92);
      WaitForSingleObject(process, 30000);
      CloseHandle(process);
    }
  }
  void kill() {
    if (!TerminateProcess(process, 92) || WaitForSingleObject(process, 30000) != WAIT_OBJECT_0)
      throw std::runtime_error("Kill import helper failed");
  }
};
}  // namespace
#endif
TEST(ImportReady, KillVerifiedAndReplacedForNewAndReplacementRecoverWithIndex) {
  RecordProperty("req", "SC-14");
#ifdef _WIN32
  World src;
  auto asset = src.create();
  const auto old = src.root / "old.zip";
  fs::copy_file(src.path(asset), old);
  asset = src.change(asset, "Two");
  asset = src.change(asset, "Three");
  for (bool replace : {false, true})
    for (auto point : {commit::FaultPoint::after_verified_journal, commit::FaultPoint::after_replace}) {
      World dst;
      std::optional<std::string> expected;
      if (replace) {
        value(dst.catalog.import_ready({old, {}}));
        expected = value(dst.catalog.import_ready({src.path(asset), {}})).current->manifest_sha256;
      }
      const auto marker = dst.root / "helper-ready";
      Process process(dst.root, src.path(asset), dst.catalog.library().config().library_id, point, marker, expected);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (!fs::exists(marker)) {
        if (std::chrono::steady_clock::now() > deadline)
          throw std::runtime_error("Import helper did not reach checkpoint");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      process.kill();
      value(dst.catalog.library().assets().recover());
      EXPECT_EQ(file_hash(target(dst.root, asset.ref.asset_id)), file_hash(src.path(asset)));
      EXPECT_EQ(value(dst.catalog.indexer().list())[0].revision, 3);
      dst.clean();
    }
#else
  GTEST_SKIP() << "Crash-process and fixed-NTFS evidence is verified on Windows; all fault points are exercised with "
                  "injected capabilities";
#endif
}
#endif
