#include <gtest/gtest.h>
#include <map>
#include "fault_world.hpp"
#include "fault_sink.hpp"
#include "fault_import.hpp"
#include "am/storage/export/prepare.hpp"
#include "am/application/logging.hpp"

namespace {
using namespace fault_test;
void protected_files(const World& w,const am::asset_zip::Sha256Digest& zip,const am::asset_zip::Sha256Digest& user) {
  EXPECT_EQ(hash(w.zip()),zip);
  EXPECT_EQ(hash(w.root/".am/userdata.sqlite"),user);
  EXPECT_TRUE(take(take(am::db::StateJournal::open(w.root))->list()).empty());
  EXPECT_TRUE(fs::is_empty(w.root/".am/.am-tmp"));
}
void no_write(const World& w,const am::Error& e,const am::asset_zip::Sha256Digest& zip,const am::asset_zip::Sha256Digest& user) {
  EXPECT_EQ(e.code,am::Errc::insufficient_space) << am::to_log_string(e);
  EXPECT_NE(e.message_id.find("insufficient_space"),std::string::npos);
  EXPECT_TRUE(e.allows(am::ErrorAction::free_space));
  EXPECT_TRUE(e.allows(am::ErrorAction::abort));
  protected_files(w,zip,user);
}
Json ready_copy(bool during) {
  Json roots=Json::array();
  for (const auto* actor:{"ready","copy"}) {
    SCOPED_TRACE(actor);
    World w;
    const auto zip=hash(w.zip()),user=hash(w.root/".am/userdata.sqlite");
    auto failures=std::make_shared<std::atomic<unsigned>>(0);
    am::repository::RepositoryOptions o;
    o.library_id=w.asset.ref.library_id;
    o.capabilities=writable().capabilities.value();
    if (!during) o.test_hooks.commit.free_space=[](const fs::path&)->am::Result<std::uint64_t> { return 0; };
    else o.test_hooks.commit.create_sink=[failures](const fs::path& path)->am::Result<std::shared_ptr<am::asset_zip::RandomAccessSink>> {
      auto sink=std::make_shared<FullSink>(); sink->inner=take(am::asset_zip::file_sink(path)); sink->failures=failures;
      return std::static_pointer_cast<am::asset_zip::RandomAccessSink>(sink);
    };
    auto repo=take(am::repository::AssetRepository::open(w.root,o));
    if (std::string_view(actor)=="ready") {
      auto result=repo.import_ready({fs::path(AM_GOLDEN_FIXTURES)/"pos-minimal/asset.zip",{},{}});
      EXPECT_FALSE(result);
      if (!result) no_write(w,result.error(),zip,user);
    } else {
      auto result=repo.copy_independent({w.asset.ref,am::Uuid::v4().to_string(),{}});
      EXPECT_FALSE(result);
      if (!result) no_write(w,result.error(),zip,user);
    }
    EXPECT_EQ(failures->load()>0,during);
    roots.push_back(w.root.generic_string());
  }
  return roots;
}
}
TEST(FaultSpace, ReadyAndCopyPreflight) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  RecordProperty("libraries",ready_copy(false).dump());
}
TEST(FaultSpace, ReadyAndCopyDuringWrite) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  RecordProperty("libraries",ready_copy(true).dump());
}
TEST(FaultSpace, ExportDuringWrite) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  Json roots=Json::array();
  for (const bool full:{true,false}) {
    World w;
    const auto old=hash(w.zip()),user=hash(w.root/".am/userdata.sqlite");
    auto o=writable(); o.mode=am::library::Mode::read_only;
    auto library=take(am::library::Library::open(w.root,o));
    auto snapshot=take(library.assets().snapshot(w.asset.ref));
    am::storage::exporting::PrepareOptions options;
    options.target_dir=w.work/"output"; fs::create_directory(options.target_dir);
    options.full=full;
    if (!full) options.files.push_back({0,"result/part.stl"});
    auto failures=std::make_shared<std::atomic<unsigned>>(0);
    options.hooks.create_sink=[failures](const fs::path& path)->am::Result<std::shared_ptr<am::asset_zip::RandomAccessSink>> {
      auto sink=std::make_shared<FullSink>(); sink->inner=take(am::asset_zip::file_sink(path)); sink->failures=failures;
      return std::static_pointer_cast<am::asset_zip::RandomAccessSink>(sink);
    };
    auto result=am::storage::exporting::prepare(snapshot,options);
    ASSERT_FALSE(result);
    no_write(w,result.error(),old,user);
    EXPECT_GT(failures->load(),0u);
    EXPECT_TRUE(fs::is_empty(options.target_dir));
    roots.push_back(w.root.generic_string());
  }
  RecordProperty("libraries",roots.dump());
}
TEST(FaultSpace, SelectiveExportPreflight) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  World w;
  auto options=writable(); options.mode=am::library::Mode::read_only;
  auto library=take(am::library::Library::open(w.root,options));
  auto snapshot=take(library.assets().snapshot(w.asset.ref));
  const auto old=hash(w.zip()),user=hash(w.root/".am/userdata.sqlite");
  am::storage::exporting::PrepareOptions request;
  request.target_dir=w.work/"output"; fs::create_directory(request.target_dir);
  request.full=false; request.files.push_back({0,"result/part.stl"});
  request.hooks.available_space=[](const fs::path&) { return 0; };
  auto result=am::storage::exporting::prepare(snapshot,request);
  ASSERT_FALSE(result);
  no_write(w,result.error(),old,user);
  EXPECT_TRUE(fs::is_empty(request.target_dir));
  RecordProperty("libraries",Json::array({w.root.generic_string()}).dump());
}
TEST(FaultSpace, DiagnosticExportDuringWrite) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  World w;
  const auto old=hash(w.zip()),user=hash(w.root/".am/userdata.sqlite");
  am::application::DiagnosticOptions options;
  options.library=w.root; options.output=w.work/"output/diagnostic.zip";
  fs::create_directory(options.output.parent_path());
  auto failures=std::make_shared<std::atomic<unsigned>>(0);
  options.create_sink=[failures](const fs::path& path)->am::Result<std::shared_ptr<am::asset_zip::RandomAccessSink>> {
    auto sink=std::make_shared<FullSink>(); sink->inner=take(am::asset_zip::file_sink(path)); sink->failures=failures;
    return std::static_pointer_cast<am::asset_zip::RandomAccessSink>(sink);
  };
  auto result=am::application::export_diagnostics(options);
  ASSERT_FALSE(result);
  no_write(w,result.error(),old,user);
  EXPECT_GT(failures->load(),0u);
  EXPECT_TRUE(fs::is_empty(options.output.parent_path()));
  RecordProperty("libraries",Json::array({w.root.generic_string()}).dump());
}
TEST(FaultSpace, ImportInputs) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  Json roots=Json::array();
  for (const bool during:{false,true}) for (const auto* mode:{"import_folder","import_zip","import_nested","import_between"}) {
    SCOPED_TRACE(mode);
    SCOPED_TRACE(during);
    World w; const auto old=hash(w.zip()),user=hash(w.root/".am/userdata.sqlite");
    auto c=take(am::catalog::Catalog::open(w.root,writable()));
    Json fixture=Json::object(); import_fixture(c,w.work,mode,fixture);
    std::map<fs::path,am::asset_zip::Sha256Digest> sources;
    for(const auto& p:fs::recursive_directory_iterator(w.work/"import-source")) if(p.is_regular_file())sources.emplace(p.path(),hash(p.path()));
    auto armed=std::make_shared<std::atomic<bool>>(std::string_view(mode)!="import_between");
    auto failures=std::make_shared<std::atomic<unsigned>>(0);
    am::repository::RepositoryOptions o; o.library_id=w.asset.ref.library_id; o.capabilities=c.library().capabilities();
    o.on_indexed=[&](const am::repository::IndexedEvent& e){take(c.indexer().refresh(e.ref.asset_id));};
    o.test_hooks.commit.free_space=[armed,during](const fs::path&)->am::Result<std::uint64_t>{return !during && *armed ? 0 : UINT64_MAX;};
    o.test_hooks.commit.create_sink=[armed,failures,during](const fs::path& p)->am::Result<std::shared_ptr<am::asset_zip::RandomAccessSink>>{
      auto sink=take(am::asset_zip::file_sink(p));
      if(!during || !*armed)return sink;
      auto full=std::make_shared<FullSink>();full->inner=std::move(sink);full->failures=failures;
      return std::static_pointer_cast<am::asset_zip::RandomAccessSink>(full);
    };
    c.library().assets()=take(am::repository::AssetRepository::open(w.root,o));
    std::optional<std::pair<fs::path,am::asset_zip::Sha256Digest>> confirmed;
    am::import::RunHooks hooks;
    hooks.after_created=[&](std::string_view id)->am::Result<void>{const auto p=asset_path(w.root,std::string(id));confirmed=std::make_pair(p,hash(p));*armed=true;return {};};
    auto report=import_result(c,fixture,hooks);
    EXPECT_NE(report.dump().find("insufficient_space"),std::string::npos)<<report.dump();
    const auto& rows=report.at("candidates");
    std::size_t created=0; for(const auto& row:rows)if(row.at("status")=="created")++created;
    EXPECT_EQ(created,std::string_view(mode)=="import_between" ? 1u : 0u);
    if(confirmed) { EXPECT_EQ(hash(confirmed->first),confirmed->second); }
    for(const auto& [p,sha]:sources)EXPECT_EQ(hash(p),sha);
    EXPECT_EQ(failures->load()>0,during);
    protected_files(w,old,user);
    roots.push_back(w.root.generic_string());
  }
  RecordProperty("libraries",roots.dump());
}
