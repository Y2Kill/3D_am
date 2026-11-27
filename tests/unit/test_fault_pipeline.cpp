#include <gtest/gtest.h>
#include "fault_world.hpp"
#include "fault_vfs.hpp"
#include "fault_import.hpp"
#include "am/services/export/run.hpp"

namespace {
using namespace fault_test;
void insufficient(const am::Error& e) {
  EXPECT_EQ(e.code,am::Errc::insufficient_space)<<am::to_log_string(e);
  EXPECT_TRUE(e.allows(am::ErrorAction::free_space));
  EXPECT_TRUE(e.allows(am::ErrorAction::abort));
}
template<class T> void rejected(const am::Result<T>& r) {
  EXPECT_FALSE(r);
  if (!r) insufficient(r.error());
}
am::repository::RepositoryOptions options(am::catalog::Catalog& c) {
  am::repository::RepositoryOptions o;
  o.library_id=c.library().config().library_id; o.capabilities=c.library().capabilities();
  o.on_indexed=[&c](const am::repository::IndexedEvent& e) { take(c.indexer().refresh(e.ref.asset_id)); };
  return o;
}
}

TEST(FaultSqlitePipeline, JournalFullBeforeAndAfterReplacement) {
  RecordProperty("req","K-117|K-122|ADR-0007|ADR-0033");
  Json roots=Json::array();
  for (const bool after:{false,true}) for (const auto* actor:{"create","change","copy","ready","trash","restore","purge"}) {
    SCOPED_TRACE(actor);
    SCOPED_TRACE(after);
    fault_test::FullVfs vfs("state.sqlite");
    World w;
    if (std::string_view(actor)=="restore" || std::string_view(actor)=="purge") {
      auto c=take(am::catalog::Catalog::open(w.root,writable())); take(c.trash(w.asset.ref));
    }
    const auto protected_path=(std::string_view(actor)=="restore" || std::string_view(actor)=="purge") ? w.root/"trash"/(w.asset.ref.asset_id+".zip") : w.zip();
    const auto old=hash(protected_path),user=hash(w.root/".am/userdata.sqlite");
    {
      auto c=take(am::catalog::Catalog::open(w.root,writable()));
      auto o=options(c);
      if (after) o.test_hooks.commit.point=[&](am::commit::FaultPoint p)->am::Result<void> { if(p==am::commit::FaultPoint::after_replace)vfs.arm(); return {}; };
      c.library().assets()=take(am::repository::AssetRepository::open(w.root,o));
      if (!after) vfs.arm();
      const std::string_view operation=actor;
      if (operation=="create") rejected(c.create(request(w.source,"New asset")));
      else if (operation=="change") {
        am::repository::ChangeRequest r; r.ref=w.asset.ref; r.expected_rev=1;
        r.ops.emplace_back(am::repository::SetMetadata{"asset","title",std::optional<Json>{std::in_place,Json("Confirmed new revision")}}); rejected(c.change(r));
      } else if (operation=="copy") rejected(c.copy_independent({w.asset.ref,{},{}}));
      else if (operation=="ready") rejected(c.import_ready({fs::path(AM_GOLDEN_FIXTURES)/"pos-minimal/asset.zip",{},{}}));
      else if (operation=="trash") rejected(c.trash(w.asset.ref));
      else if (operation=="restore") rejected(c.restore(w.asset.ref));
      else if (operation=="purge") rejected(c.purge(w.asset.ref));
      EXPECT_GT(vfs.failures(),0);
      vfs.disarm();
    }
    if (!after || (std::string_view(actor)!="change" && std::string_view(actor)!="trash" && std::string_view(actor)!="restore" && std::string_view(actor)!="purge")) {
      EXPECT_EQ(hash(protected_path),old);
    }
    if(after && std::string_view(actor)=="trash") { EXPECT_EQ(hash(w.root/"trash"/(w.asset.ref.asset_id+".zip")),old); }
    if(after && std::string_view(actor)=="restore") { EXPECT_EQ(hash(w.zip()),old); }
    if(after && std::string_view(actor)=="purge") { EXPECT_FALSE(fs::exists(protected_path)); }
    EXPECT_EQ(hash(w.root/".am/userdata.sqlite"),user);
    roots.push_back(w.root.generic_string());
  }
  RecordProperty("libraries",roots.dump());
}

TEST(FaultSqlitePipeline, IndexFullAfterConfirmedCommitWarnsAndRemainsRecoverable) {
  RecordProperty("req","K-117|K-122|ADR-0007|ADR-0033");
  fault_test::FullVfs vfs("index.sqlite");
  World w;
  const auto old=hash(w.zip()),user=hash(w.root/".am/userdata.sqlite");
  {
    auto c=take(am::catalog::Catalog::open(w.root,writable()));
    vfs.arm();
    auto created=c.create(request(w.source,"Confirmed while index is full"));
    ASSERT_TRUE(created);
    EXPECT_TRUE(fs::exists(asset_path(w.root,created->change.ref.asset_id)));
    bool found=false;
    for (const auto& warning:created->warnings) if(warning.message_id=="am.catalog.index_update_failed") {
      found=true;
      EXPECT_EQ(warning.root_cause().code,am::Errc::insufficient_space);
      EXPECT_TRUE(warning.allows(am::ErrorAction::free_space));
    }
    EXPECT_TRUE(found);
    EXPECT_GT(vfs.failures(),0);
    vfs.disarm(); // Space can be freed before the writer closes; the independent oracle still calls sync.
  }
  EXPECT_EQ(hash(w.zip()),old); EXPECT_EQ(hash(w.root/".am/userdata.sqlite"),user);
  RecordProperty("libraries",Json::array({w.root.generic_string()}).dump());
}

TEST(FaultSqlitePipeline, IndexTransactionFull) {
  RecordProperty("req","K-117|K-122|ADR-0007|ADR-0033");
  Json roots=Json::array();
  for (const bool rebuild:{true,false}) {
    fault_test::FullVfs vfs("index.sqlite");
    World w;
    {
      auto c=take(am::catalog::Catalog::open(w.root,writable()));
      auto o=options(c); o.on_indexed={};
      auto repo=take(am::repository::AssetRepository::open(w.root,o));
      take(repo.create(request(w.source,"External committed asset")));
      vfs.arm();
      if (rebuild) rejected(c.indexer().rebuild()); else rejected(c.sync());
      EXPECT_GT(vfs.failures(),0);
      vfs.disarm();
    }
    roots.push_back(w.root.generic_string());
  }
  RecordProperty("libraries",roots.dump());
}

TEST(FaultSqlitePipeline, OperationLogFull) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  fault_test::FullVfs vfs("state.sqlite");
  World w;
  {
    auto c=take(am::catalog::Catalog::open(w.root,writable()));
    const auto before=take(c.operation_log().query()).size();
    am::db::LogEntry entry; entry.code="unconfirmed"; entry.level="info"; entry.message="not committed";
    vfs.arm_once();
    rejected(c.operation_log().append(entry));
    EXPECT_GT(vfs.failures(),0);
    EXPECT_EQ(take(c.operation_log().query()).size(),before);
  }
  RecordProperty("libraries",Json::array({w.root.generic_string()}).dump());
}
TEST(FaultSqlitePipeline, ImportJournalFull) {
  RecordProperty("req","K-117|K-122|ADR-0007|ADR-0033");
  Json roots=Json::array();
  for(const auto* mode:{"import_folder","import_zip","import_nested","import_between"}) {
    SCOPED_TRACE(mode);
    FullVfs vfs("state.sqlite"); World w;
    const auto old=hash(w.zip()),user=hash(w.root/".am/userdata.sqlite");
    {
      auto c=take(am::catalog::Catalog::open(w.root,writable()));
      Json fixture=Json::object();import_fixture(c,w.work,mode,fixture);
      bool armed=std::string_view(mode)!="import_between";
      std::optional<std::pair<fs::path,am::asset_zip::Sha256Digest>> confirmed;
      am::import::RunHooks hooks;
      hooks.before_attempt=[&](am::repository::CreateRequest&){if(armed)vfs.arm_once();};
      hooks.after_created=[&](std::string_view id)->am::Result<void>{auto p=asset_path(w.root,std::string(id));confirmed=std::make_pair(p,hash(p));armed=true;return {};};
      const auto report=import_result(c,fixture,hooks);
      EXPECT_GT(vfs.failures(),0);
      EXPECT_NE(report.dump().find("insufficient_space"),std::string::npos)<<report.dump();
      if(confirmed) { EXPECT_EQ(hash(confirmed->first),confirmed->second); }
      vfs.disarm();
    }
    EXPECT_EQ(hash(w.zip()),old);EXPECT_EQ(hash(w.root/".am/userdata.sqlite"),user);
    EXPECT_TRUE(fs::is_empty(w.root/".am/.am-tmp"));
    roots.push_back(w.root.generic_string());
  }
  RecordProperty("libraries",roots.dump());
}
TEST(FaultSqlitePipeline, ExportCheckpointFull) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  Json roots=Json::array();
  for(const bool full:{false,true}) {
    FullVfs vfs("state.sqlite");World w;
    const auto old=hash(w.zip()),user=hash(w.root/".am/userdata.sqlite");
    {
      auto c=take(am::catalog::Catalog::open(w.root,writable()));
      const auto output=w.work/"output";fs::create_directory(output);
      am::exporting::ExportRequest r;r.asset_ids={w.asset.ref.asset_id};r.target_dir=output;
      if(!full)r.presets={am::exporting::Preset::all};
      auto parent=take(am::exporting::enqueue_export(c,r));
      am::exporting::RunHooks hooks;hooks.storage.after_write=[&](const fs::path&){vfs.arm_once();};
      am::jobs::HandlerRegistry registry;
      am::jobs::ExecutorOptions o;o.auto_retries=0;o.limits={1,1,1,1,1,1,1,1};
      am::jobs::ResourceManager resources(o.limits);
      take(am::exporting::register_export_jobs(registry,c,resources,hooks));
      am::jobs::Executor executor(c.job_store(),resources,registry,o);take(executor.run_until_idle());
      const auto report=take(am::exporting::export_summary(c,parent));
      EXPECT_GT(vfs.failures(),0);
      EXPECT_EQ(report.at("assets").at(0).at("status"),"resource_error");
      EXPECT_NE(report.dump().find("insufficient_space"),std::string::npos)<<report.dump();
      EXPECT_TRUE(fs::is_empty(output));vfs.disarm();
    }
    EXPECT_EQ(hash(w.zip()),old);EXPECT_EQ(hash(w.root/".am/userdata.sqlite"),user);
    roots.push_back(w.root.generic_string());
  }
  RecordProperty("libraries",roots.dump());
}
TEST(FaultSqlitePipeline, RecoveryFull) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  FullVfs vfs("state.sqlite");World w;
  const auto user=hash(w.root/".am/userdata.sqlite");
  {
    auto c=take(am::catalog::Catalog::open(w.root,writable()));
    auto o=options(c);
    o.test_hooks.commit.point=[](am::commit::FaultPoint p)->am::Result<void>{
      if(p==am::commit::FaultPoint::after_replace)return am::Error::make(am::Errc::io_error,"Simulated interruption after replacement");
      return {};
    };
    c.library().assets()=take(am::repository::AssetRepository::open(w.root,o));
    am::repository::ChangeRequest r;r.ref=w.asset.ref;r.expected_rev=1;
    r.ops.emplace_back(am::repository::SetMetadata{"asset","title",std::optional<Json>{std::in_place,Json("Confirmed revision before recovery")}});
    EXPECT_FALSE(c.change(r));
  }
  const auto confirmed=hash(w.zip());
  vfs.arm();
  auto opened=am::catalog::Catalog::open(w.root,writable());
  ASSERT_TRUE(opened);
  bool reported=false;
  for(const auto& warning:opened->open_report().warnings)if(warning.root_cause().code==am::Errc::insufficient_space) {
    reported=true;EXPECT_TRUE(warning.allows(am::ErrorAction::free_space))<<am::to_log_string(warning);
  }
  EXPECT_TRUE(reported);
  EXPECT_GT(vfs.failures(),0);vfs.disarm();
  EXPECT_EQ(hash(w.zip()),confirmed);EXPECT_EQ(hash(w.root/".am/userdata.sqlite"),user);
  RecordProperty("libraries",Json::array({w.root.generic_string()}).dump());
}
