#include <gtest/gtest.h>
#include <cstdlib>
#include <optional>
#include <map>
#ifdef _WIN32
#include <windows.h>
#endif
#include "fault_world.hpp"
#include "am/application/logging.hpp"
#include "am/application/import_service.hpp"
#include "am/services/import/run.hpp"
#include "am/storage/export/prepare.hpp"

namespace {
using namespace fault_test;
constexpr std::size_t payload_size=4*1024*1024; // hypothesis: exceeds remaining test-volume space
std::string payload(unsigned seed) {
  std::string data(payload_size,'\0');
  for (auto& c:data) { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; c=static_cast<char>(seed&255); }
  return data;
}
struct Volume {
  fs::path area,root,source,output,filler;
  am::repository::ChangeResult asset;
  std::optional<am::catalog::Catalog> catalog;
  Volume() {
    std::string environment;
#ifdef _WIN32
    const auto count=GetEnvironmentVariableA("AM_SMALL_VOLUME",nullptr,0);
    if (count) { environment.resize(count); GetEnvironmentVariableA("AM_SMALL_VOLUME",environment.data(),count); environment.pop_back(); }
    const char* setting=environment.c_str();
#else
    const char* setting=std::getenv("AM_SMALL_VOLUME");
#endif
    if (!setting || !*setting) throw std::runtime_error("AM_SMALL_VOLUME is missing");
    const auto base=fs::path(setting);
    const auto capacity=fs::space(base).capacity;
    if (!capacity || capacity>128ULL*1024*1024) throw std::runtime_error("Refusing a volume larger than the 128 MiB safety hypothesis");
    area=base/("am-t036-"+am::Uuid::v4().to_string()); root=area/"library"; output=area/"output"; filler=area/"filler.bin";
    source=fs::path(AM_FAULT_TEST_ROOT)/"small-source"/(am::Uuid::v4().to_string()+".stl");
    fs::create_directories(area); fs::create_directories(output);
    write(source,payload(0x73518u));
    take(am::library::Library::init(root));
    catalog.emplace(take(am::catalog::Catalog::open(root,writable())));
    auto repo_options=options(true);
    repo_options.on_indexed=[this](const am::repository::IndexedEvent& e) { take(catalog->indexer().refresh(e.ref.asset_id)); };
    auto repo=take(am::repository::AssetRepository::open(root,repo_options));
    asset=take(repo.create(request(source)));
    auto user=am::db::Connection::borrow(catalog->library().database(am::library::DatabaseKind::userdata));
    take(user.exec("INSERT INTO collections(coll_id,name,kind,created_at) VALUES('confirmed','Favorite','favorites','2020-01-01T00:00:00Z')"));
    auto classified=take(am::repository::detect_storage_capabilities(root));
    testing::Test::RecordProperty("actual_storage_mode",static_cast<int>(classified.mode));
    testing::Test::RecordProperty("actual_filesystem",classified.filesystem);
    testing::Test::RecordProperty("volume_capacity_bytes",std::to_string(capacity));
    testing::Test::RecordProperty("libraries",Json::array({root.generic_string()}).dump());
  }
  ~Volume() { catalog.reset(); std::error_code e; fs::remove(filler,e); }
  am::repository::RepositoryOptions options(bool overstate) {
    am::repository::RepositoryOptions o; o.library_id=catalog->library().config().library_id; o.capabilities=writable().capabilities.value();
    if (overstate) o.test_hooks.commit.free_space=[](const fs::path&)->am::Result<std::uint64_t> { return UINT64_MAX; };
    return o;
  }
  void fill(std::uint64_t leave) {
    auto sink=take(am::asset_zip::file_sink(filler));
    std::array<std::byte,65536> block{};
    std::uint64_t offset=0;
    while (fs::space(area).available>leave) {
      const auto count=static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(block.size()),fs::space(area).available-leave));
      if (!count) break;
      const auto result=sink->write_at(offset,std::span(block).first(count));
      if (!result) { EXPECT_EQ(result.error().code,am::Errc::insufficient_space); break; }
      offset+=count;
    }
    auto flushed=sink->flush();
    if (!flushed) { EXPECT_EQ(flushed.error().code,am::Errc::insufficient_space); }
    take(sink->close());
  }
  void prime_state_wal() {
    auto db=am::db::Connection::borrow(catalog->library().database(am::library::DatabaseKind::state));
    auto q=take(db.prepare("INSERT INTO op_log(at,level,code,message) VALUES('2020-01-01T00:00:00Z','info','reserve',?)"));
    take(q.bind(1,std::string(2*1024*1024,'r'))); take(q.step());
    take(db.exec("DELETE FROM op_log WHERE code='reserve'; PRAGMA wal_checkpoint(RESTART)"));
  }
  fs::path zip() const { return asset_path(root,asset.ref.asset_id); }
  void space_error(const am::Error& e) const {
    EXPECT_EQ(e.code,am::Errc::insufficient_space)<<am::to_log_string(e);
    EXPECT_TRUE(e.allows(am::ErrorAction::free_space));
    EXPECT_TRUE(e.allows(am::ErrorAction::abort));
  }
  void clean_temporary() const {
    for (const auto& p:fs::recursive_directory_iterator(root/".am/.am-tmp")) EXPECT_FALSE(p.is_regular_file())<<p.path();
  }
  void userdata() {
    auto db=am::db::Connection::borrow(catalog->library().database(am::library::DatabaseKind::userdata));
    auto q=take(db.prepare("SELECT name FROM collections WHERE coll_id='confirmed'"));
    ASSERT_EQ(take(q.step()),am::db::Step::row);
    EXPECT_EQ(take(q.text(0)),"Favorite");
    EXPECT_EQ(take(q.step()),am::db::Step::done);
  }
};
}

TEST(SmallVolume, ChangePreflight) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  Volume v; const auto old=hash(v.zip());
  v.fill(128*1024); // hypothesis: smaller than the required new revision
  auto repo=take(am::repository::AssetRepository::open(v.root,v.options(false)));
  am::repository::ChangeRequest r; r.ref=v.asset.ref; r.expected_rev=1;
  r.ops.emplace_back(am::repository::SetMetadata{"asset","title",std::optional<Json>{std::in_place,Json("No space")}});
  auto changed=repo.change(r); ASSERT_FALSE(changed); v.space_error(changed.error());
  EXPECT_EQ(hash(v.zip()),old); v.clean_temporary(); v.userdata();
}
TEST(SmallVolume, ChangeDuringWrite) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  Volume v; const auto old=hash(v.zip());
  v.prime_state_wal();
  v.fill(128*1024);
  auto repo=take(am::repository::AssetRepository::open(v.root,v.options(true)));
  am::repository::ChangeRequest r; r.ref=v.asset.ref; r.expected_rev=1;
  r.ops.emplace_back(am::repository::SetMetadata{"asset","title",std::optional<Json>{std::in_place,Json("New revision cannot fit")}});
  auto changed=repo.change(r); ASSERT_FALSE(changed); v.space_error(changed.error());
  EXPECT_EQ(hash(v.zip()),old); v.clean_temporary(); v.userdata();
}
TEST(SmallVolume, FullExport) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  Volume v; const auto old=hash(v.zip());
  auto snapshot=take(v.catalog->library().assets().snapshot(v.asset.ref));
  v.fill(128*1024);
  am::storage::exporting::PrepareOptions o; o.target_dir=v.output;
  o.hooks.available_space=[](const fs::path&) { return UINT64_MAX; };
  auto result=am::storage::exporting::prepare(snapshot,o);
  ASSERT_FALSE(result); v.space_error(result.error());
  EXPECT_EQ(hash(v.zip()),old); EXPECT_TRUE(fs::is_empty(v.output)); v.userdata();
}
TEST(SmallVolume, ImportInput) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  Volume v; const auto old=hash(v.zip()),source=hash(v.source);
  am::import::ScanRequest scan; scan.roots={v.source}; scan.profile=take(am::application::load_import_profile());
  auto plan=take(am::import::scan(scan));
  for (auto& c:plan.value["candidates"]) { c["action"]="import"; for (auto& e:c["entries"]) if(e["decision"]=="ask")e["decision"]="include"; }
  const auto path=v.source.parent_path()/(am::Uuid::v4().to_string()+".json"); write(path,am::import::serialize_plan(plan));
  auto queued=take(am::import::enqueue_run(*v.catalog,path));
  v.catalog->library().assets()=take(am::repository::AssetRepository::open(v.root,v.options(true)));
  v.prime_state_wal(); v.fill(128*1024);
  am::jobs::HandlerRegistry registry; take(am::import::register_import_jobs(registry,*v.catalog,"small-volume"));
  am::jobs::ExecutorOptions options; options.auto_retries=0; options.limits={1,1,1,1,1,1,1,1};
  am::jobs::ResourceManager resources(options.limits);
  am::jobs::Executor executor(v.catalog->job_store(),resources,registry,options);
  auto executed=executor.run_until_idle();
  if (!executed) v.space_error(executed.error());
  else {
    auto report=take(am::import::run_summary(*v.catalog,queued));
    EXPECT_NE(report.dump().find("am.insufficient_space"),std::string::npos)<<report.dump();
    EXPECT_NE(report.at("candidates").at(0).at("status"),"created");
  }
  EXPECT_EQ(hash(v.zip()),old); EXPECT_EQ(hash(v.source),source); v.clean_temporary(); v.userdata();
}
TEST(SmallVolume, DatabaseFull) {
  RecordProperty("req","K-117|K-122|ADR-0007|ADR-0033");
  Volume v; const auto old=hash(v.zip());
  auto db=am::db::Connection::borrow(v.catalog->library().database(am::library::DatabaseKind::index));
  take(db.exec("PRAGMA wal_checkpoint(TRUNCATE)"));
  v.fill(0);
  auto result=v.catalog->indexer().rebuild();
  ASSERT_FALSE(result); v.space_error(result.error());
  EXPECT_EQ(hash(v.zip()),old);
  v.userdata();
}
TEST(SmallVolume, DiagnosticExportFull) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  Volume v; const auto old=hash(v.zip());
  v.fill(0);
  am::application::DiagnosticOptions options; options.library=v.root; options.output=v.output/"diagnostics.zip";
  auto result=am::application::export_diagnostics(options);
  ASSERT_FALSE(result); v.space_error(result.error());
  EXPECT_FALSE(fs::exists(options.output)); EXPECT_TRUE(fs::is_empty(v.output));
  EXPECT_EQ(hash(v.zip()),old);
  v.userdata();
}
TEST(SmallVolume, ApplicationLogFull) {
  RecordProperty("req","K-117|K-122|NFR-14.4.9");
  Volume v;const auto old=hash(v.zip());
  am::application::LoggingOptions options;options.directory=v.output;
  auto sink=take(am::application::open_file_log(options));
  AM_LOG(error,"fault","confirmed","Confirmed log record");
  std::map<fs::path,std::string> confirmed;
  for(const auto& p:fs::directory_iterator(v.output))if(p.is_regular_file())confirmed.emplace(p.path(),bytes(p.path()));
  EXPECT_FALSE(confirmed.empty());
  v.fill(0);
  const std::string large(16*1024,'x'); // hypothesis: larger than filesystem allocation slack
  for(unsigned i=0;i<4;++i)AM_LOG(error,"fault","unconfirmed",large);
  fs::remove(v.filler);
  AM_LOG(error,"fault","after_space","Space was freed");
  am::log::set_sink(nullptr);sink.reset();
  for(const auto& [path,prefix]:confirmed) {
    EXPECT_FALSE(prefix.empty());
    EXPECT_TRUE(bytes(path).starts_with(prefix));
  }
  std::string content;
  for(const auto& p:fs::directory_iterator(v.output))if(p.is_regular_file())content+=bytes(p.path());
  EXPECT_NE(content.find("log.dropped"),std::string::npos);
  EXPECT_NE(content.find("after_space"),std::string::npos);
  EXPECT_EQ(hash(v.zip()),old);v.userdata();
}
TEST(SmallVolume, InitConfigurationFull) {
  RecordProperty("req","K-117|K-122|ADR-0033");
  Volume v;const auto old=hash(v.zip());
  const auto target=v.area/"new-library";
  am::library::InitOptions options;
  options.before_config=[&]()->am::Result<void>{v.fill(0);return {};};
  const auto initialized=am::library::Library::init(target,options);
  ASSERT_FALSE(initialized);v.space_error(initialized.error());
  EXPECT_FALSE(fs::exists(target/"am-library.toml"));
  EXPECT_TRUE(fs::is_empty(target/".am/.am-tmp"));
  EXPECT_EQ(hash(v.zip()),old);v.userdata();
}
