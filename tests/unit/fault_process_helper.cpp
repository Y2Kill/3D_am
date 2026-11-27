#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include "fault_world.hpp"
#include "am/services/export/run.hpp"
#include "am/application/logging.hpp"
#include "am/application/import_service.hpp"
#include "am/services/import/run.hpp"
#include "fault_import.hpp"
#include "fault_barrier_sink.hpp"

namespace {
using namespace fault_test;
std::string wanted;
fs::path marker;
fs::path utf8_path(std::string_view text) {
  std::u8string value(text.size(),u8'\0');
  if (!text.empty()) std::memcpy(value.data(),text.data(),text.size());
  return fs::path(value);
}
void hit(std::string_view stage) {
  if (stage!=wanted) return;
  write(marker,stage);
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(60); // hypothesis: abandoned child guard
  while (std::chrono::steady_clock::now()<until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::_Exit(98);
}
Json fixture(const fs::path& root) { return Json::parse(bytes(root.parent_path()/"fixture.json")); }
am::repository::RepositoryOptions repository_options(am::catalog::Catalog&,bool hooks=true);
void execute_import(am::catalog::Catalog& c,const Json& j,bool inject) {
  if (inject) c.library().assets()=take(am::repository::AssetRepository::open(c.library().root(),repository_options(c)));
  am::jobs::HandlerRegistry registry;
  am::import::RunHooks hooks;
  if (inject) hooks.after_created=[](std::string_view)->am::Result<void> { hit("candidate"); return {}; };
  take(am::import::register_import_jobs(registry,c,"fault",hooks));
  am::jobs::ExecutorOptions options; options.auto_retries=0;
  options.limits={1,1,1,1,1,1,1,1};
  am::jobs::ResourceManager resources(options.limits);
  am::jobs::Executor executor(c.job_store(),resources,registry,options);
  take(executor.run_until_idle());
  am::import::QueuedRun q; q.job_id=j.at("job_id"); q.plan_sha256=j.at("plan_sha");
  const auto report=take(am::import::run_summary(c,q));
  for (const auto& row:report.at("candidates"))
    if (row.at("status")!="created" && row.at("status")!="already_imported")
      throw std::runtime_error("Import did not resume successfully: "+report.dump());
  std::cout<<report.dump()<<'\n';
}
am::Result<std::shared_ptr<am::asset_zip::RandomAccessSink>> barrier_sink(const fs::path& path) {
  auto sink=std::make_shared<BarrierSink>(); sink->inner=take(am::asset_zip::file_sink(path)); sink->point=hit;
  return std::static_pointer_cast<am::asset_zip::RandomAccessSink>(sink);
}
void execute_export(am::catalog::Catalog& c,const Json& j,bool inject) {
  am::jobs::HandlerRegistry registry;
  am::jobs::ExecutorOptions options; options.auto_retries=0; options.limits={1,1,1,1,1,1,1,1};
  am::jobs::ResourceManager resources(options.limits);
  am::exporting::RunHooks hooks;
  if (inject) {
    hooks.storage.create_sink=barrier_sink;
    hooks.before_publish=[](const fs::path&) { hit("before_publish"); };
    hooks.after_publish=[](const fs::path&) { hit("after_publish"); };
  }
  take(am::exporting::register_export_jobs(registry,c,resources,hooks));
  am::jobs::Executor executor(c.job_store(),resources,registry,options);
  take(executor.run_until_idle());
  const auto report=take(am::exporting::export_summary(c,j.at("export_job").get<std::string>()));
  for (const auto& row:report.at("assets")) if (row.at("status")!="exported")
    throw std::runtime_error("Export did not resume successfully: "+report.dump());
  std::cout<<report.dump()<<'\n';
}
void diagnostic(const fs::path& root,bool inject) {
  am::application::DiagnosticOptions o;
  o.library=root;
  o.output=root.parent_path()/"diagnostic.zip";
  if (inject) o.create_sink=barrier_sink;
  take(am::application::export_diagnostics(o));
  if (inject) hit("published");
  std::cout<<Json{{"published",true}}.dump()<<'\n';
}
am::repository::RepositoryOptions repository_options(am::catalog::Catalog& c,bool hooks) {
  am::repository::RepositoryOptions o;
  o.library_id=c.library().config().library_id;
  o.capabilities=c.library().capabilities();
  o.on_indexed=[&c](const am::repository::IndexedEvent& event) { take(c.indexer().refresh(event.ref.asset_id)); };
  if (hooks) {
    o.test_hooks.commit.mid_write_after_bytes=1;
    o.test_hooks.commit.point=[](am::commit::FaultPoint point)->am::Result<void> { hit(am::commit::point_name(point)); return {}; };
  }
  return o;
}
void seed(const fs::path& root,std::string_view mode) {
  fs::create_directories(root.parent_path());
  Json j=Json::object();
  if (mode!="init") {
    take(am::library::Library::init(root));
    auto c=take(am::catalog::Catalog::open(root,writable()));
    auto source=root.parent_path()/"source.bin";
    write(source,std::string(8192,'x'));
    const auto created=take(c.create(request(source))).change;
    j={{"library_id",created.ref.library_id},{"asset_id",created.ref.asset_id},
       {"copy_id",am::Uuid::v4().to_string()},{"source",source.generic_string()}};
    auto user=am::db::Connection::borrow(c.library().database(am::library::DatabaseKind::userdata));
    take(user.exec("INSERT INTO collections(coll_id,name,kind,created_at) VALUES('confirmed','Favorite','favorites','2020-01-01T00:00:00Z')"));
    if (mode=="restore" || mode=="purge") take(c.trash(created.ref));
    if (mode=="ready" || mode=="ready_replace") {
      const auto original=fs::path(AM_GOLDEN_FIXTURES)/"pos-minimal/asset.zip";
      auto incoming=root.parent_path()/"ready.zip";
      if (mode=="ready") fs::copy_file(original,incoming);
      else {
        auto accepted=take(c.import_ready({original,{},{}}));
        const auto id=accepted.ref.asset_id;
        const auto ref=am::repository::AssetRef{c.library().config().library_id,id};
        auto snapshot=take(c.library().assets().snapshot(ref));
        j["replace_expected"]=snapshot.base().manifest_sha256;
        const auto other=root.parent_path()/"incoming-library";
        take(am::library::Library::init(other));
        auto b=take(am::catalog::Catalog::open(other,writable()));
        take(b.import_ready({original,{},{}}));
        am::repository::ChangeRequest change;
        change.ref={b.library().config().library_id,id}; change.expected_rev=1;
        change.ops.emplace_back(am::repository::SetMetadata{"asset","title",std::optional<Json>{std::in_place,Json("Incoming revision")}});
        take(b.change(change));
        fs::copy_file(asset_path(other,id),incoming);
      }
      j["ready_source"]=incoming.generic_string();
    }
    if (mode=="sync" || mode=="rebuild") {
      auto o=repository_options(c,false); o.on_indexed={};
      auto repo=take(am::repository::AssetRepository::open(root,o));
      take(repo.create(request(source,"Unobserved second asset")));
    }
    if (mode.starts_with("import_")) import_fixture(c,root.parent_path(),mode,j);
    if (mode.starts_with("export_")) {
      const auto output=root.parent_path()/"exports";
      fs::create_directories(output);
      write(output/"confirmed.txt","Confirmed previous external file");
      am::exporting::ExportRequest r;
      r.asset_ids={created.ref.asset_id}; r.target_dir=output; r.name="result";
      if (mode=="export_files") r.presets={am::exporting::Preset::all};
      j["export_job"]=take(am::exporting::enqueue_export(c,r));
    }
  }
  write(root.parent_path()/"fixture.json",j.dump());
  std::cout<<j.dump()<<'\n';
}
void run(const fs::path& root,std::string_view mode) {
  if (mode=="diagnostic") { diagnostic(root,true); throw std::runtime_error("Diagnostic checkpoint was not reached"); }
  if (mode=="init") {
    am::library::InitOptions o;
    o.before_config=[]()->am::Result<void> { hit("configuration"); return {}; };
    take(am::library::Library::init(root,o));
    return;
  }
  if (mode=="state_lock" || mode=="wal_loss") {
    if (mode=="wal_loss") {
      auto library=take(am::library::Library::open(root,writable()));
      am::db::IndexStore index(library.database(am::library::DatabaseKind::index));
      take(index.set_meta("writer_session","open"));
    }
    auto db=take(am::db::Connection::open(root/".am/state.sqlite"));
    take(db.exec("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0"));
    auto tx=take(am::db::Transaction::begin(db));
    if (mode=="wal_loss") {
      take(db.exec("INSERT INTO op_log(at,level,code,message) VALUES('2020-01-01T00:00:00Z','info','wal_only','confirmed in WAL')"));
      take(tx.commit());
      const auto saved=fixture(root);
      am::repository::RepositoryOptions o;
      o.library_id=saved.at("library_id");o.capabilities=writable().capabilities.value();
      auto repo=take(am::repository::AssetRepository::open(root,o));
      am::repository::ChangeRequest change;change.ref={o.library_id,saved.at("asset_id")};change.expected_rev=1;
      change.ops.emplace_back(am::repository::SetMetadata{"asset","title",std::optional<Json>{std::in_place,"Confirmed before WAL loss"}});
      take(repo.change(change)); // No index observer: next ordinary open must detect the new ZIP by stat.
      hit("committed");
    } else hit("transaction");
    throw std::runtime_error("Requested state checkpoint was not reached");
  }
  am::catalog::IndexerHooks hooks;
  if (mode=="rebuild") hooks.after_rebuild_insert=[](std::size_t)->am::Result<void> { hit("transaction"); return {}; };
  if (mode=="sync") hooks.after_sync_change=[](std::size_t)->am::Result<void> { hit("transaction"); return {}; };
  if (mode=="recovery") hooks.before_update=[](std::string_view)->am::Result<void> { hit("index"); return {}; };
  auto c=take(am::catalog::Catalog::open(root,writable(),hooks));
  const auto j=fixture(root);
  if (mode.starts_with("import_")) {
    execute_import(c,j,true);
    throw std::runtime_error("Requested import checkpoint was not reached");
  }
  if (mode.starts_with("export_")) {
    execute_export(c,j,true);
    throw std::runtime_error("Requested export checkpoint was not reached");
  }
  const am::repository::AssetRef ref{j.at("library_id"),j.at("asset_id")};
  c.library().assets()=take(am::repository::AssetRepository::open(root,repository_options(c)));
  if (mode=="trash") take(c.trash(ref));
  else if (mode=="restore") take(c.restore(ref));
  else if (mode=="purge") take(c.purge(ref));
  else if (mode=="copy") take(c.copy_independent({ref,j.at("copy_id").get<std::string>(),{}}));
  else if (mode=="change") {
    am::repository::ChangeRequest change; change.ref=ref; change.expected_rev=1;
    change.ops.emplace_back(am::repository::SetMetadata{"asset","title",std::optional<Json>{std::in_place,Json("Changed after crash")}});
    take(c.change(change));
  } else if (mode=="ready" || mode=="ready_replace") {
    am::repository::ImportReadyRequest r;
    r.source=utf8_path(j.at("ready_source").get<std::string>());
    if (mode=="ready_replace") r.replace_expected=j.at("replace_expected").get<std::string>();
    take(c.import_ready(r));
  } else if (mode=="rebuild") take(c.indexer().rebuild());
  else if (mode=="sync") take(c.sync());
  else if (mode=="operation_log" || mode=="state_lock") {
    auto db=am::db::Connection::borrow(c.library().database(am::library::DatabaseKind::state));
    auto tx=take(am::db::Transaction::begin(db));
    if (mode=="operation_log") take(db.exec("INSERT INTO op_log(at,level,code,message) VALUES('2020-01-01T00:00:00Z','info','unconfirmed','unconfirmed')"));
    hit("transaction"); take(tx.commit());
  } else if (mode!="recovery") throw std::runtime_error("Unknown fault operation");
  throw std::runtime_error("Requested checkpoint was not reached");
}
void recover(const fs::path& root,bool rebuild) {
  auto c=take(am::catalog::Catalog::open(root,writable()));
  if (rebuild) take(c.indexer().rebuild());
  else take(c.sync());
  am::catalog::LibraryVerifyOptions options; options.level=am::catalog::LibraryVerifyLevel::full;
  const auto verified=take(c.verify(options));
  if (verified.corrupt || verified.unreadable || verified.database_failed) throw std::runtime_error("Recovered library failed full verification");
  auto db=am::db::Connection::borrow(c.library().database(am::library::DatabaseKind::state));
  auto q=take(db.prepare("SELECT count(*) FROM pending_ops WHERE state='prepared'")); take(q.step());
  if (q.integer(0)) throw std::runtime_error("Prepared journal row remains after recovery");
  Json result={{"recovered",Json::array()}};
  for (const auto& item:c.open_report().recovered)
    result["recovered"].push_back({{"path",item.path.generic_string()},{"outcome",am::commit::outcome_name(item.outcome)},{"detail",item.detail}});
  std::cout<<result.dump()<<'\n';
}
}

int main(int argc,char** argv) {
  try {
    if (argc<3) throw std::runtime_error("Usage: fault-helper seed|run|recover|rebuild ROOT [MODE POINT MARKER]");
    const auto root=fs::absolute(utf8_path(argv[2]));
    const std::string command=argv[1];
    if (command=="seed" && argc==4) seed(root,argv[3]);
    else if (command=="run" && argc==6) { wanted=argv[4]; marker=utf8_path(argv[5]); run(root,argv[3]); }
    else if (command=="recover" || command=="rebuild") recover(root,command=="rebuild");
    else if (command=="try_open" && argc==4) {
      auto o=writable();
      if (std::string_view(argv[3])=="ro") o.mode=am::library::Mode::read_only;
      auto c=am::catalog::Catalog::open(root,o);
      Json result={{"opened",static_cast<bool>(c)}};
      if (!c) { result["code"]=static_cast<int>(c.error().code); result["message_id"]=c.error().message_id; }
      std::cout<<result.dump()<<'\n';
    }
    else if (command=="resume_import") {
      auto c=take(am::catalog::Catalog::open(root,writable()));
      execute_import(c,fixture(root),false);
    }
    else if (command=="resume_export") {
      auto c=take(am::catalog::Catalog::open(root,writable()));
      execute_export(c,fixture(root),false);
    }
    else if (command=="resume_diagnostic") diagnostic(root,false);
    else if (command=="try_diagnostic") {
      am::application::DiagnosticOptions o; o.library=root; o.output=root.parent_path()/"diagnostic.zip";
      auto result=am::application::export_diagnostics(o);
      Json report={{"published",static_cast<bool>(result)}};
      if (!result) report["message_id"]=result.error().message_id;
      std::cout<<report.dump()<<'\n';
    }
    else throw std::runtime_error("Invalid fault helper arguments");
    return 0;
  } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; }
}
