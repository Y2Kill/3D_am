#include <gtest/gtest.h>
#include <sqlite3.h>

#include <fstream>
#include <iostream>
#include <set>

#include "am/application/export_service.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/storage/asset_zip/probe.hpp"
#include "am/storage/db/job_store.hpp"
#include "am/storage/db/operation_log.hpp"
#include "am/storage/fs/publish.hpp"
#include "zip_builder.hpp"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {
namespace ex = am::exporting;
namespace be = am::storage::exporting;
namespace repo = am::repository;
namespace fs = std::filesystem;
using J = repo::Json;
template <class T> T value(am::Result<T> result) {
  if (!result) throw std::runtime_error(am::to_log_string(result.error()));
  return std::move(*result);
}
void value(am::Result<void> result) { if (!result) throw std::runtime_error(am::to_log_string(result.error())); }
std::string uuid() { return am::Uuid::v4().to_string(); }
std::string bytes(const fs::path& path) {
  std::ifstream file(am::paths::native(path),std::ios::binary);
  if (!file) throw std::runtime_error("Cannot read synthetic export file");
  return {std::istreambuf_iterator<char>(file),{}};
}
void write(const fs::path& path, std::string_view data) {
  std::ofstream file(am::paths::native(path),std::ios::binary); file << data;
  if (!file) throw std::runtime_error("Cannot write synthetic export file");
}
am::library::OpenOptions writable() {
  am::library::OpenOptions options; options.capabilities.emplace(); options.capabilities->mode = repo::StorageMode::full;
  return options;
}
struct World {
  fs::path home = fs::path(AM_EXPORT_TEST_ROOT) / uuid(), root = home / "library", target = home / "output";
  std::optional<am::catalog::Catalog> catalog;
  World() {
    fs::create_directories(home); value(am::library::Library::init(root)); fs::create_directories(target);
    catalog.emplace(value(am::catalog::Catalog::open(root,writable())));
    value(catalog->indexer().rebuild());
  }
  repo::ChangeResult create(std::string title = "Export model", std::vector<std::string> names = {"part.stl","picture.png","other.bin"}) {
    repo::CreateRequest request; request.metadata = J{{"title",title}};
    repo::AddFiles add;
    add.input = {{"id",uuid()},{"imported_at","2026-11-26T00:00:00Z"},{"mode","strict"},{"completeness","complete"},
                 {"objects",J::array()},{"entries",J::array()}};
    for (std::size_t i = 0; i < names.size(); ++i) {
      const auto source = home / (uuid()+".bin"); write(source,std::string(4000,static_cast<char>('a'+i)));
      repo::FileInput file; file.path = names[i]; file.source = source; add.files.push_back(std::move(file));
    }
    request.files.push_back(std::move(add)); return value(catalog->create(request)).change;
  }
  fs::path asset_path(const repo::ChangeResult& asset) const {
    return root / "assets" / asset.ref.asset_id.substr(34) / (asset.ref.asset_id+".zip");
  }
  ex::ExportRequest request(const repo::ChangeResult& asset) const {
    ex::ExportRequest out; out.asset_ids = {asset.ref.asset_id}; out.target_dir = target; return out;
  }
  J run(const ex::ExportRequest& request, ex::RunHooks hooks = {}) {
    return value(ex::export_direct(*catalog,request,{},std::move(hooks)));
  }
  void clean() const {
    for (const auto& entry : fs::directory_iterator(target)) EXPECT_EQ(entry.path().extension(),".zip");
  }
};
const J& row(const J& result) { return result.at("assets").at(0); }
J dump_database(sqlite3* database) {
  const auto prepare = [&](const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database,sql.c_str(),-1,&statement,nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(database));
    return std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>(statement,sqlite3_finalize);
  };
  auto tables = prepare("SELECT name,sql FROM sqlite_master WHERE type='table' ORDER BY name");
  J result = J::object();
  for (int rc = sqlite3_step(tables.get()); rc != SQLITE_DONE; rc = sqlite3_step(tables.get())) {
    if (rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    const std::string name(reinterpret_cast<const char*>(sqlite3_column_text(tables.get(),0)));
    std::string escaped;
    for (const auto c : name) { escaped += c; if (c == '"') escaped += c; }
    auto rows = prepare("SELECT * FROM \""+escaped+"\"");
    std::vector<std::string> values;
    for (int step = sqlite3_step(rows.get()); step != SQLITE_DONE; step = sqlite3_step(rows.get())) {
      if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
      J value = J::array();
      for (int column = 0; column < sqlite3_column_count(rows.get()); ++column) {
        const auto type = sqlite3_column_type(rows.get(),column);
        std::string bytes;
        if (type != SQLITE_NULL) {
          const auto* data = static_cast<const char*>(sqlite3_column_blob(rows.get(),column));
          const auto count = sqlite3_column_bytes(rows.get(),column);
          // Hex retains BLOBs, embedded NULs and exact scalar representations.
          constexpr char digits[] = "0123456789abcdef";
          for (int i = 0; i < count; ++i) { const auto c = static_cast<unsigned char>(data[i]); bytes += digits[c>>4]; bytes += digits[c&15]; }
        }
        value.push_back({type,bytes});
      }
      values.push_back(value.dump());
    }
    std::sort(values.begin(),values.end());
    result[name] = {{"sql",reinterpret_cast<const char*>(sqlite3_column_text(tables.get(),1))},{"rows",values}};
  }
  return result;
}

TEST(Export, FullIsByteExactAndSnapshotSurvivesConcurrentCommit) {
  RecordProperty("req","K-10|K-116|ADR-0025");
  World world; const auto asset = world.create(); const auto original = bytes(world.asset_path(asset));
  ex::RunHooks hooks;
  hooks.after_snapshot = [&](const auto&) {
    repo::ChangeRequest change; change.ref = asset.ref; change.expected_rev = 1;
    change.ops.push_back(repo::SetMetadata{"asset","title",std::optional<J>{J("New revision")}});
    value(world.catalog->change(change));
  };
  const auto result = world.run(world.request(asset),hooks);
  ASSERT_EQ(row(result).at("status"),"exported") << result.dump();
  EXPECT_EQ(row(result).at("revision"),1);
  EXPECT_EQ(row(result).at("asset_id"),asset.ref.asset_id);
  EXPECT_EQ(row(result).at("target_name"),"Export model.zip");
  EXPECT_EQ(bytes(world.target / "Export model.zip"),original);
  EXPECT_NE(bytes(world.asset_path(asset)),original);
  const auto verified = value(repo::verify_container(world.target / "Export model.zip"));
  EXPECT_FALSE(verified.error);
  world.clean();
}

TEST(Export, SelectiveRawStreamsAndCollisionPathsPreserveEveryByte) {
  RecordProperty("req","K-10|K-46|K-116");
  World world; const auto asset = world.create("e\xcc\x81",{"part.stl","picture.png","other.bin"});
  auto request = world.request(asset); request.presets = {ex::Preset::model};
  auto snapshot = value(world.catalog->library().assets().snapshot(asset.ref));
  const auto original = snapshot.manifest().known();
  auto result = world.run(request);
  ASSERT_EQ(row(result).at("status"),"exported") << result.dump();
  const auto output = world.target / fs::path(std::u8string(u8"e\u0301 (3d).zip"));
  auto archive = value(am::asset_zip::ZipArchive::open(value(am::asset_zip::file_source(output))));
  ASSERT_EQ(archive.entries().size(),1u);
  EXPECT_EQ(archive.entries()[0].raw_name,"e\xcc\x81 (3d)/part.stl");
  EXPECT_EQ(archive.entries()[0].method,snapshot.archive().entries()[1].method);
  EXPECT_EQ(archive.entries()[0].crc32,snapshot.archive().entries()[1].crc32);
  std::vector<std::byte> output_raw(static_cast<std::size_t>(archive.entries()[0].compressed_size));
  std::vector<std::byte> source_raw(output_raw.size());
  EXPECT_EQ(value(archive.read_raw(0,0,output_raw)),output_raw.size());
  EXPECT_EQ(value(snapshot.archive().read_raw(1,0,source_raw)),source_raw.size());
  EXPECT_EQ(output_raw,source_raw);
  EXPECT_TRUE(archive.entries()[0].utf8_flag);
  am::asset_zip::ReadBudget budget(16000);
  const auto stream = value(archive.verify_entry(0,budget,true));
  EXPECT_EQ(stream.size,4000u);
  EXPECT_EQ(value(am::asset_zip::probe_container(value(am::asset_zip::file_source(output)))).outcome,
             am::asset_zip::ProbeOutcome::foreign_zip);
  EXPECT_EQ(archive.summary().comment_length,0);
  request.presets = {ex::Preset::all}; result = world.run(request);
  ASSERT_EQ(row(result).at("status"),"exported") << result.dump();
  EXPECT_EQ(row(result).at("file_count"),3);
  world.clean();
}

TEST(Export, ConflictsNeverReplaceAndRenameUsesNextName) {
  RecordProperty("req","K-10|NFR-14.5.6");
  World world; const auto asset = world.create(); auto request = world.request(asset);
  write(world.target / "Export model.zip","original target");
  auto result = world.run(request);
  EXPECT_EQ(row(result).at("error").at("message_id"),"am.export.target_exists");
  EXPECT_EQ(bytes(world.target / "Export model.zip"),"original target");
  request.on_conflict = ex::ConflictPolicy::rename;
  result = world.run(request);
  ASSERT_EQ(row(result).at("status"),"exported") << result.dump();
  EXPECT_EQ(row(result).at("target_name"),"Export model (2).zip");
#ifdef _WIN32
  request.name = "EXPORT MODEL.ZIP"; request.on_conflict = ex::ConflictPolicy::fail;
  result = world.run(request);
  EXPECT_EQ(row(result).at("error").at("message_id"),"am.export.target_exists");
#endif
  world.clean();
}

TEST(Export, PublicationRacePreservesForeignFileAndRemovesTemporary) {
  RecordProperty("req","NFR-14.5.6");
  World world; const auto asset = world.create(); ex::RunHooks hooks;
  hooks.before_publish = [](const auto& target) { write(target,"racing publisher"); };
  const auto result = world.run(world.request(asset),hooks);
  EXPECT_EQ(row(result).at("error").at("message_id"),"am.export.target_exists");
  EXPECT_EQ(bytes(world.target / "Export model.zip"),"racing publisher");
  world.clean();
}

TEST(Export, CancelSpaceAndCorruptTemporaryLeaveNoOutput) {
  RecordProperty("req","K-10|NFR-14.4.3|NFR-14.5.6");
  World world; const auto asset = world.create();
  ex::RunHooks hooks;
  hooks.storage.available_space = [](const auto&) { return std::uintmax_t{0}; };
  auto result = world.run(world.request(asset),hooks);
  EXPECT_EQ(row(result).at("error").at("message_id"),"am.export.insufficient_space");
  EXPECT_TRUE(fs::is_empty(world.target));
  hooks = {}; hooks.storage.after_write = [](const auto& temporary) {
    auto content = bytes(temporary); content[50] ^= 1; write(temporary,content);
  };
  result = world.run(world.request(asset),hooks);
  EXPECT_EQ(row(result).at("error").at("message_id"),"am.export.verification_failed");
  EXPECT_TRUE(fs::is_empty(world.target));
  auto snapshot = value(world.catalog->library().assets().snapshot(asset.ref));
  std::stop_source stop;
  be::PrepareOptions options; options.target_dir = world.target; options.stop = stop.get_token();
  options.hooks.on_read = [&](auto) { stop.request_stop(); };
  auto cancelled = be::prepare(snapshot,options);
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().code,am::Errc::cancelled);
  EXPECT_TRUE(fs::is_empty(world.target));
}

TEST(Export, QuickRejectsCrcCorruptionOutsideManifest) {
  RecordProperty("req","K-10|ADR-0035");
  World world; const auto asset = world.create();
  std::uint64_t offset = 0;
  {
    auto snapshot = value(world.catalog->library().assets().snapshot(asset.ref));
    offset = snapshot.archive().entries()[1].data_offset;
  }
  auto content = bytes(world.asset_path(asset)); content.at(static_cast<std::size_t>(offset)) ^= 1;
  write(world.asset_path(asset),content);
  const auto result = world.run(world.request(asset));
  ASSERT_EQ(row(result).at("status"),"failed") << result.dump();
  EXPECT_EQ(row(result).at("error").at("message_id"),"am.export.source_invalid");
  EXPECT_TRUE(fs::is_empty(world.target));
  auto selected = world.request(asset); selected.presets = {ex::Preset::all};
  const auto selective = world.run(selected);
  EXPECT_EQ(row(selective).at("error").at("message_id"),"am.export.source_invalid");
  EXPECT_TRUE(fs::is_empty(world.target));
}

TEST(Export, BatchContinuesAfterTrashAndUnknownIdsFailBeforeOutput) {
  RecordProperty("req","K-10|K-46");
  World world; const auto first = world.create("First"), second = world.create("Second"), third = world.create("Third");
  value(world.catalog->trash(second.ref));
  auto request = world.request(first); request.asset_ids = {first.ref.asset_id,second.ref.asset_id,third.ref.asset_id};
  const auto result = world.run(request);
  EXPECT_EQ(result.at("assets").at(0).at("status"),"exported");
  EXPECT_EQ(result.at("assets").at(1).at("error").at("message_id"),"am.export.asset_in_trash");
  EXPECT_EQ(result.at("assets").at(2).at("status"),"exported");
  request = world.request(first); request.file_ids = {uuid()};
  const auto unknown = ex::export_direct(*world.catalog,request);
  ASSERT_FALSE(unknown); EXPECT_EQ(unknown.error().message_id,"am.export.file_unknown");
  world.clean();
}
TEST(Export, EmptySelectionFailsBeforeCreatingAnyOutput) {
  RecordProperty("req","K-10|K-46");
  World world; const auto asset = world.create("Only image",{"picture.png"});
  auto request = world.request(asset); request.presets = {ex::Preset::model};
  const auto result = world.run(request);
  EXPECT_EQ(row(result).at("status"),"failed");
  EXPECT_EQ(row(result).at("error").at("message_id"),"am.export.selection_empty");
  EXPECT_TRUE(fs::is_empty(world.target));
}

TEST(Export, DestinationMustBeOutsideLibraryAndDirectory) {
  RecordProperty("req","NFR-14.5.6");
  World world; const auto asset = world.create(); auto request = world.request(asset);
  request.target_dir = world.root / "assets";
  auto result = ex::export_direct(*world.catalog,request);
  ASSERT_FALSE(result); EXPECT_EQ(result.error().message_id,"am.export.target_inside_library");
  const auto file = world.home / "file"; write(file,"file"); request.target_dir = file;
  result = ex::export_direct(*world.catalog,request);
  ASSERT_FALSE(result); EXPECT_EQ(result.error().message_id,"am.export.target_unavailable");
  std::error_code ec; const auto link = world.home / "output-link"; fs::create_directory_symlink(world.target,link,ec);
  if (!ec) {
    request.target_dir = link; result = ex::export_direct(*world.catalog,request);
    ASSERT_FALSE(result); EXPECT_EQ(result.error().message_id,"am.export.target_unavailable");
  }
}

TEST(Export, ReadOnlyFullAndSelectiveLeaveDatabasesUnchanged) {
  RecordProperty("req","K-127|K-46");
  World world; const auto asset = world.create(); world.catalog.reset();
  std::vector<std::pair<fs::path,std::string>> before;
  for (const auto* name : {"index.sqlite","userdata.sqlite","state.sqlite"}) {
    const auto path = world.root / ".am" / name; before.emplace_back(path,bytes(path));
  }
  const auto source = bytes(world.asset_path(asset));
  auto read_only = writable(); read_only.mode = am::library::Mode::read_only;
  auto reader = value(am::library::Library::open(world.root,read_only));
  std::array<J,3> database_before;
  const std::array kinds{am::library::DatabaseKind::index,am::library::DatabaseKind::userdata,am::library::DatabaseKind::state};
  for (std::size_t i = 0; i < kinds.size(); ++i) database_before[i] = dump_database(reader.database(kinds[i]));
  am::application::ExportOptions options; options.request = world.request(asset); options.library.mode = am::library::Mode::read_only;
  auto result = value(am::application::export_assets(world.root,options));
  EXPECT_EQ(result.status,am::application::ExitCode::success); EXPECT_FALSE(result.value.at("queued").get<bool>());
  options.request.presets = {ex::Preset::model}; result = value(am::application::export_assets(world.root,options));
  EXPECT_EQ(result.status,am::application::ExitCode::success);
  options.enqueue_only = true;
  const auto rejected = am::application::export_assets(world.root,options);
  ASSERT_FALSE(rejected); EXPECT_EQ(rejected.error().message_id,"am.export.queue_unavailable");
  EXPECT_EQ(bytes(world.asset_path(asset)),source);
  for (const auto& [path,content] : before) EXPECT_EQ(bytes(path),content);
  for (std::size_t i = 0; i < kinds.size(); ++i) EXPECT_EQ(dump_database(reader.database(kinds[i])),database_before[i]);
  world.clean();
}

TEST(Export, PublishedCheckpointSurvivesFailedJobAndRetryWithoutSecondFile) {
  RecordProperty("req","K-10|K-121|NFR-14.5.6");
  World world; const auto asset = world.create(); auto request = world.request(asset); request.on_conflict = ex::ConflictPolicy::rename;
  am::jobs::ResourceManager resources(am::jobs::profile_limits(am::jobs::Profile::balanced,2));
  am::jobs::HandlerRegistry registry; ex::RunHooks hooks; bool crash = true;
  hooks.after_publish = [&](const auto&) {
    if (crash) throw am::Error::make(am::Errc::io_error,"Simulated failure after publication").id("am.export.io_error");
  };
  value(ex::register_export_jobs(registry,*world.catalog,resources,hooks));
  am::jobs::ExecutorOptions options; options.auto_retries = 0;
  am::jobs::Executor executor(world.catalog->job_store(),resources,registry,options);
  const auto parent = value(ex::enqueue_export(*world.catalog,request));
  value(executor.run_until_idle());
  auto summary = value(ex::export_summary(*world.catalog,parent));
  EXPECT_EQ(row(summary).at("status"),"failed");
  const auto child = row(summary).at("job_id").get<std::string>();
  auto record = value(world.catalog->job_store().get(child)); ASSERT_TRUE(record);
  ASSERT_TRUE(record->result_json);
  EXPECT_TRUE(J::parse(*record->result_json).contains("checkpoint"));
  const auto file = world.target / "Export model.zip";
  const auto modified = fs::last_write_time(file);
  crash = false; value(executor.retry(parent)); value(executor.run_until_idle());
  summary = value(ex::export_summary(*world.catalog,parent));
  ASSERT_EQ(row(summary).at("status"),"exported") << summary.dump();
  EXPECT_EQ(fs::last_write_time(file),modified);
  EXPECT_FALSE(fs::exists(world.target / "Export model (2).zip"));
  record = value(world.catalog->job_store().get(child)); ASSERT_TRUE(record->result_json);
  EXPECT_FALSE(J::parse(*record->result_json).contains("checkpoint"));
  world.clean();
}
TEST(Export, CheckpointMismatchHonorsConflictPolicyAndPreservesForeignBytes) {
  RecordProperty("req","K-121|NFR-14.5.6");
  for (const bool rename : {false,true}) {
    World world; const auto asset = world.create(); auto request = world.request(asset);
    request.on_conflict = rename ? ex::ConflictPolicy::rename : ex::ConflictPolicy::fail;
    am::jobs::ResourceManager resources(am::jobs::profile_limits(am::jobs::Profile::balanced,2));
    am::jobs::HandlerRegistry registry; ex::RunHooks hooks; bool crash = true;
    hooks.after_publish = [&](const auto&) {
      if (crash) throw am::Error::make(am::Errc::io_error,"Injected failure after publication").id("am.export.io_error");
    };
    value(ex::register_export_jobs(registry,*world.catalog,resources,hooks));
    am::jobs::ExecutorOptions options; options.auto_retries = 0;
    am::jobs::Executor executor(world.catalog->job_store(),resources,registry,options);
    const auto parent = value(ex::enqueue_export(*world.catalog,request)); value(executor.run_until_idle());
    const auto target = world.target / "Export model.zip"; write(target,"foreign replacement");
    crash = false; value(executor.retry(parent)); value(executor.run_until_idle());
    const auto result = value(ex::export_summary(*world.catalog,parent));
    EXPECT_EQ(bytes(target),"foreign replacement");
    if (rename) {
      ASSERT_EQ(row(result).at("status"),"exported") << result.dump();
      EXPECT_EQ(row(result).at("target_name"),"Export model (2).zip");
      EXPECT_EQ(bytes(world.target / "Export model (2).zip"),bytes(world.asset_path(asset)));
    } else {
      EXPECT_EQ(row(result).at("status"),"failed");
      EXPECT_EQ(row(result).at("error").at("message_id"),"am.export.target_exists");
      EXPECT_FALSE(fs::exists(world.target / "Export model (2).zip"));
    }
    world.clean();
  }
}
TEST(Export, PlainPathProfilePreservesNfdAndSupportsZipNameFieldLength) {
  RecordProperty("req","K-10|ADR-0035");
  const std::string nfd = "e\xcc\x81/part.stl";
  const std::string long_name = std::string(220,'x') + "/part.stl";
  for (const auto& name : {nfd,long_name,std::string(5000,'x')}) {
    EXPECT_FALSE(am::paths::check_container_path(name));
    EXPECT_TRUE(am::paths::check_container_path(name,am::paths::ContainerPathProfile::plain));
    auto sink = value(am::asset_zip::memory_sink());
    am::asset_zip::WriterOptions options; options.path_profile = am::paths::ContainerPathProfile::plain;
    auto writer = value(am::asset_zip::ZipWriter::create(sink,options));
    auto entry = value(writer.begin_entry(name,{})); value(entry.finish()); const auto summary = value(writer.finish());
    am::asset_zip::ReadBudget budget(100);
    value(am::asset_zip::verify_written(value(am::asset_zip::memory_source(value(sink->take_bytes()))),summary,budget));
  }
  EXPECT_FALSE(am::paths::check_container_path(std::string(65536,'x'),am::paths::ContainerPathProfile::plain));
  for (std::string bad : {"../a","/a","a//b","a/./b","CON/file","a\x7f/b","a\xc2\x80/b"})
    EXPECT_FALSE(am::paths::check_container_path(bad,am::paths::ContainerPathProfile::plain)) << bad;
  auto sink = value(am::asset_zip::memory_sink()); am::asset_zip::WriterOptions options;
  options.path_profile = am::paths::ContainerPathProfile::plain;
  auto writer = value(am::asset_zip::ZipWriter::create(sink,options));
  EXPECT_FALSE(writer.begin_entry("manifest.json",{}));
}

TEST(Export, LongOutputRootAndTwoInputSetsResolveCollisionsInManifestOrder) {
  RecordProperty("req","K-10|K-116|NFR-14.5.6");
  World world;
  const auto asset = world.create(std::string(245,'a'),{"Part.STL"});
  repo::ChangeRequest change; change.ref = asset.ref; change.expected_rev = 1;
  repo::AddFiles add; add.input = {{"id",uuid()},{"imported_at","2026-11-26T00:00:00Z"},{"mode","strict"},
                                  {"completeness","complete"},{"objects",J::array()},{"entries",J::array()}};
  const auto source = world.home / "second"; write(source,"second input");
  repo::FileInput file; file.path = "part.stl"; file.source = source; add.files.push_back(std::move(file));
  change.ops.push_back(std::move(add)); value(world.catalog->change(change));
  auto request = world.request(asset); request.presets = {ex::Preset::all};
  const auto result = world.run(request);
  ASSERT_EQ(row(result).at("status"),"exported") << result.dump();
  ASSERT_EQ(row(result).at("renamed").size(),1u);
  const auto output = world.target / row(result).at("target_name").get<std::string>();
  auto archive = value(am::asset_zip::ZipArchive::open(value(am::asset_zip::file_source(output))));
  ASSERT_EQ(archive.entries().size(),2u);
  EXPECT_GT(archive.entries()[0].raw_name.size(),200u);
  EXPECT_TRUE(archive.entries()[0].raw_name.ends_with("/Part.STL"));
  EXPECT_TRUE(archive.entries()[1].raw_name.ends_with("/part (2).stl"));
  am::asset_zip::ReadBudget budget(10000);
  auto a = value(archive.verify_entry(0,budget,true)), b = value(archive.verify_entry(1,budget,true));
  EXPECT_NE(a.sha256,b.sha256);
  EXPECT_EQ(row(result).at("renamed").at(0).at("path"),archive.entries()[1].raw_name);
}

TEST(Export, AllPresetsExplicitUnionAndNonImagePreviewUseOnlyCurrentStreams) {
  RecordProperty("req","K-10|K-46");
  World world; const auto asset = world.create("Categories",{"model.stl","texture.png","preview.bin","print.ctb","other.dat"});
  J manifest; std::vector<zip_test::Entry> entries;
  {
    auto snapshot = value(world.catalog->library().assets().snapshot(asset.ref)); manifest = snapshot.manifest().known();
    manifest["previews"] = {{"items",J::array({{{"id",uuid()},{"file_id",manifest.at("files").at(2).at("id")}}})}};
    // These are synthetic initial data for this validation test; no production
    // path edits a live ZIP. The fixture is checked by V1-V4 before exporting.
    zip_test::Entry root; root.name = "manifest.json"; root.data = manifest.dump(); entries.push_back(root);
    for (std::size_t i = 0; i < manifest.at("files").size(); ++i) {
      zip_test::Entry entry; entry.name = manifest.at("files").at(i).at("storage_path").get<std::string>();
      entry.data = std::string(4000,static_cast<char>('a'+i)); entry.method = i % 2 == 0 ? 0 : 8; entries.push_back(entry);
    }
  }
  const auto built = zip_test::build(entries);
  write(world.asset_path(asset),{reinterpret_cast<const char*>(built.bytes.data()),built.bytes.size()});
  ASSERT_FALSE(value(repo::verify_container(world.asset_path(asset))).error);
  for (const auto& [preset,expected] : std::array<std::pair<ex::Preset,std::size_t>,4>{{
           {ex::Preset::model,1},{ex::Preset::model_preview,3},{ex::Preset::slicer,2},{ex::Preset::all,5}}}) {
    auto request = world.request(asset); request.presets = {preset};
    const auto result = world.run(request);
    ASSERT_EQ(row(result).at("status"),"exported") << result.dump();
    EXPECT_EQ(row(result).at("file_count"),expected);
  }
  auto request = world.request(asset); request.file_ids = {manifest.at("files").at(4).at("id").get<std::string>()};
  const auto result = world.run(request);
  ASSERT_EQ(row(result).at("status"),"exported") << result.dump();
  EXPECT_EQ(row(result).at("file_count"),1u);
  EXPECT_EQ(row(result).at("target_name"),"Categories (files).zip");
}

TEST(Export, CheckpointWriteFailurePreventsPublicationAndCleansTemporary) {
  RecordProperty("req","K-121|NFR-14.5.6");
  World world; const auto asset = world.create();
  auto database = am::db::Connection::borrow(world.catalog->library().database(am::library::DatabaseKind::state));
  value(database.exec("CREATE TEMP TRIGGER export_checkpoint_failure BEFORE UPDATE OF result_json ON jobs WHEN NEW.result_json IS NOT NULL AND json_type(NEW.result_json,'$.checkpoint')='object' BEGIN SELECT RAISE(ABORT,'injected checkpoint failure'); END"));
  am::jobs::ResourceManager resources(am::jobs::profile_limits(am::jobs::Profile::balanced,2)); am::jobs::HandlerRegistry registry;
  value(ex::register_export_jobs(registry,*world.catalog,resources));
  am::jobs::ExecutorOptions options; options.auto_retries = 0;
  am::jobs::Executor executor(world.catalog->job_store(),resources,registry,options);
  const auto parent = value(ex::enqueue_export(*world.catalog,world.request(asset)));
  value(executor.run_until_idle());
  const auto result = value(ex::export_summary(*world.catalog,parent));
  EXPECT_EQ(row(result).at("status"),"failed");
  EXPECT_TRUE(fs::is_empty(world.target));
  value(database.exec("DROP TRIGGER export_checkpoint_failure"));
}
TEST(Export, FullTemporaryVerificationReportsVerifiedStreamsBeyondQuick) {
  RecordProperty("req","K-10|ADR-0035");
  World world; const auto source = world.home / "random.bin";
  std::string content(256*1024,'\0'); std::uint32_t random = 0x12345678;
  for (auto& c : content) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; c = static_cast<char>(random); }
  write(source,content);
  repo::CreateRequest create; repo::AddFiles add;
  add.input = {{"id",uuid()},{"imported_at","2026-11-26T00:00:00Z"},{"mode","strict"},
               {"completeness","complete"},{"objects",J::array()},{"entries",J::array()}};
  repo::FileInput file; file.path = "random.bin"; file.source = source; add.files.push_back(std::move(file));
  create.files.push_back(std::move(add)); const auto asset = value(world.catalog->create(create)).change;
  auto snapshot = value(world.catalog->library().assets().snapshot(asset.ref));
  std::array<std::size_t,2> source_streams{}, temporary_streams{}, source_reports{}, temporary_reports{};
  for (std::size_t i = 0; i < source_streams.size(); ++i) {
    be::PrepareOptions options; options.target_dir = world.target; options.verify_full = i != 0;
    options.hooks.on_container_verification = [&](bool temporary, const auto& report) {
      EXPECT_FALSE(report.error);
      EXPECT_EQ(report.v1,repo::CheckStatus::passed);
      EXPECT_EQ(report.v2,repo::CheckStatus::passed);
      EXPECT_EQ(report.v3,repo::CheckStatus::passed);
      std::size_t count = 0;
      for (const auto& stream : report.v4) {
        EXPECT_EQ(stream.status,repo::StreamStatus::verified_sha256);
        if (stream.status == repo::StreamStatus::verified_sha256) ++count;
      }
      if (temporary) { ++temporary_reports[i]; temporary_streams[i] = count; }
      else { ++source_reports[i]; source_streams[i] = count; }
    };
    auto prepared = value(be::prepare(snapshot,options));
  }
  EXPECT_EQ(source_reports,(std::array<std::size_t,2>{1,1}));
  EXPECT_EQ(temporary_reports,(std::array<std::size_t,2>{1,1}));
  EXPECT_EQ(source_streams,(std::array<std::size_t,2>{1,1}));
  EXPECT_EQ(temporary_streams,(std::array<std::size_t,2>{0,1}));
  RecordProperty("quick_temporary_verified_streams",std::to_string(temporary_streams[0]));
  RecordProperty("full_temporary_verified_streams",std::to_string(temporary_streams[1]));
  EXPECT_TRUE(fs::is_empty(world.target));
}

TEST(Export, OrphansOnlyRemoveOldRegularUnoccupiedUuidFiles) {
  RecordProperty("req","NFR-14.5.6");
  World world;
  const auto old = world.target / (".am-export-"+uuid()+".tmp");
  const auto young = world.target / (".am-export-"+uuid()+".tmp");
  const auto busy = world.target / (".am-export-"+uuid()+".tmp");
  const auto similar = world.target / ".am-export-not-a-uuid.tmp";
  const auto link = world.target / (".am-export-"+uuid()+".tmp");
  const auto external = world.home / "external";
  for (const auto& name : {old,young,busy,similar,external}) write(name,"orphan");
  const auto past = fs::file_time_type::clock::now()-std::chrono::hours(48);
  for (const auto& name : {old,busy,similar,external}) fs::last_write_time(name,past);
  std::error_code link_error; fs::create_symlink(external,link,link_error);
#ifdef _WIN32
  const auto handle = CreateFileW(am::paths::native(busy).c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
  ASSERT_NE(handle,INVALID_HANDLE_VALUE);
#else
  const int handle = ::open(busy.c_str(),O_RDONLY); ASSERT_NE(handle,-1); ASSERT_EQ(::flock(handle,LOCK_EX|LOCK_NB),0);
#endif
  const auto removed = value(am::storage::fs::cleanup_export_orphans(world.target));
#ifdef _WIN32
  CloseHandle(handle);
#else
  ::close(handle);
#endif
  EXPECT_EQ(removed,1u); EXPECT_FALSE(fs::exists(old)); EXPECT_TRUE(fs::exists(young)); EXPECT_TRUE(fs::exists(busy));
  EXPECT_TRUE(fs::exists(similar)); EXPECT_EQ(bytes(external),"orphan");
  if (!link_error) EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link)));
  else RecordProperty("symlink_creation_error",link_error.message());
}
TEST(Export, LiveTemporaryRemainsBusyThroughCheckpointEvenAfterWriterCloses) {
  RecordProperty("req","K-121|NFR-14.5.6");
  World world; const auto asset = world.create();
  auto snapshot = value(world.catalog->library().assets().snapshot(asset.ref));
  be::PrepareOptions options; options.target_dir = world.target;
  options.hooks.after_write = [&](const auto& temporary) {
    fs::last_write_time(temporary,fs::file_time_type::clock::now()-std::chrono::hours(48));
    EXPECT_EQ(value(am::storage::fs::cleanup_export_orphans(world.target)),0u);
    EXPECT_TRUE(fs::exists(temporary));
  };
  fs::path temporary;
  {
    auto prepared = value(be::prepare(snapshot,options)); temporary = prepared.temporary;
    EXPECT_EQ(value(am::storage::fs::cleanup_export_orphans(world.target)),0u);
    EXPECT_TRUE(fs::exists(temporary));
    prepared.owned = false;  // Model an aged crash artifact after releasing its lease.
  }
  EXPECT_EQ(value(am::storage::fs::cleanup_export_orphans(world.target)),1u);
  EXPECT_FALSE(fs::exists(temporary));
}
TEST(ExportStreaming, IncompressibleSizesRecordPeakAndResidualMemory) {
  RecordProperty("req","K-116|NFR-14.2.3");
  const auto memory = []() -> std::pair<std::uint64_t,std::uint64_t> {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info{}; info.cb = sizeof(info);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),&info,sizeof(info))) throw std::runtime_error("Cannot sample process memory");
    return {info.PeakWorkingSetSize,info.WorkingSetSize};
#else
    struct rusage usage{}; if (::getrusage(RUSAGE_SELF,&usage) != 0) throw std::runtime_error("Cannot sample peak memory");
    std::ifstream input("/proc/self/status"); std::string key; std::uint64_t resident = 0;
    while (input >> key) { if (key == "VmRSS:") { input >> resident; break; } std::string rest; std::getline(input,rest); }
    return {static_cast<std::uint64_t>(usage.ru_maxrss)*1024,resident*1024};
#endif
  };
  // Workload points, not normative performance thresholds. Record both points
  // instead of inventing an acceptable RAM increase. The I/O block assertion
  // checks the actual streaming contract independently of OS memory noise.
  for (unsigned mib : {16u,128u}) {
    World world; const auto source = world.home / "large.bin";
    std::ofstream input(am::paths::native(source),std::ios::binary);
    std::array<char,65536> block{}; std::uint32_t random = 0x12345678;
    for (std::uint64_t offset = 0; offset < static_cast<std::uint64_t>(mib)*1024*1024; offset += block.size()) {
      for (auto& byte : block) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = static_cast<char>(random); }
      input.write(block.data(),block.size());
    }
    input.close(); ASSERT_TRUE(input);
    repo::CreateRequest create; create.metadata = J{{"title","Streaming"}};
    repo::AddFiles add; add.input = {{"id",uuid()},{"imported_at","2026-11-26T00:00:00Z"},{"mode","strict"},
                                    {"completeness","complete"},{"objects",J::array()},{"entries",J::array()}};
    repo::FileInput file; file.path = "large.bin"; file.source = source; add.files.push_back(std::move(file));
    create.files.push_back(std::move(add)); const auto asset = value(world.catalog->create(create)).change;
    const auto before = memory(); std::uint64_t max_block = 0, observed = 0;
    ex::RunHooks hooks; hooks.storage.on_read = [&](auto n) { observed += n; max_block = std::max(max_block,n); };
    const auto started = std::chrono::steady_clock::now();
    const auto result = world.run(world.request(asset),hooks);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    const auto after = memory();
    ASSERT_EQ(row(result).at("status"),"exported") << result.dump();
    EXPECT_EQ(observed,fs::file_size(world.asset_path(asset))); EXPECT_LE(max_block,65536u);
    const auto prefix = "workload_"+std::to_string(mib)+"mib_";
    RecordProperty(prefix+"source_zip_bytes",std::to_string(observed));
    RecordProperty(prefix+"baseline_peak_bytes",std::to_string(before.first));
    RecordProperty(prefix+"peak_bytes",std::to_string(after.first));
    RecordProperty(prefix+"residual_bytes",std::to_string(after.second));
    RecordProperty(prefix+"seconds",std::to_string(elapsed));
    std::cout << prefix << "source_zip_bytes=" << observed << " baseline_peak_bytes=" << before.first
              << " peak_bytes=" << after.first << " residual_bytes=" << after.second << " seconds=" << elapsed << '\n';
    world.catalog.reset();
    EXPECT_TRUE(fs::remove(source)); EXPECT_TRUE(fs::remove(world.asset_path(asset)));
    EXPECT_TRUE(fs::remove(world.target / "Streaming.zip"));
  }
}
}  // namespace
