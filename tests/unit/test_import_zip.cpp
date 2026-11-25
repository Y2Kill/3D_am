#include <gtest/gtest.h>
#include <fstream>
#include <array>
#include <set>
#include <zlib.h>
#include "am/foundation/uuid.hpp"
#include "am/storage/archive/zip_input.hpp"
#include "am/storage/repository/repository.hpp"
#include "am/application/import_service.hpp"
#include "am/services/import/plan_view.hpp"
#include "am/services/import/run.hpp"
#include "zip_builder.hpp"

namespace {
namespace fs=std::filesystem;
namespace zip=am::asset_zip;
namespace input=am::archive;
template<class T> T take(am::Result<T> r) { if(!r) throw std::runtime_error(am::to_log_string(r.error())); return std::move(*r); }
fs::path install(const zip_test::Built& built) {
  const auto path=fs::path(AM_IMPORT_SCAN_ROOT)/"zip-input"/(am::Uuid::v4().to_string()+".zip");
  fs::create_directories(path.parent_path());
  std::ofstream out(path,std::ios::binary);
  out.write(reinterpret_cast<const char*>(built.bytes.data()),static_cast<std::streamsize>(built.bytes.size()));
  if(!out) throw std::runtime_error("ZIP fixture write failed");
  return path;
}
std::string read(input::ZipInput& archive,std::size_t index) {
  auto stream=take(archive.open_entry(index,true));
  std::string result;
  std::array<std::byte,65536> bytes{};
  for(;;) { const auto n=take(stream.read(bytes)); if(!n) break; result.append(reinterpret_cast<const char*>(bytes.data()),n); }
  EXPECT_TRUE(stream.verification());
  return result;
}
std::shared_ptr<input::ZipInputJob> job(input::ZipInputLimits limits={}) { return std::make_shared<input::ZipInputJob>(limits); }
zip_test::Bytes unicode_extra(const std::string& raw,const std::string& decoded,bool correct) {
  zip_test::Bytes value;
  zip_test::append(value,0x7075,2); zip_test::append(value,decoded.size()+5,2); zip_test::append(value,1,1);
  auto crc=static_cast<std::uint32_t>(::crc32(0,reinterpret_cast<const Bytef*>(raw.data()),static_cast<uInt>(raw.size())));
  zip_test::append(value,correct?crc:crc^1,4);
  for(unsigned char c:decoded) value.push_back(static_cast<std::byte>(c));
  return value;
}
am::import::ImportPlan scan_zip(const fs::path& path,am::import::ImportProfile profile={}) {
  am::import::ScanRequest r; r.roots={path};
  r.profile=profile.settings.is_null()?take(am::application::load_import_profile()):std::move(profile);
  auto plan=take(am::import::scan(r));
  const auto valid=am::import::parse_plan_view(am::import::serialize_plan(plan));
  if(!valid) throw std::runtime_error(am::to_log_string(valid.error()));
  fs::create_directories(AM_IMPORT_PLAN_CASES);
  std::ofstream(fs::path(AM_IMPORT_PLAN_CASES)/"zip-input.json",std::ios::binary)<<am::import::serialize_plan(plan);
  return plan;
}
} // namespace
namespace {
class ImportZipRun : public testing::Test {
 protected:
  fs::path work,source,library,plan_file;
  am::import::ImportPlan plan;
  std::optional<am::catalog::Catalog> catalog;
  void SetUp() override {
    work=fs::path(AM_IMPORT_SCAN_ROOT)/"zip-run"/am::Uuid::v4().to_string();
    source=work/"source"; library=work/"library"; plan_file=work/"plan.json";
    fs::create_directories(source);
    take(am::library::Library::init(library));
    am::library::OpenOptions options; options.application_version="test";
    options.capabilities.emplace(); options.capabilities->mode=am::repository::StorageMode::full;
    options.capabilities->filesystem="synthetic unit-test capabilities";
    catalog.emplace(take(am::catalog::Catalog::open(library,options)));
  }
  fs::path fixture(const std::vector<zip_test::Entry>& entries,std::string name="model.zip") {
    const auto target=source/name; fs::create_directories(target.parent_path());
    const auto built=zip_test::build(entries);
    std::ofstream out(target,std::ios::binary); out.write(reinterpret_cast<const char*>(built.bytes.data()),static_cast<std::streamsize>(built.bytes.size()));
    return target;
  }
  void scan(const fs::path& root,bool hash=false) {
    am::import::ScanRequest r; r.roots={root}; r.hash_inputs=hash; r.profile=take(am::application::load_import_profile());
    plan=take(am::import::scan(r));
  }
  void include() {
    for(auto& c:plan.value["candidates"]) {
      c["action"]="import";
      for(auto& e:c["entries"]) if(e["decision"]=="ask") e["decision"]="include";
    }
  }
  am::import::QueuedRun enqueue() {
    std::ofstream(plan_file,std::ios::binary)<<am::import::serialize_plan(plan);
    return take(am::import::enqueue_run(*catalog,plan_file));
  }
  am::import::PlanJson execute(const am::import::QueuedRun& q,am::import::RunHooks hooks={},int retries=0) {
    am::jobs::HandlerRegistry registry;
    if(auto r=am::import::register_import_jobs(registry,*catalog,"synthetic-test",std::move(hooks));!r) throw std::runtime_error(am::to_log_string(r.error()));
    am::jobs::ExecutorOptions options; options.auto_retries=retries;
    am::jobs::ResourceManager resources(options.limits);
    am::jobs::Executor executor(catalog->job_store(),resources,registry,options);
    take(executor.run_until_idle());
    return take(am::import::run_summary(*catalog,q))["candidates"][0];
  }
  am::repository::AssetRef ref(const am::import::PlanJson& row) {return {catalog->library().config().library_id,row.at("asset_id")};}
  am::import::PlanJson manifest(const am::import::PlanJson& row) {
    const auto snapshot=take(catalog->library().assets().snapshot(ref(row)));
    return am::import::PlanJson::parse(snapshot.manifest().known().dump());
  }
  std::multiset<std::string> payloads(const am::import::PlanJson& row) {
    const auto snapshot=take(catalog->library().assets().snapshot(ref(row)));
    zip::ReadBudget budget(10000000); std::multiset<std::string> result;
    for(const auto& e:snapshot.archive().entries()) if(e.raw_name.starts_with("content/")) {
      auto stream=take(snapshot.archive().open_entry(e.index,budget,true)); std::string bytes; std::array<std::byte,65536> block{};
      for(;;) {const auto n=take(stream.read(block)); if(!n) break; bytes.append(reinterpret_cast<const char*>(block.data()),n);}
      result.insert(std::move(bytes));
    }
    return result;
  }
  void clean(bool empty=false) {
    const auto tmp=library/".am/.am-tmp";
    // T-032: the shared parent of temporary areas may remain, but it must be empty.
    if(fs::exists(tmp/"import")) EXPECT_TRUE(fs::is_empty(tmp/"import"));
    if(fs::exists(tmp)) for(const auto& item:fs::directory_iterator(tmp)) EXPECT_EQ(item.path().filename(),"import");
    if(empty) {
      EXPECT_TRUE(take(catalog->indexer().list()).empty());
      EXPECT_TRUE(take(take(am::db::StateJournal::open(library))->list()).empty());
    }
    am::catalog::LibraryVerifyOptions options; options.level=am::catalog::LibraryVerifyLevel::full;
    const auto report=take(catalog->verify(options)); EXPECT_EQ(report.corrupt,0u); EXPECT_EQ(report.unreadable,0u); EXPECT_FALSE(report.database_failed);
  }
};
}
TEST_F(ImportZipRun, StoredDeflateAndDuplicateRecordsPreserveDistinctBytesAndVerify) {
  RecordProperty("req","SC-1|K-5|K-6|ADR-0025");
  zip_test::Entry a,b,d; a.name=b.name="same.bin"; a.data="first"; b.data="second"; b.method=8; d.name="empty/"; d.data="";
  const auto file=fixture({a,b,d}); scan(file); include();
  const auto q=enqueue(); const auto row=execute(q);
  ASSERT_EQ(row["status"],"created")<<row.dump();
  EXPECT_EQ(payloads(row),(std::multiset<std::string>{"first","second"}));
  const auto m=manifest(row); const auto& input=m["inputs"][0];
  ASSERT_EQ(input["objects"].size(),1u); EXPECT_EQ(input["objects"][0]["kind"],"archive");
  EXPECT_EQ(input["objects"][0]["sha256"].get<std::string>().size(),64u);
  EXPECT_EQ(input["objects"][0]["relative_path"],"model.zip");
  for(std::size_t i=0;i<2;++i) {
    EXPECT_EQ(input["entries"][i]["result"],"included_renamed"); EXPECT_EQ(input["entries"][i]["rename_reason"],"collision");
    EXPECT_TRUE(input["entries"][i].contains("path_raw")); EXPECT_TRUE(input["entries"][i].contains("crc32"));
  }
  EXPECT_EQ(execute(enqueue())["status"],"already_imported");
  clean();
}
TEST_F(ImportZipRun, NestedPreserveAndSpecializedDeflateFollowPlanWithoutOpeningInnerArchive) {
  RecordProperty("req","SC-1|SC-10|K-6");
  const auto inner=zip_test::build({zip_test::Entry{}}).bytes;
  const std::string bytes(reinterpret_cast<const char*>(inner.data()),inner.size());
  zip_test::Entry a,b,r; a.name="model.3mf"; b.name="inner.zip"; a.data=b.data=bytes; a.method=8;
  r.name="inner.rar"; r.data=std::string("Rar!\x1a\x07\0",7);
  scan(fixture({a,b,r}));
  auto q=enqueue(); EXPECT_EQ(execute(q)["status"],"deferred");
  // T-032: the default profile expands a listed nested ZIP; choosing include preserves it as a file.
  EXPECT_EQ(plan.value["candidates"][0]["entries"][2]["decision"],"expand");
  EXPECT_EQ(plan.value["candidates"][0]["entries"][4]["decision"],"ask");
  include(); plan.value["candidates"][0]["entries"][2]["decision"]="include"; plan.value["candidates"][0]["entries"][4]["decision"]="exclude";
  q=enqueue(); const auto row=execute(q); ASSERT_EQ(row["status"],"created")<<row.dump();
  const auto m=manifest(row); const auto& objects=m["inputs"][0]["objects"];
  ASSERT_EQ(objects.size(),3u); EXPECT_EQ(objects[1]["name"],"inner.zip"); EXPECT_EQ(objects[1]["nested_action"],"preserve");
  EXPECT_EQ(objects[2]["container"],"rar4"); EXPECT_EQ(objects[2]["nested_action"],"exclude");
  EXPECT_EQ(payloads(row),(std::multiset<std::string>{bytes,bytes})); clean();
}
TEST_F(ImportZipRun, FilesystemArchiveHasNoParentEntryAndExpansionPrefixCollisionsAreRenamed) {
  RecordProperty("req","SC-1|K-5|K-6");
  zip_test::Entry a; a.name="part.bin"; a.data="zip bytes";
  fixture({a},"sub/model.zip"); fs::create_directories(source/"sub/model");
  std::ofstream(source/"sub/model/part.bin",std::ios::binary)<<"filesystem bytes";
  scan(source); include(); const auto row=execute(enqueue()); ASSERT_EQ(row["status"],"created")<<row.dump();
  const auto m=manifest(row); const auto& in=m["inputs"][0]; ASSERT_EQ(in["objects"].size(),2u);
  EXPECT_EQ(in["objects"][1]["relative_path"],"sub/model.zip"); EXPECT_FALSE(in["objects"][1].contains("parent_object_id"));
  unsigned renamed=0;
  for(const auto& e:in["entries"]) { EXPECT_NE(e["path"],"sub/model.zip"); if(e.value("rename_reason",std::string{})=="collision") ++renamed; }
  EXPECT_EQ(renamed,1u); EXPECT_EQ(payloads(row),(std::multiset<std::string>{"zip bytes","filesystem bytes"})); clean();
}
TEST_F(ImportZipRun, OuterExcludeOverridesInnerAskAndAvoidsAllArchiveReads) {
  RecordProperty("req","SC-1|K-5|K-6|ADR-0011");
  zip_test::Entry a; a.name="../unsafe.bin"; a.data="payload";
  const auto file=fixture({a}); scan(file); auto& c=plan.value["candidates"][0]; c["action"]="import";
  c["entries"][0]["decision"]="exclude";
  am::import::RunHooks hooks; hooks.on_open=[&](const auto& p){EXPECT_NE(p,file);};
  const auto row=execute(enqueue(),hooks); ASSERT_EQ(row["status"],"created")<<row.dump();
  const auto m=manifest(row); EXPECT_EQ(m["inputs"][0]["entries"][0]["result"],"excluded");
  EXPECT_EQ(m["inputs"][0]["entries"][0]["exclusion"]["by"],"user"); EXPECT_TRUE(payloads(row).empty()); clean();
}
TEST_F(ImportZipRun, UnsupportedRecordsStrictFailPartialAccountAndExcludedAreNeverOpened) {
  RecordProperty("req","SC-1|K-5|K-6|ADR-0011");
  zip_test::Entry a,c; a.name="encrypted"; a.flags|=1;
  std::vector<zip_test::Entry> entries{a};
  for(const auto method:{12,14,93,99}) {zip_test::Entry e;e.name="method"+std::to_string(method);e.method=static_cast<std::uint16_t>(method);e.compressed=zip_test::Bytes{};e.declared_size=0;entries.push_back(e);}
  c.name="good"; c.data="good"; entries.push_back(c); scan(fixture(entries)); include();
  const auto strict=execute(enqueue()); EXPECT_EQ(strict["status"],"failed"); clean(true);
  plan.value["mode"]="partial";
  unsigned opened=0; am::import::RunHooks hooks; hooks.before_archive_read=[&](auto,auto index)->am::Result<void>{EXPECT_EQ(index,5u);++opened;return {};};
  const auto row=execute(enqueue(),hooks); ASSERT_EQ(row["status"],"created")<<row.dump(); EXPECT_GT(opened,0u);
  const auto m=manifest(row); EXPECT_EQ(m["inputs"][0]["completeness"],"incomplete");
  for(std::size_t i=0;i<5;++i) EXPECT_EQ(m["inputs"][0]["entries"][i]["result"],"unavailable");
  EXPECT_EQ(payloads(row),(std::multiset<std::string>{"good"})); clean();
  plan.value["mode"]="strict";
  for(std::size_t i=1;i<=5;++i) plan.value["candidates"][0]["entries"][i]["decision"]="exclude";
  const auto excluded=execute(enqueue(),hooks); ASSERT_EQ(excluded["status"],"created")<<excluded.dump(); clean();
}
TEST_F(ImportZipRun, ActualByteLimitFailsBothModesWithoutAnAssetOrPartialRetry) {
  RecordProperty("req","SC-1|K-6|ADR-0011");
  zip_test::Entry a; a.data="1234567890"; scan(fixture({a})); include();
  for(const auto& budget:{"15","10"}) for(const auto& mode:{"strict","partial"}) {
    plan.value["profile"]["limits"]["max_unpacked_bytes"]=budget;
    plan.value["mode"]=mode;
    unsigned attempts=0; am::import::RunHooks hooks; hooks.before_attempt=[&](auto&){++attempts;};
    const auto row=execute(enqueue(),hooks,2); EXPECT_EQ(row["status"],"failed")<<row.dump();
    ASSERT_EQ(row.at("error").at("message_id"),"am.import.archive.limit_exceeded")<<row.dump();
    EXPECT_EQ(row.at("error").at("code"),"am.unsupported");
    ASSERT_TRUE(row.contains("problem"))<<row.dump(); EXPECT_EQ(row["problem"]["limit"],"max_unpacked_bytes");
    EXPECT_EQ(row["problem"]["source"],"plan"); EXPECT_EQ(attempts,1u); clean(true);
  }
}
TEST_F(ImportZipRun, CrcFailureIsFailedInPartialAndAbortsStrictWithoutRetry) {
  RecordProperty("req","SC-1|K-5|K-6");
  zip_test::Entry a,b; a.name="bad"; a.data=std::string(1000,'x'); a.method=8; a.declared_crc=0; b.name="good"; b.data="good";
  scan(fixture({a,b})); include();
  unsigned attempts=0; am::import::RunHooks hooks; hooks.before_attempt=[&](auto&){++attempts;};
  auto row=execute(enqueue(),hooks); EXPECT_EQ(row["status"],"failed"); EXPECT_EQ(attempts,1u); clean(true);
  plan.value["mode"]="partial"; attempts=0; row=execute(enqueue(),hooks);
  ASSERT_EQ(row["status"],"created")<<row.dump(); EXPECT_EQ(attempts,2u);
  const auto m=manifest(row); EXPECT_EQ(m["inputs"][0]["entries"][0]["result"],"failed");
  EXPECT_EQ(m["inputs"][0]["completeness"],"incomplete"); EXPECT_EQ(payloads(row),(std::multiset<std::string>{"good"})); clean();
}
TEST_F(ImportZipRun, UnsafeAndGuessedNamesRemainAuditableAndLinksNeverMaterialize) {
  RecordProperty("req","SC-1|K-6|ADR-0012|ADR-0035");
  std::vector<zip_test::Entry> entries;
  for(const auto& name:std::vector<std::string>{"../x","/abs","C:\\x","\\\\srv\\x","a\\b","a//b",std::string("x\0y",3),"A.txt","a.txt","\xc3\xa9","e\xcc\x81"}) {
    zip_test::Entry e; e.name=name; e.data=name; entries.push_back(e);
  }
  zip_test::Entry guessed; guessed.name=std::string("\x8c\xae\xa4\xa5\xab\xec.bin",10); guessed.flags=0; guessed.data="guess"; entries.push_back(guessed);
  zip_test::Entry link,special; link.name="symlink"; link.made_by=0x314; link.attributes=0120777u<<16;
  special.name="fifo"; special.made_by=0x314; special.attributes=0010644u<<16; entries.push_back(link); entries.push_back(special);
  scan(fixture(entries)); include();
  const auto row=execute(enqueue()); ASSERT_EQ(row["status"],"created")<<row.dump();
  const auto m=manifest(row); const auto& in=m["inputs"][0]; ASSERT_EQ(in["entries"].size(),entries.size());
  for(std::size_t i=0;i<11;++i) {EXPECT_EQ(in["entries"][i]["result"],"included_renamed"); EXPECT_TRUE(in["entries"][i].contains("path_raw"));}
  EXPECT_TRUE(in["objects"][0]["name_encoding"].get<std::string>().find("heuristic")!=std::string::npos);
  EXPECT_EQ(in["entries"][12]["result"],"not_processed"); EXPECT_EQ(in["entries"][13]["problem"]["code"],"special_not_materialized");
  EXPECT_EQ(payloads(row).size(),12u); clean();
}
TEST_F(ImportZipRun, ArchiveFreshnessChecksPlanListingShaAndPostCommitStat) {
  RecordProperty("req","SC-1|K-6|NFR-14.4.3");
  zip_test::Entry a; a.name="part"; a.data="before"; const auto file=fixture({a}); scan(file,true); include();
  auto q=enqueue(); const auto timestamp=fs::last_write_time(file);
  a.data="after!"; fixture({a}); fs::last_write_time(file,timestamp);
  auto row=execute(q); EXPECT_EQ(row["status"],"failed"); EXPECT_EQ(row["error"]["message_id"],"am.import.source_changed"); clean(true);
  scan(file,true); include(); q=enqueue();
  const auto same_time=fs::last_write_time(file);
  auto broken=zip_test::build({a}); broken.bytes[broken.central[0]]=std::byte{0};
  {std::ofstream out(file,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(broken.bytes.data()),static_cast<std::streamsize>(broken.bytes.size()));}
  fs::last_write_time(file,same_time);
  row=execute(q); EXPECT_EQ(row["status"],"failed"); EXPECT_EQ(row["error"]["message_id"],"am.import.source_changed"); clean(true);
  fixture({a});
  scan(file); include(); q=enqueue();
  am::import::RunHooks hooks; hooks.after_created=[&](auto)->am::Result<void> {
    fs::last_write_time(file,fs::last_write_time(file)+std::chrono::seconds(2)); return {};
  };
  row=execute(q,hooks); EXPECT_EQ(row["status"],"failed"); EXPECT_TRUE(row["in_trash"]);
  EXPECT_EQ(row["error"]["message_id"],"am.import.source_changed_during_import");
  EXPECT_TRUE(take(catalog->library().assets().snapshot(ref(row),true)).in_trash()); clean();
}
TEST_F(ImportZipRun, DurableParametersDoNotGrowWithArchiveEntryCountAndLegacyV1Defers) {
  RecordProperty("req","SC-1|K-5|K-121");
  std::vector<zip_test::Entry> entries; std::size_t small=0;
  for(const auto count:{10,10000}) {
    entries.clear();
    for(int i=0;i<count;++i) {zip_test::Entry e; e.name="f"+std::to_string(i); e.data=""; entries.push_back(e);}
    scan(fixture(entries)); include(); const auto q=enqueue(); am::contracts::JobFilter f; f.parent_id=q.job_id;
    const auto children=take(catalog->job_store().list(f)); ASSERT_EQ(children.size(),1u);
    if(count==10) small=children[0].params_json.size(); else EXPECT_EQ(children[0].params_json.size(),small);
  }
  plan.value["version"]=1; plan.value.erase("archives"); plan.value.erase("archive_entries"); plan.value["profile"].erase("limits");
  plan.value["profile"]["not_applied"]=am::import::PlanJson::array();
  auto& decisions=plan.value["candidates"][0]["entries"];
  decisions.erase(std::remove_if(decisions.begin(),decisions.end(),[](const auto& e){return e.contains("archive_entry");}),decisions.end());
  const auto legacy=enqueue();
  const auto row=take(am::import::run_summary(*catalog,legacy))["candidates"][0];
  am::contracts::JobFilter filter; filter.parent_id=legacy.job_id; EXPECT_TRUE(take(catalog->job_store().list(filter)).empty());
  EXPECT_EQ(row["status"],"deferred"); EXPECT_EQ(row["message_id"],"am.import.run.archive_plan_rescan_required");
}
TEST_F(ImportZipRun, UnreadableArchiveRequiresExplicitExcludeAndRetainsItsObjectWithoutInventedEntries) {
  RecordProperty("req","SC-1|SC-13|K-5|K-6");
  zip_test::Entry e; auto built=zip_test::build({e}); built.bytes.resize(built.cd);
  const auto file=source/"broken.zip";
  {std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(built.bytes.data()),static_cast<std::streamsize>(built.bytes.size()));}
  scan(file); ASSERT_EQ(plan.value["archives"][0]["status"],"unreadable");
  EXPECT_EQ(execute(enqueue())["status"],"deferred"); include();
  for(const auto& mode:{"strict","partial"}) {
    plan.value["mode"]=mode; const auto row=execute(enqueue()); EXPECT_EQ(row["status"],"failed");
    EXPECT_EQ(row["error"]["message_id"],"am.import.run.archive_unreadable"); clean(true);
  }
  plan.value["candidates"][0]["entries"][0]["decision"]="exclude";
  const auto row=execute(enqueue()); ASSERT_EQ(row["status"],"created")<<row.dump();
  EXPECT_TRUE(row.contains("archive_problems")); const auto m=manifest(row);
  EXPECT_EQ(m["inputs"][0]["objects"].size(),1u); EXPECT_TRUE(m["inputs"][0]["entries"].empty()); clean();
}
TEST_F(ImportZipRun, ExcludingArchiveDirectoryCascadesAndRecordsUseTheOuterRuleProvenance) {
  RecordProperty("req","SC-1|K-5|K-6|ADR-0011");
  zip_test::Entry d,f; d.name="dir/"; d.data=""; f.name="dir/a";
  scan(fixture({d,f})); include();
  auto& c=plan.value["candidates"][0]; c["entries"][1]["decision"]="exclude";
  c["entries"][1]["rule_id"]="macos.macosx_dir"; c["entries"][1]["confidence"]="certain";
  const auto row=execute(enqueue()); ASSERT_EQ(row["status"],"created")<<row.dump();
  const auto m=manifest(row); for(const auto& e:m["inputs"][0]["entries"]) {EXPECT_EQ(e["result"],"excluded");EXPECT_EQ(e["exclusion"]["by"],"auto");}
  clean();
}
TEST_F(ImportZipRun, RatioTimeAndEntryLimitsCannotBeBypassedInPartialMode) {
  RecordProperty("req","SC-1|K-6|ADR-0011");
  zip_test::Entry a,b; a.name="a"; a.data=std::string(2*1024*1024,'x'); a.method=8; b.name="b";
  const auto file=fixture({a,b});
  for(const auto& name:{"max_ratio","max_seconds_per_container","max_entries"}) {
    scan(file); include();
    if(std::string_view(name)=="max_ratio") plan.value["profile"]["limits"][name]=2;
    else plan.value["profile"]["limits"][name]="1";
    for(const auto& mode:{"strict","partial"}) {
      plan.value["mode"]=mode;
      am::import::RunHooks hooks; std::chrono::steady_clock::time_point time{};
      if(std::string_view(name)=="max_seconds_per_container") hooks.archive_clock=[&]{time+=std::chrono::seconds(2);return time;};
      const auto row=execute(enqueue(),hooks,2); ASSERT_EQ(row["status"],"failed")<<row.dump();
      ASSERT_TRUE(row.contains("problem"))<<row.dump(); EXPECT_EQ(row["problem"]["limit"],name);
      EXPECT_EQ(row["error"]["code"],"am.unsupported"); EXPECT_TRUE(row["problem"].contains("effective_value")); EXPECT_EQ(row["problem"]["source"],"plan");
      const auto stored=take(catalog->job_store().get(row.at("job_id").get<std::string>())); ASSERT_TRUE(stored); EXPECT_EQ(stored->state,am::contracts::JobState::failed); EXPECT_TRUE(stored->result_json);
      clean(true);
    }
  }
}
TEST_F(ImportZipRun, MachineAllocationFailureKeepsResourceErrorAndDoesNotBecomeAnInputLimit) {
  RecordProperty("req","SC-1|K-6|ADR-0033");
  zip_test::Entry a; scan(fixture({a})); include(); const auto q=enqueue();
  am::import::RunHooks hooks; hooks.before_archive_read=[](auto,auto)->am::Result<void>{throw std::bad_alloc();};
  const auto row=execute(q,hooks); EXPECT_EQ(row["status"],"failed"); EXPECT_FALSE(row.contains("problem"));
  am::contracts::JobFilter filter; filter.parent_id=q.job_id; const auto children=take(catalog->job_store().list(filter));
  ASSERT_EQ(children.size(),1u); EXPECT_EQ(children[0].state,am::contracts::JobState::resource_error); EXPECT_FALSE(children[0].result_json);
  ASSERT_TRUE(children[0].error_json); EXPECT_NE(children[0].error_json->find("am.resource_exhausted"),std::string::npos); clean(true);
}
TEST_F(ImportZipRun, SnapshotLimitsClampToUnconditionalAndDoNotRereadChangedProfile) {
  RecordProperty("req","SC-1|K-6|ADR-0011");
  zip_test::Entry a; const auto file=fixture({a});
  const auto profile=work/"profile.toml";
  {std::ofstream out(profile);out<<"id=\"custom\"\nversion=1\n[limits]\nmax_entries=200000\n";}
  am::import::ScanRequest request; request.roots={file}; request.profile=take(am::application::load_import_profile(profile.string()));
  plan=take(am::import::scan(request)); include();
  {std::ofstream out(profile,std::ios::trunc);out<<"broken TOML [";}
  const auto row=execute(enqueue()); ASSERT_EQ(row["status"],"created")<<row.dump();
  EXPECT_EQ(row["effective_limits"]["max_entries"]["value"],"100000"); EXPECT_EQ(row["effective_limits"]["max_entries"]["source"],"unconditional"); clean();
}
TEST_F(ImportZipRun, RarSevenZipAndZipVolumeSetsRemainDeferredWithoutChildJobs) {
  RecordProperty("req","SC-2|SC-6|K-6");
  for(const auto& signature:{std::string("Rar!\x1a\x07\0",7),std::string("7z\xbc\xaf\x27\x1c",6)}) {
    const auto file=source/"unsupported.bin";
    {std::ofstream out(file,std::ios::binary|std::ios::trunc);out<<signature;}
    scan(file); include(); const auto q=enqueue();
    const auto row=take(am::import::run_summary(*catalog,q))["candidates"][0];
    EXPECT_EQ(row["status"],"deferred"); EXPECT_EQ(row["message_id"],"am.import.run.archive_input_unsupported");
    am::contracts::JobFilter filter; filter.parent_id=q.job_id; EXPECT_TRUE(take(catalog->job_store().list(filter)).empty());
  }
  zip_test::Entry a; fixture({a},"split/base.zip");
  {std::ofstream out(source/"split/base.z01",std::ios::binary);out<<"first volume";}
  scan(source/"split"); include(); const auto q=enqueue();
  const auto row=take(am::import::run_summary(*catalog,q))["candidates"][0];
  EXPECT_EQ(row["status"],"deferred"); EXPECT_EQ(row["message_id"],"am.import.run.volume_set_unsupported");
  am::contracts::JobFilter filter; filter.parent_id=q.job_id; EXPECT_TRUE(take(catalog->job_store().list(filter)).empty());
}
TEST(ImportZipPlan, LimitsRawFieldsAndMetadataAreValidatedBeforeEnqueue) {
  RecordProperty("req","SC-1|K-6|ADR-0038");
  zip_test::Entry e; const auto plan=scan_zip(install(zip_test::build({e}))).value;
  auto invalid=[&](am::import::PlanJson value,const std::string& pointer) {
    const auto result=am::import::parse_plan_view(value.dump()); ASSERT_FALSE(result)<<pointer;
    EXPECT_EQ(result.error().message_id,"am.import.plan_invalid"); EXPECT_TRUE(result.error().ctx.detail.find(pointer)!=std::string::npos)<<am::to_log_string(result.error());
  };
  auto value=plan; value["profile"].erase("limits"); invalid(value,"/profile");
  for(const auto& bad:am::import::PlanJson::array({"0","-1",0,true,"18446744073709551616"})) {
    value=plan; value["profile"]["limits"]["max_entries"]=bad; invalid(value,"/profile/limits/max_entries");
  }
  value=plan; value["profile"]["limits"]["extra"]="1"; invalid(value,"/profile/limits");
  for(const auto& raw:{"AA","0","xz"}) {value=plan;value["archive_entries"][0]["path_raw"]=raw;invalid(value,"/archive_entries/0/path_raw");}
  value=plan; value["archive_entries"][0]["path_raw"]=std::string(40000,'a'); EXPECT_TRUE(am::import::parse_plan_view(value.dump()));
  const auto schema_file=fs::path(AM_MANIFEST_SCHEMA).parent_path()/"import-plan.v3.schema.json";
  std::ifstream source(schema_file,std::ios::binary);
  const auto schema=am::import::PlanJson::parse(std::string{std::istreambuf_iterator<char>(source),{}});
  std::set<std::string> actual,handled;
  std::function<void(const am::import::PlanJson&,std::string)> visit=[&](const auto& item,std::string pointer) {
    if(item.is_object()) {
      const auto pattern=item.value("pattern",std::string{});
      if(pattern=="^([0-9a-f]{2})+$" || pattern=="^([0-9a-f]{2})*$") actual.insert(pointer);
      for(auto it=item.begin();it!=item.end();++it) visit(it.value(),pointer+"/"+it.key());
    } else if(item.is_array()) for(std::size_t i=0;i<item.size();++i) visit(item[i],pointer+"/"+std::to_string(i));
  };
  visit(schema,""); for(const auto field:am::import::plan_raw_schema_fields) handled.insert(std::string(field));
  handled.insert(std::string(am::import::plan_archive_raw_schema_field)); EXPECT_EQ(actual,handled);
}
TEST(ZipInput, DuplicateNamesRemainDistinctAndAssetModeRemainsStrict) {
  RecordProperty("req","K-2|K-6|ADR-0011|ADR-0025");
  zip_test::Entry a,b; a.name=b.name="same.bin"; a.data="first"; b.data="second";
  const auto path=install(zip_test::build({a,b}));
  auto strict=zip::ZipArchive::open(take(zip::file_source(path)));
  ASSERT_FALSE(strict); EXPECT_EQ(strict.error().message_id,"am.zip.duplicate_name");
  auto archive=take(input::ZipInput::open(path,job()));
  ASSERT_EQ(archive.entries().size(),2u);
  for(const auto& e:archive.entries()) EXPECT_EQ(e.path_problem,"duplicate_entry_name");
  EXPECT_EQ(read(archive,0),"first"); EXPECT_EQ(read(archive,1),"second");
}
TEST(ZipInput, DuplicateLocalHeadersRemainRejectedInBothProfiles) {
  RecordProperty("req","ADR-0011|K-2");
  zip_test::Entry a,b; a.name=b.name="same";
  auto bytes=zip_test::build({a,b}); zip_test::put(bytes.bytes,bytes.central[1]+42,bytes.local[0],4);
  const auto path=install(bytes);
  const auto strict=zip::ZipArchive::open(take(zip::file_source(path)));
  const auto archive=input::ZipInput::open(path,job());
  ASSERT_FALSE(strict); ASSERT_FALSE(archive);
  EXPECT_EQ(strict.error().message_id,"am.zip.overlapping_entries");
  EXPECT_EQ(archive.error().message_id,strict.error().message_id);
}
TEST(ZipInput, NameDecodingRetainsRawBytesAndExplainsHeuristics) {
  RecordProperty("req","K-6|ADR-0012");
  std::vector<zip_test::Entry> entries(6);
  entries[0].name="utf8-\xc3\xa9";
  entries[1].name="valid-\xc3\xa9"; entries[1].flags=0;
  entries[2].name="raw"; entries[2].flags=0; entries[2].central_extra=unicode_extra("raw","unicode-\xc3\xa9",true);
  entries[3].name="fallback"; entries[3].flags=0; entries[3].central_extra=unicode_extra("fallback","ignored",false);
  entries[4].name=std::string("\x8c\xae\xa4\xa5\xab\xec.bin",10); entries[4].flags=0;
  entries[5].name="bad-\xff";
  const auto path=install(zip_test::build(entries));
  auto archive=take(input::ZipInput::open(path,job()));
  auto e=archive.entries();
  EXPECT_EQ(e[0].name_encoding,"utf-8(bit11)"); EXPECT_TRUE(e[0].encoding_exact);
  EXPECT_EQ(e[1].name_encoding,"utf-8(valid)");
  EXPECT_EQ(e[2].name_encoding,"utf-8(0x7075)"); EXPECT_EQ(e[2].path,"unicode-\xc3\xa9");
  EXPECT_EQ(e[3].path,"fallback"); EXPECT_EQ(e[3].problem_code,"unicode_path_crc_mismatch");
  EXPECT_FALSE(e[4].encoding_exact); EXPECT_TRUE(e[4].encoding_confidence); EXPECT_TRUE(e[4].name_encoding.ends_with("(heuristic)"));
  EXPECT_EQ(e[5].problem_code,"name_invalid_utf8"); EXPECT_EQ(e[5].path,"bad-\xef\xbf\xbd");
  for(std::size_t i=0;i<entries.size();++i) EXPECT_EQ(e[i].info.raw_name,entries[i].name);
}
TEST(ZipInput, UnsafePathsAndCaseOrNormalizationCollisionsAreListed) {
  RecordProperty("req","K-6|ADR-0012|ADR-0035");
  const std::vector<std::string> names{"../x","/abs","C:\\x","\\\\srv\\x","a\\b","a//b",std::string("x\0y",3),"A.txt","a.txt","\xc3\xa9","e\xcc\x81"};
  std::vector<zip_test::Entry> entries;
  for(const auto& n:names) { zip_test::Entry e; e.name=n; entries.push_back(e); }
  auto archive=take(input::ZipInput::open(install(zip_test::build(entries)),job()));
  ASSERT_EQ(archive.entries().size(),names.size());
  for(std::size_t i=0;i<7;++i) EXPECT_EQ(archive.entries()[i].path_problem,"unsafe_entry_path");
  for(std::size_t i=7;i<names.size();++i) EXPECT_EQ(archive.entries()[i].path_problem,"case_collision");
}
TEST(ZipInput, StoredAndDeflateStreamsShareActualByteBudgetAcrossPasses) {
  RecordProperty("req","K-2|K-6|ADR-0011");
  zip_test::Entry a,b; a.name="a"; a.data="12345"; b.name="b"; b.data="abcde"; b.method=8;
  input::ZipInputLimits limits; limits.max_unpacked_bytes=12;
  auto context=job(limits);
  const auto path=install(zip_test::build({a,b},true));
  auto archive=take(input::ZipInput::open(path,context));
  EXPECT_TRUE(archive.summary().zip64);
  EXPECT_EQ(read(archive,0),a.data); EXPECT_EQ(read(archive,1),b.data); EXPECT_EQ(context->remaining(),2u);
  auto next=take(input::ZipInput::open(path,context));
  auto stream=take(next.open_entry(0)); std::array<std::byte,64> buffer{};
  EXPECT_EQ(take(stream.read(buffer)),2u);
  const auto limit=stream.read(buffer); ASSERT_FALSE(limit);
  EXPECT_EQ(limit.error().message_id,"am.import.archive.limit_exceeded"); EXPECT_EQ(context->exceeded_limit(),"max_unpacked_bytes");
  limits.max_unpacked_bytes=10; context=job(limits);
  auto exact=take(input::ZipInput::open(path,context));
  EXPECT_EQ(read(exact,0),a.data); EXPECT_EQ(read(exact,1),b.data);
  const auto empty=exact.open_entry(0); ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().code,am::Errc::unsupported); EXPECT_EQ(empty.error().message_id,"am.import.archive.limit_exceeded");
  EXPECT_EQ(context->exceeded_limit(),"max_unpacked_bytes");
}
TEST(ZipInput, FalseSizesCannotAvoidActualByteBudgetAndCrcIsVerified) {
  RecordProperty("req","K-2|K-6|ADR-0011");
  zip_test::Entry e; e.method=8; e.data=std::string(200,'x'); e.declared_size=100;
  input::ZipInputLimits limits; limits.max_unpacked_bytes=50;
  auto context=job(limits); auto archive=take(input::ZipInput::open(install(zip_test::build({e})),context));
  auto stream=take(archive.open_entry(0)); std::array<std::byte,128> out{};
  EXPECT_EQ(take(stream.read(out)),50u);
  EXPECT_FALSE(stream.read(out)); EXPECT_EQ(context->exceeded_limit(),"max_unpacked_bytes");
  e.declared_size.reset(); e.declared_crc=0;
  archive=take(input::ZipInput::open(install(zip_test::build({e})),job()));
  stream=take(archive.open_entry(0)); const auto bad=stream.read(out);
  if(bad) EXPECT_FALSE(stream.read(out));
  else EXPECT_EQ(bad.error().code,am::Errc::corrupt_data);
}
TEST(ZipInput, TimeAndRatioLimitsAreStickyAndNameTheLimit) {
  RecordProperty("req","K-2|K-6|ADR-0011");
  zip_test::Entry e; e.data=std::string(2*1024*1024,'x'); e.method=8;
  const auto path=install(zip_test::build({e}));
  input::ZipInputLimits limits; limits.max_ratio=2;
  auto context=job(limits); auto archive=take(input::ZipInput::open(path,context));
  auto stream=take(archive.open_entry(0)); std::array<std::byte,65536> out{};
  bool exceeded=false;
  for(;;) { const auto n=stream.read(out); if(!n) { exceeded=true; break; } if(!*n) break; }
  EXPECT_TRUE(exceeded); EXPECT_EQ(context->exceeded_limit(),"max_ratio"); EXPECT_FALSE(archive.open_entry(0));
  limits.max_seconds_per_container=std::chrono::seconds(1);
  input::ZipInputHooks hooks; std::chrono::steady_clock::time_point now{};
  hooks.clock=[&]{return now;}; context=std::make_shared<input::ZipInputJob>(limits,hooks);
  archive=take(input::ZipInput::open(path,context)); now+=std::chrono::seconds(2);
  EXPECT_FALSE(archive.open_entry(0)); EXPECT_EQ(context->exceeded_limit(),"max_seconds_per_container");
}
TEST(ZipInput, EntryCountLimitAndUnsupportedEntriesDoNotReadPayload) {
  RecordProperty("req","K-2|K-6|ADR-0011");
  zip_test::Entry a,b; a.name="encrypted"; a.flags|=1; b.name="unsupported"; b.method=93; b.compressed=zip_test::Bytes{}; b.declared_size=0;
  const auto path=install(zip_test::build({a,b}));
  input::ZipInputLimits limits; limits.zip.max_entries=1; auto context=job(limits);
  EXPECT_FALSE(input::ZipInput::open(path,context)); EXPECT_EQ(context->exceeded_limit(),"max_entries");
  input::ZipInputHooks hooks; unsigned reads=0; hooks.before_read=[&](auto,auto)->am::Result<void>{++reads;return {};};
  context=std::make_shared<input::ZipInputJob>(input::ZipInputLimits{},hooks);
  auto archive=take(input::ZipInput::open(path,context));
  ASSERT_EQ(archive.entries().size(),2u); EXPECT_FALSE(archive.open_entry(0)); EXPECT_FALSE(archive.open_entry(1)); EXPECT_EQ(reads,0u);
}
TEST(ZipInput, ContextualOwnManifestProbeKeepsLegacyProbeAndChargesSharedBudgetOnce) {
  RecordProperty("req","SC-1|K-6|ADR-0035");
  zip_test::Entry manifest,a,b; manifest.name="manifest.json";
  manifest.data=R"({"format":{"id":"urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128","version":{"major":1,"minor":0}}})";
  a.name=b.name="same"; a.data="first"; b.data="second";
  const auto path=install(zip_test::build({manifest,a,b}));
  const auto legacy=take(zip::probe_container(take(zip::file_source(path))));
  EXPECT_EQ(legacy.outcome,zip::ProbeOutcome::invalid_zip);
  input::ZipInputLimits limits; limits.max_unpacked_bytes=manifest.data.size();
  auto context=job(limits);
  const auto probe=take(input::ZipInput::probe(take(zip::file_source(path)),path,context));
  EXPECT_EQ(probe.outcome,zip::ProbeOutcome::own_corrupt); ASSERT_TRUE(probe.error);
  EXPECT_EQ(probe.error->message_id,"am.zip.duplicate_name");
  EXPECT_EQ(context->remaining(),0u); EXPECT_TRUE(context->exceeded_limit().empty());
  limits.max_unpacked_bytes=0; context=job(limits);
  const auto limited=take(input::ZipInput::probe(take(zip::file_source(path)),path,context));
  EXPECT_EQ(limited.outcome,zip::ProbeOutcome::invalid_zip);
  EXPECT_EQ(context->exceeded_limit(),"max_unpacked_bytes");
  EXPECT_FALSE(input::ZipInput::open(path,context));
  for(const auto& text:{std::string("broken"),std::string(R"({"format":{"id":"foreign"}})")}) {
    manifest.data=text;
    const auto other=install(zip_test::build({manifest,a,b}));
    const auto result=take(input::ZipInput::probe(take(zip::file_source(other)),other,job()));
    EXPECT_EQ(result.outcome,zip::ProbeOutcome::invalid_zip);
  }
  manifest.data=R"({"format":{"id":"urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128","version":{"major":1,"minor":0}}})";
  auto overlap=zip_test::build({manifest,a,b});
  zip_test::put(overlap.bytes,overlap.central[2]+42,overlap.local[1],4);
  const auto bad=install(overlap);
  EXPECT_EQ(take(input::ZipInput::probe(take(zip::file_source(bad)),bad,job())).outcome,zip::ProbeOutcome::invalid_zip);
  manifest.method=8;
  manifest.data=std::string(R"({"format":{"id":"urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128","version":{"major":1,"minor":0}},"padding":")")+std::string(2*1024*1024,'x')+R"("})";
  const auto compressed=install(zip_test::build({manifest,a,b}));
  context=job();
  const auto ratio=take(input::ZipInput::probe(take(zip::file_source(compressed)),compressed,context));
  EXPECT_EQ(ratio.outcome,zip::ProbeOutcome::invalid_zip); EXPECT_EQ(context->exceeded_limit(),"max_ratio");
  const auto normal=install(zip_test::build({manifest})); context=job();
  const auto normal_ratio=take(input::ZipInput::probe(take(zip::file_source(normal)),normal,context));
  EXPECT_EQ(normal_ratio.outcome,zip::ProbeOutcome::invalid_zip); EXPECT_EQ(context->exceeded_limit(),"max_ratio");
}
TEST(ImportZipManifest, FilesystemArchiveIsAnIndependentTopLevelObject) {
  RecordProperty("req","SC-1|K-5|K-6|ADR-0037");
  namespace repo=am::repository;
  const auto id=[] { return am::Uuid::v4().to_string(); };
  const auto root=fs::path(AM_IMPORT_SCAN_ROOT)/"zip-input"/id();
  fs::create_directories(root);
  const auto payload=root/"payload";
  { std::ofstream out(payload,std::ios::binary); out<<"bytes"; }
  repo::RepositoryOptions options; options.library_id=id(); options.application_version="test";
  options.capabilities.emplace(); options.capabilities->mode=repo::StorageMode::full;
  options.capabilities->filesystem="synthetic unit-test capabilities";
  auto repository=take(repo::AssetRepository::open(root,options));
  const auto directory=id(), archive=id(), file=id();
  repo::AddFiles add;
  add.input={{"id",id()},{"imported_at","2026-11-25T00:00:00.000Z"},{"mode","strict"},{"completeness","complete"},
    {"objects",repo::Json::array({repo::Json{{"id",directory},{"kind","directory"},{"name","source"}},
      repo::Json{{"id",archive},{"kind","archive"},{"container","zip"},{"name","model.zip"},{"relative_path","sub/model.zip"},{"size_bytes","123"},{"sha256",std::string(64,'0')}}})},
    {"entries",repo::Json::array({repo::Json{{"id",id()},{"object_id",directory},{"entry_kind","directory"},{"path",""},{"result","included"}},
      repo::Json{{"id",id()},{"object_id",directory},{"entry_kind","directory"},{"path","sub"},{"result","included"}},
      repo::Json{{"id",id()},{"object_id",archive},{"entry_kind","file"},{"path","part.bin"},{"result","included"},{"file_id",file}}})}};
  repo::FileInput source; source.path="sub/model/part.bin"; source.source=payload; source.file_id=file;
  add.files.push_back(source);
  repo::CreateRequest request; request.files.push_back(add);
  const auto created=repository.create(request);
  ASSERT_TRUE(created)<<am::to_log_string(created.error());
  EXPECT_EQ(created->verification.v1,repo::CheckStatus::passed);
  EXPECT_EQ(created->verification.v2,repo::CheckStatus::passed);
  EXPECT_EQ(created->verification.v3,repo::CheckStatus::passed);
  const auto snapshot=take(repository.snapshot(created->ref));
  const auto& input=snapshot.manifest().known()["inputs"][0];
  ASSERT_EQ(input["objects"].size(),2u);
  EXPECT_FALSE(input["objects"][1].contains("parent_entry_id"));
  EXPECT_FALSE(input["objects"][1].contains("nested_action"));
  for(const auto& entry:input["entries"]) EXPECT_NE(entry["path"],"sub/model.zip");
}
TEST(ImportZipScan, VersionTwoListsAllRecordsAndRetainsOuterArchiveDecision) {
  RecordProperty("req","SC-1|K-5|K-6");
  zip_test::Entry directory,a,b; directory.name="folder/"; directory.data="";
  a.name="folder/part.bin"; a.data="payload"; b.name="other.bin"; b.data="other"; b.method=8;
  const auto path=install(zip_test::build({directory,a,b},true,"comment"));
  const auto plan=scan_zip(path).value;
  EXPECT_EQ(plan["version"],3); ASSERT_EQ(plan["archives"].size(),1u); ASSERT_EQ(plan["archive_entries"].size(),3u);
  EXPECT_EQ(plan["archives"][0]["entry_count"],3); EXPECT_TRUE(plan["archives"][0]["zip64"]);
  EXPECT_EQ(plan["archives"][0]["comment_length"],7);
  EXPECT_EQ(plan["archive_entries"][0]["kind"],"directory");
  EXPECT_EQ(plan["archive_entries"][1]["path"],a.name);
  EXPECT_EQ(plan["candidates"][0]["entries"].size(),4u);
  EXPECT_TRUE(plan["candidates"][0]["entries"][0].contains("node"));
  EXPECT_EQ(plan["candidates"][0]["action"],"import");
  EXPECT_EQ(plan["profile"]["limits"]["max_entries"],"100000");
}
TEST(ImportZipScan, DuplicateOwnManifestIsReadyInvalidWhileOtherDuplicatesAreArchives) {
  RecordProperty("req","SC-1|K-6|ADR-0035");
  zip_test::Entry manifest,a,b; manifest.name="manifest.json";
  manifest.data=R"({"format":{"id":"urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128","version":{"major":1,"minor":0}}})";
  a.name=b.name="same";
  auto plan=scan_zip(install(zip_test::build({manifest,a,b}))).value;
  EXPECT_EQ(plan["candidates"][0]["kind"],"ready_asset"); EXPECT_EQ(plan["candidates"][0]["action"],"ask");
  EXPECT_TRUE(plan["archives"].empty());
  EXPECT_NE(plan["candidates"][0]["warnings"].dump().find("am.zip.duplicate_name"),std::string::npos);
  plan=scan_zip(install(zip_test::build({a,b}))).value;
  EXPECT_EQ(plan["candidates"][0]["kind"],"new_asset"); EXPECT_EQ(plan["archive_entries"].size(),2u);
  EXPECT_EQ(plan["candidates"][0]["entries"][1]["explanation_id"],"duplicate_entry_name");
  EXPECT_EQ(plan["candidates"][0]["entries"][2]["explanation_id"],"duplicate_entry_name");
}
TEST(ImportZipScan, NestedArchivesAskButSpecializedExtensionAndAppleDoubleFollowDecisionSeven) {
  RecordProperty("req","SC-1|SC-10|K-6|ADR-0011");
  const auto inner=zip_test::build({zip_test::Entry{}}).bytes;
  const std::string data(reinterpret_cast<const char*>(inner.data()),inner.size());
  zip_test::Entry model,nested,junk; model.name="model.3mf"; model.data=data; model.method=8;
  nested.name="inner.zip"; nested.data=data;
  junk.name="._model.3mf"; junk.data=std::string("\0\5\x16\7",4);
  const auto plan=scan_zip(install(zip_test::build({model,nested,junk}))).value;
  const auto& entries=plan["candidates"][0]["entries"];
  EXPECT_EQ(entries[1]["decision"],"include");
  EXPECT_EQ(entries[1]["confidence"],"probable"); EXPECT_EQ(entries[1]["explanation_id"],"specialized_container_unverified");
  // T-032: a nested ZIP is listed and expanded by the default profile; its record follows the parent's.
  EXPECT_EQ(entries[2]["decision"],"expand"); EXPECT_EQ(entries[2]["explanation_id"],"nested_archive_profile_expand");
  EXPECT_EQ(entries[4]["decision"],"exclude"); EXPECT_EQ(entries[4]["rule_id"],"macos.appledouble");
  EXPECT_EQ(plan["archive_entries"][0]["detected"],"archive.zip");
}
TEST(ImportZipScan, EntryLimitIsExplicitAndCannotBecomeAnOrdinaryUnreadableArchive) {
  RecordProperty("req","SC-1|K-6|ADR-0011");
  zip_test::Entry a,b; a.name="a"; b.name="b";
  auto profile=take(am::application::load_import_profile()); profile.settings["limits"]["max_entries"]=1;
  const auto plan=scan_zip(install(zip_test::build({a,b})),profile).value;
  ASSERT_EQ(plan["archives"].size(),1u);
  EXPECT_EQ(plan["archives"][0]["status"],"limit_exceeded");
  EXPECT_EQ(plan["archives"][0]["problem"]["limit"],"max_entries");
  EXPECT_EQ(plan["archives"][0]["problem"]["effective_value"],"1");
  EXPECT_EQ(plan["archives"][0]["problem"]["source"],"plan");
  EXPECT_EQ(plan["candidates"][0]["action"],"ask");
}
TEST(ImportZipRepository, ArchivePassesDistinguishCorruptionReplacementDestinationFailureAndCancellation) {
  RecordProperty("req","SC-1|K-6|ADR-0025");
  namespace repo=am::repository;
  using J=repo::Json;
  for(const auto scenario:{"ok","crc","replacement","destination","cancel","sha"}) {
    SCOPED_TRACE(scenario);
    zip_test::Entry e; e.name="part.bin"; e.data="payload"; e.method=8;
    if(std::string_view(scenario)=="crc") e.declared_crc=0;
    const auto file=install(zip_test::build({e}));
    auto snapshot=std::make_shared<input::ZipInputSnapshot>(); snapshot->path=file;
    snapshot->job=job();
    {const auto opened=take(input::ZipInput::open(file,snapshot->job));snapshot->entries=opened.identity();snapshot->sha256=take(opened.sha256());}
    repo::RepositoryOptions options; options.library_id=am::Uuid::v4().to_string(); options.application_version="test";
    options.capabilities.emplace(); options.capabilities->mode=repo::StorageMode::full;
    std::stop_source stop;
    if(std::string_view(scenario)=="replacement") options.test_hooks.after_preflight=[&] {
      e.data="changed"; const auto bytes=zip_test::build({e}).bytes;
      std::ofstream out(file,std::ios::binary|std::ios::trunc); out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    };
    if(std::string_view(scenario)=="cancel") options.test_hooks.after_preflight=[&]{stop.request_stop();};
    if(std::string_view(scenario)=="destination") options.test_hooks.commit.create_sink=[](const auto&)->am::Result<std::shared_ptr<zip::RandomAccessSink>> {
      return am::Error::make(am::Errc::io_error,"Injected destination error").id("test.destination");
    };
    const auto root=file.parent_path()/am::Uuid::v4().to_string(); fs::create_directories(root);
    auto repository=take(repo::AssetRepository::open(root,options));
    const auto oid=am::Uuid::v4().to_string(), fid=am::Uuid::v4().to_string();
    repo::AddFiles add;
    add.input={{"id",am::Uuid::v4().to_string()},{"imported_at","2026-11-25T00:00:00Z"},{"mode","strict"},{"completeness","complete"},
      {"objects",J::array({J{{"id",oid},{"kind","archive"},{"name","source.zip"},{"container","zip"},{"sha256",*snapshot->sha256}}})},
      {"entries",J::array({J{{"id",am::Uuid::v4().to_string()},{"object_id",oid},{"entry_kind","file"},{"path","part.bin"},{"result","included"},{"file_id",fid}}})}};
    repo::FileInput source; source.path="part.bin"; source.file_id=fid; source.archive_entry=repo::ArchiveEntryInput{snapshot,0};
    if(std::string_view(scenario)=="sha") source.expected_sha256=std::string(64,'0');
    add.files.push_back(source); repo::CreateRequest request; request.files.push_back(add); request.stop=stop.get_token();
    std::optional<repo::SourceFailure> failure; request.source_failure=&failure;
    const auto result=repository.create(request);
    if(std::string_view(scenario)=="ok") { ASSERT_TRUE(result)<<am::to_log_string(result.error()); EXPECT_FALSE(failure); }
    else {
      ASSERT_FALSE(result);
      if(std::string_view(scenario)=="crc") {ASSERT_TRUE(failure);EXPECT_TRUE(failure->read_failure);EXPECT_EQ(failure->file_id,fid);}
      else if(std::string_view(scenario)=="replacement" || std::string_view(scenario)=="sha") {
        ASSERT_TRUE(failure); EXPECT_FALSE(failure->read_failure); EXPECT_EQ(result.error().message_id,"am.repo.source_changed");
      } else EXPECT_FALSE(failure);
      if(std::string_view(scenario)=="cancel") { EXPECT_EQ(result.error().code,am::Errc::cancelled); }
      const auto assets=root/"assets";
      if(fs::exists(assets)) {
        for(const auto& entry:fs::recursive_directory_iterator(assets)) { EXPECT_FALSE(entry.is_regular_file()); }
      }
      const auto tmp=root/".am/.am-tmp"; EXPECT_TRUE(!fs::exists(tmp) || fs::is_empty(tmp));
      EXPECT_TRUE(take(take(am::db::StateJournal::open(root))->list()).empty());
    }
  }
}
// T-032: nested ZIP expansion.
namespace {
std::string blob(const zip_test::Built& built) { return {reinterpret_cast<const char*>(built.bytes.data()),built.bytes.size()}; }
zip_test::Entry member(std::string name,std::string data,std::uint16_t method=0) {
  zip_test::Entry e; e.name=std::move(name); e.data=std::move(data); e.method=method; return e;
}
std::size_t files_in(const fs::path& dir) {
  if(!fs::exists(dir)) return 0;
  std::size_t n=0; for(const auto& e:fs::recursive_directory_iterator(dir)) n+=e.is_regular_file(); return n;
}
fs::path temp_root() { return fs::path(AM_IMPORT_SCAN_ROOT)/"nested-temp"/am::Uuid::v4().to_string(); }
} // namespace
TEST(ZipInputNested, StoredIsReadInPlaceAndCompressedUsesTemporaryFileThatOutlivesNoReader) {
  RecordProperty("req","SC-1|K-6|NFR-14.4.3");
  const auto inner=blob(zip_test::build({member("x","hello")}));
  const auto path=install(zip_test::build({member("stored.zip",inner),member("deflated.zip",inner,8)}));
  auto outer=take(input::ZipInput::open(path,job()));
  auto stored=take(outer.open_nested(0,{}));
  EXPECT_FALSE(stored.temporary()); EXPECT_EQ(stored.depth(),2u); EXPECT_EQ(read(stored,0),"hello");
  const auto refused=outer.open_nested(1,{});
  ASSERT_FALSE(refused); EXPECT_EQ(refused.error().message_id,"am.archive.temp_unavailable");
  const auto temp=temp_root();
  std::optional<input::ZipInputStream> stream;
  {
    auto deflated=take(outer.open_nested(1,temp));
    EXPECT_TRUE(deflated.temporary()); EXPECT_EQ(files_in(temp),1u); EXPECT_EQ(read(deflated,0),"hello");
    stream.emplace(take(deflated.open_entry(0)));
  }
  EXPECT_EQ(files_in(temp),1u);  // the open stream keeps the file alive
  stream.reset();
  EXPECT_EQ(files_in(temp),0u);
}
TEST(ZipInputNested, DepthAndTemporaryBytesAreChargedToTheSharedJob) {
  RecordProperty("req","SC-1|K-6|NFR-14.4.3");
  const auto level3=blob(zip_test::build({member("x","deep")}));
  const auto level2=blob(zip_test::build({member("c.zip",level3)}));
  const auto path=install(zip_test::build({member("b.zip",level2)}));
  input::ZipInputLimits limits; limits.max_depth=2;
  auto shared=job(limits);
  auto outer=take(input::ZipInput::open(path,shared));
  auto second=take(outer.open_nested(0,{}));
  const auto third=second.open_nested(0,{});
  ASSERT_FALSE(third); EXPECT_EQ(third.error().message_id,"am.import.archive.limit_exceeded");
  EXPECT_EQ(shared->exceeded_limit(),"max_depth");
  input::ZipInputLimits wide; wide.max_depth=20;
  EXPECT_EQ(job(wide)->limits().max_depth,input::unconditional_max_depth);
  // Peak temporary bytes: exactly the unpacked size fits, one byte less does not.
  const auto compressed=install(zip_test::build({member("d.zip",level2,8)}));
  for(const auto extra:{0,-1}) {
    input::ZipInputLimits l; l.max_temp_bytes=level2.size()+extra;
    auto j=job(l); auto zip=take(input::ZipInput::open(compressed,j));
    const auto temp=temp_root();
    const auto nested=zip.open_nested(0,temp);
    EXPECT_EQ(static_cast<bool>(nested),extra==0);
    if(extra) EXPECT_EQ(j->exceeded_limit(),"max_temp_bytes");
    if(!nested) EXPECT_EQ(files_in(temp),0u);
  }
}
TEST(ZipInputNested, SnapshotChainReopensNestedArchives) {
  RecordProperty("req","SC-1|K-6");
  const auto inner=blob(zip_test::build({member("x","hello")}));
  const auto path=install(zip_test::build({member("i.zip",inner,8)}));
  auto shared=job();
  auto outer=take(input::ZipInput::open(path,shared));
  const auto temp=temp_root();
  auto nested=take(outer.open_nested(0,temp));
  auto parent=std::make_shared<input::ZipInputSnapshot>(); parent->path=path; parent->entries=outer.identity();
  input::ZipInputSnapshot child; child.path=nested.label(); child.entries=nested.identity();
  child.parent=parent; child.parent_index=0; child.temp_dir=temp;
  auto reopened=take(input::open_snapshot(child,shared));
  EXPECT_EQ(reopened.identity(),nested.identity()); EXPECT_EQ(take(reopened.sha256()),take(nested.sha256()));
}
TEST(ImportZipScan, NestedZipIsListedWithDepthDigestAccessAndVersionThree) {
  RecordProperty("req","SC-1|K-6");
  const auto inner=blob(zip_test::build({member("x","hello")}));
  const auto plan=scan_zip(install(zip_test::build({member("inner.zip",inner)}))).value;
  EXPECT_EQ(plan["version"],3); EXPECT_EQ(plan["profile"]["limits"]["max_depth"],"3");
  for(const auto& item:plan["profile"]["not_applied"]) { EXPECT_NE(item,"nested"); EXPECT_NE(item,"limits.max_temp_bytes"); }
  ASSERT_EQ(plan["archives"].size(),2u);
  const auto& nested=plan["archives"][1];
  EXPECT_EQ(nested["entry"],plan["archive_entries"][0]["id"]); EXPECT_EQ(nested["depth"],2); EXPECT_EQ(nested["access"],"range");
  EXPECT_EQ(nested["status"],"listed"); EXPECT_EQ(nested["sha256"].get<std::string>().size(),64u);
  EXPECT_EQ(plan["archive_entries"][1]["archive"],plan["archive_entries"][0]["id"]);
  const auto& entries=plan["candidates"][0]["entries"];
  EXPECT_EQ(entries[1]["decision"],"expand"); EXPECT_EQ(entries[2]["archive_entry"],plan["archive_entries"][1]["id"]);
  EXPECT_EQ(plan["scan"]["completeness"],"complete");
}
TEST(ImportZipScan, CompressedNestedWithoutLibraryAsksAndDepthLimitIsIncomplete) {
  RecordProperty("req","SC-1|K-6|NFR-14.4.3");
  const auto inner=blob(zip_test::build({member("x","hello")}));
  auto plan=scan_zip(install(zip_test::build({member("inner.zip",inner,8)}))).value;
  EXPECT_EQ(plan["archives"].size(),1u); EXPECT_EQ(plan["scan"]["completeness"],"incomplete");
  EXPECT_EQ(plan["candidates"][0]["entries"][1]["explanation_id"],"nested_archive_temp_unavailable");
  const auto deeper=blob(zip_test::build({member("deep.zip",inner)}));
  auto profile=take(am::application::load_import_profile()); profile.settings["nested"]["max_depth"]=2;
  plan=scan_zip(install(zip_test::build({member("mid.zip",deeper)})),profile).value;
  ASSERT_EQ(plan["archives"].size(),2u); EXPECT_EQ(plan["scan"]["completeness"],"incomplete");
  const auto& hit=plan["scan"]["limits_hit"]; EXPECT_NE(std::find(hit.begin(),hit.end(),"max_depth"),hit.end());
  EXPECT_EQ(plan["candidates"][0]["entries"][2]["explanation_id"],"nested_archive_depth_limit");
  const auto& reasons=plan["candidates"][0]["reasons"];
  EXPECT_NE(std::find(reasons.begin(),reasons.end(),"nested_archive_depth_limit"),reasons.end());
}
TEST(ImportZipScan, ExclusionRulesRunBeforeExpansionAtEveryLevel) {
  RecordProperty("req","SC-1|K-6|ADR-0011");
  const auto inner=blob(zip_test::build({member("model.stl","solid"),member(".DS_Store","junk")}));
  const auto plan=scan_zip(install(zip_test::build({member("inner.zip",inner),member("__MACOSX/._inner.zip",std::string("\0\5\x16\7",4)+inner)}))).value;
  ASSERT_EQ(plan["archives"].size(),2u);  // the AppleDouble copy is never listed
  const auto& entries=plan["candidates"][0]["entries"];
  EXPECT_NE(entries[3]["decision"],"include"); EXPECT_EQ(entries[3]["rule_id"],"macos.ds_store");
  EXPECT_EQ(entries[4]["decision"],"exclude"); EXPECT_EQ(entries[4]["rule_id"],"macos.appledouble");
}
TEST(ImportZipPlan, ExpandRequiresAListedNestedArchive) {
  RecordProperty("req","SC-1|K-6");
  const auto inner=blob(zip_test::build({member("x","hello")}));
  auto plan=scan_zip(install(zip_test::build({member("inner.zip",inner),member("y","plain")})));
  plan.value["candidates"][0]["entries"][3]["decision"]="expand";  // y is not an archive
  const auto parsed=am::import::parse_plan_view(am::import::serialize_plan(plan));
  ASSERT_FALSE(parsed); EXPECT_EQ(parsed.error().message_id,"am.import.plan_invalid");
  EXPECT_TRUE(parsed.error().ctx.detail.ends_with("/entries/3/decision")) << am::to_log_string(parsed.error());
}
TEST_F(ImportZipRun, NestedArchivesExpandUnderTheirStemAndRecordExtractProvenance) {
  RecordProperty("req","SC-1|K-6|X-01");
  const auto deep=blob(zip_test::build({member("d.txt","deepest")}));
  const auto inner=blob(zip_test::build({member("i.txt","inner"),member("deep.zip",deep,8)}));
  // Compressed nested records need the library's managed temporary area during the scan.
  am::import::ScanRequest r; r.roots={fixture({member("a/inner.zip",inner),member("top.txt","top")})};
  r.profile=take(am::application::load_import_profile()); r.library=library;
  plan=take(am::import::scan(r));
  ASSERT_EQ(plan.value["archives"].size(),3u);
  EXPECT_EQ(files_in(library/".am/.am-tmp"),0u);
  EXPECT_EQ(plan.value["archives"][2]["access"],"temp");
  include();
  const auto row=execute(enqueue()); ASSERT_EQ(row["status"],"created") << row.dump();
  EXPECT_EQ(payloads(row),(std::multiset<std::string>{"inner","deepest","top"}));
  const auto m=manifest(row); const auto& in=m["inputs"][0];
  ASSERT_EQ(in["objects"].size(),3u);
  const auto& first=in["objects"][1]; const auto& second=in["objects"][2];
  EXPECT_EQ(first["nested_action"],"extract"); EXPECT_EQ(first["name"],"inner.zip"); EXPECT_FALSE(first.contains("relative_path"));
  EXPECT_EQ(first["parent_object_id"],in["objects"][0]["id"]); EXPECT_EQ(second["parent_object_id"],first["id"]);
  EXPECT_EQ(first["sha256"].get<std::string>().size(),64u);
  std::set<std::string> paths;
  for(const auto& f:m["files"]) paths.insert(f["storage_path"].get<std::string>());
  for(const auto& e:in["entries"]) {
    if(e["id"]==first["parent_entry_id"] || e["id"]==second["parent_entry_id"]) {
      EXPECT_EQ(e["result"],"excluded"); EXPECT_EQ(e["exclusion"]["rule"],"nested_extract"); EXPECT_EQ(e["exclusion"]["by"],"profile");
      EXPECT_FALSE(e.contains("file_id"));
    }
  }
  const auto prefix="content/"+in["id"].get<std::string>()+"/";
  EXPECT_TRUE(paths.contains(prefix+"a/inner/i.txt")) << m["files"].dump();
  EXPECT_TRUE(paths.contains(prefix+"a/inner/deep/d.txt")) << m["files"].dump();
  clean();
}
TEST_F(ImportZipRun, NestedChangeIsSourceChangedAndCorruptRecordFailsStrictWithoutAsset) {
  RecordProperty("req","SC-1|K-6|SC-13");
  const auto inner=blob(zip_test::build({member("x",std::string(4096,'a'))}));
  const auto target=fixture({member("inner.zip",inner)});
  scan(target); include();
  auto changed=inner; changed[changed.size()-30]^=1;  // inner central directory changes, outer CRC updates
  const auto stamp=fs::last_write_time(target);
  fixture({member("inner.zip",changed)}); fs::last_write_time(target,stamp);
  auto row=execute(enqueue());
  EXPECT_EQ(row["status"],"failed") << row.dump(); EXPECT_EQ(row["error"]["message_id"],"am.import.source_changed");
  // A bad CRC two levels down fails a strict import without creating an asset.
  zip_test::Entry bad=member("x",std::string(4096,'b')); bad.declared_crc=0x12345678;
  const auto broken=blob(zip_test::build({bad}));
  fixture({member("inner.zip",broken)},"other.zip");
  scan(source/"other.zip"); include();
  row=execute(enqueue());
  EXPECT_EQ(row["status"],"failed") << row.dump(); EXPECT_FALSE(row.contains("asset_id"));
  clean(true);
}
TEST_F(ImportZipRun, NestedEntryIdentifiersStayOutOfDurableParameters) {
  RecordProperty("req","SC-1|K-6|NFR-14.4.3");
  std::size_t small=0;
  for(const auto count:{10,2000}) {
    std::vector<zip_test::Entry> records;
    for(int i=0;i<count;++i) records.push_back(member("f"+std::to_string(i),"x"));
    scan(fixture({member("inner.zip",blob(zip_test::build(records)))})); include();
    const auto q=enqueue(); am::contracts::JobFilter f; f.parent_id=q.job_id;
    const auto children=take(catalog->job_store().list(f)); ASSERT_EQ(children.size(),1u);
    if(count==10) small=children[0].params_json.size(); else EXPECT_EQ(children[0].params_json.size(),small);
  }
}
TEST(ImportZipScan, RarAndSevenZipInsideZipStayAskAndAreNotExpanded) {
  RecordProperty("req","SC-2|SC-6|K-6");
  for(const auto& signature:{std::string("Rar!\x1a\x07\0",7),std::string("7z\xbc\xaf\x27\x1c",6)}) {
    const auto plan=scan_zip(install(zip_test::build({member("inner.bin",signature+"payload")}))).value;
    EXPECT_EQ(plan["archives"].size(),1u);
    EXPECT_EQ(plan["candidates"][0]["entries"][1]["decision"],"ask");
    EXPECT_EQ(plan["candidates"][0]["entries"][1]["explanation_id"],"nested_archive_expand_unsupported");
  }
}
TEST(ImportZipScan, UnpackedBudgetIsSummedAcrossNestingLevels) {
  RecordProperty("req","SC-1|K-6|ADR-0011");
  // Each stored nested archive fits the budget alone; together they do not.
  const auto inner=blob(zip_test::build({member("x",std::string(4096,'a'))}));
  auto profile=take(am::application::load_import_profile());
  profile.settings["limits"]["max_unpacked_bytes"]=inner.size()+inner.size()/2;
  const auto plan=scan_zip(install(zip_test::build({member("one.zip",inner),member("two.zip",inner)})),profile).value;
  // The sticky limit is reported on the nested archive whose read crossed it.
  ASSERT_EQ(plan["archives"].size(),3u);
  EXPECT_EQ(plan["archives"][1]["status"],"listed");
  ASSERT_EQ(plan["archives"][2]["status"],"limit_exceeded") << plan["archives"].dump();
  EXPECT_EQ(plan["archives"][2]["problem"]["limit"],"max_unpacked_bytes");
  const auto& hit=plan["scan"]["limits_hit"]; EXPECT_NE(std::find(hit.begin(),hit.end(),"max_unpacked_bytes"),hit.end());
  EXPECT_EQ(plan["candidates"][0]["action"],"ask");
  EXPECT_EQ(plan["scan"]["completeness"],"incomplete");
}
TEST_F(ImportZipRun, NestedBudgetIsSummedAcrossLevelsAndFailsOnceWithoutAsset) {
  RecordProperty("req","SC-1|K-6|ADR-0011");
  const auto inner=blob(zip_test::build({member("x",std::string(4096,'a'))}));
  scan(fixture({member("inner.zip",inner)})); include();
  // The outer record and the inner payload each fit; their sum does not.
  plan.value["profile"]["limits"]["max_unpacked_bytes"]=std::to_string(inner.size()+1000);
  unsigned attempts=0; am::import::RunHooks hooks; hooks.before_attempt=[&](auto&){++attempts;};
  const auto row=execute(enqueue(),hooks,2);
  EXPECT_EQ(row["status"],"failed") << row.dump();
  EXPECT_EQ(row["error"]["message_id"],"am.import.archive.limit_exceeded") << row.dump();
  EXPECT_EQ(row["problem"]["limit"],"max_unpacked_bytes");
  EXPECT_EQ(attempts,1u); EXPECT_FALSE(row.contains("asset_id"));
  clean(true);
}
TEST_F(ImportZipRun, NestedCrcFailureIsAFailedRecordInPartialMode) {
  RecordProperty("req","SC-1|K-5|K-6");
  zip_test::Entry bad=member("bad",std::string(1000,'b'),8); bad.declared_crc=0;
  const auto inner=blob(zip_test::build({bad,member("good","good")}));
  scan(fixture({member("inner.zip",inner)})); include();
  plan.value["mode"]="partial";
  const auto row=execute(enqueue()); ASSERT_EQ(row["status"],"created") << row.dump();
  const auto m=manifest(row); const auto& in=m["inputs"][0];
  EXPECT_EQ(in["completeness"],"incomplete");
  unsigned failed=0; for(const auto& e:in["entries"]) failed+=e["result"]=="failed";
  EXPECT_EQ(failed,1u); EXPECT_EQ(payloads(row),(std::multiset<std::string>{"good"}));
  clean();
}
TEST_F(ImportZipRun, NestedPrefixCollidingWithADirectoryIsRenamed) {
  RecordProperty("req","SC-1|K-5|K-6");
  const auto inner=blob(zip_test::build({member("x.txt","nested")}));
  scan(fixture({member("a/inner.zip",inner),member("a/inner/x.txt","plain")})); include();
  const auto row=execute(enqueue()); ASSERT_EQ(row["status"],"created") << row.dump();
  const auto m=manifest(row); unsigned renamed=0;
  for(const auto& e:m["inputs"][0]["entries"]) if(e.value("rename_reason",std::string{})=="collision") ++renamed;
  EXPECT_GE(renamed,1u) << m["inputs"][0]["entries"].dump();
  std::set<std::string> paths; for(const auto& f:m["files"]) paths.insert(f["storage_path"].get<std::string>());
  EXPECT_EQ(paths.size(),2u);
  EXPECT_EQ(payloads(row),(std::multiset<std::string>{"nested","plain"}));
  clean();
}
TEST_F(ImportZipRun, ThreeMfIsVerifiedByItsModelPartThroughNestedAccess) {
  RecordProperty("req","SC-1|SC-10|K-6");
  const auto model=blob(zip_test::build({member("[Content_Types].xml","types"),member("3D/3dmodel.model","<model/>")}));
  const auto bare=blob(zip_test::build({member("[Content_Types].xml","types"),member("Metadata/thumbnail.png","png")}));
  const auto target=fixture({member("a.3mf",model),member("b.3mf",model,8),member("e.3mf","PK\x03\x04 broken"),member("c.3mf",bare)});
  am::import::ScanRequest r; r.roots={target}; r.profile=take(am::application::load_import_profile()); r.library=library;
  plan=take(am::import::scan(r));
  EXPECT_EQ(files_in(library/".am/.am-tmp"),0u);
  const auto& entries=plan.value["candidates"][0]["entries"];
  for(const auto i:{1,2}) {
    EXPECT_EQ(entries[i]["decision"],"include") << entries.dump();
    EXPECT_EQ(entries[i]["confidence"],"certain"); EXPECT_EQ(entries[i]["explanation_id"],"specialized_container_verified");
  }
  // Unreadable: the T-031 outcome.
  EXPECT_EQ(entries[3]["decision"],"include"); EXPECT_EQ(entries[3]["confidence"],"probable");
  EXPECT_EQ(entries[3]["explanation_id"],"specialized_container_unverified");
  // Readable but without a model part: listed, asks to keep it as a file or expand it.
  EXPECT_EQ(entries[4]["decision"],"ask"); EXPECT_EQ(entries[4]["explanation_id"],"specialized_container_unverified");
  ASSERT_EQ(plan.value["archives"].size(),2u); EXPECT_EQ(plan.value["archives"][1]["depth"],2);
  // Verified 3MF records are kept as files; no nesting object is written for them.
  include();
  const auto row=execute(enqueue()); ASSERT_EQ(row["status"],"created") << row.dump();
  EXPECT_EQ(manifest(row)["inputs"][0]["objects"].size(),1u);
  clean();
}
TEST(ImportZipScan, ThreeMfWithoutTemporaryAreaKeepsTheT031Outcome) {
  RecordProperty("req","SC-1|SC-10|K-6");
  const auto bare=blob(zip_test::build({member("x","y")}));
  const auto plan=scan_zip(install(zip_test::build({member("a.3mf",bare,8),member("b.3mf",bare)}))).value;
  const auto& entries=plan["candidates"][0]["entries"];
  // Deflate without a library cannot be read: probable include; Stored is read in place and asks.
  EXPECT_EQ(entries[1]["decision"],"include"); EXPECT_EQ(entries[1]["confidence"],"probable");
  EXPECT_EQ(entries[2]["decision"],"ask"); EXPECT_EQ(entries[2]["explanation_id"],"specialized_container_unverified");
}
