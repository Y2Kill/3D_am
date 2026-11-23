#include <gtest/gtest.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <barrier>
#include <fstream>
#include <map>
#include <stdexcept>
#include <set>
#include <thread>
#include "am/contracts/job_store.hpp"
#include "am/foundation/uuid.hpp"
#include "am/storage/db/job_store.hpp"
#include "am/storage/db/operation_log.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/application/library_service.hpp"

namespace {
namespace fs=std::filesystem;
namespace c=am::contracts;
using c::JobState;
using am::db::StateJobStore;
using am::db::OperationLog;
using am::db::LogEntry;
using am::db::LogQuery;
template<class T> T value(am::Result<T> r) { if(!r) throw std::runtime_error(am::to_log_string(r.error())); return std::move(r).value(); }
void value(am::Result<void> r) { if(!r) throw std::runtime_error(am::to_log_string(r.error())); }
std::string id() { return am::Uuid::v4().to_string(); }
std::string bytes(const fs::path& p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
struct Database {
  fs::path path=fs::path(AM_LIBRARY_TEST_ROOT)/id()/"state.sqlite";
  sqlite3* raw=nullptr;
  std::chrono::system_clock::time_point now=std::chrono::sys_days(std::chrono::year(2026)/11/18);
  Database() {
    fs::create_directories(path.parent_path()); open(false);
    value(am::db::Connection::borrow(raw).exec(bytes(AM_STATE_JOB_SCHEMA)));
  }
  void open(bool ro) {
    if(raw) sqlite3_close(raw);
    const auto p=path.u8string();
    if(sqlite3_open_v2(reinterpret_cast<const char*>(p.c_str()),&raw,(ro ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE)|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) throw std::runtime_error("Fixture database open failed");
    sqlite3_busy_timeout(raw,5000); value(am::db::Connection::borrow(raw).exec("PRAGMA foreign_keys=ON"));
  }
  ~Database() { if(raw) sqlite3_close(raw); }
  auto clock() { return [this] { return now; }; }
  void exec(std::string_view sql) { value(am::db::Connection::borrow(raw).exec(sql)); }
};
c::JobRecord enqueue(StateJobStore& store,std::optional<std::string> parent={},int priority=0) {
  c::JobSpec spec; spec.type="import.asset"; spec.parent_id=std::move(parent); spec.priority=priority; return value(store.enqueue(spec));
}
std::string opaque=R"( { "n": 1e9999, "z": -0, "b": [1, 2] } )";
void expect_error(const auto& result,std::string_view mid) { ASSERT_FALSE(result); EXPECT_EQ(result.error().message_id,mid); }
}

TEST(JobContract, NamesAndAllTransitionsMatchIndependentTable) {
  RecordProperty("req","NFR-14.4.3");
  const std::array<std::string,10> names{"queued","running","paused","needs_decision","waiting_resources","resource_error","cannot_continue_safely","succeeded","failed","cancelled"};
  const std::array<std::set<int>,10> allowed{{{1,2,9},{0,2,3,4,5,6,7,8,9},{0,9},{0,9},{0,9},{0,8,9},{0,9},{},{0},{}}};
  const auto schema=bytes(AM_STATE_JOB_SCHEMA);
  for(int from=0;from<10;++from) {
    const auto state=static_cast<JobState>(from); EXPECT_EQ(c::job_state_name(state),names[static_cast<std::size_t>(from)]);
    EXPECT_EQ(c::parse_job_state(names[static_cast<std::size_t>(from)]),state);
    EXPECT_NE(schema.find(names[static_cast<std::size_t>(from)]),std::string::npos);
    EXPECT_EQ(c::job_terminal(state),from==7 || from==9);
    for(int to=0;to<10;++to) EXPECT_EQ(c::job_transition_allowed(state,static_cast<JobState>(to)),allowed[static_cast<std::size_t>(from)].contains(to)) << from << ':' << to;
  }
  EXPECT_FALSE(c::parse_job_state("unknown")); EXPECT_TRUE(c::job_state_name(static_cast<JobState>(100)).empty());
}
TEST(StateJobs, LifecyclePersistsExactJsonProgressAndDecisions) {
  RecordProperty("req","NFR-14.4.3"); Database db; std::string identity, finished;
  {
    StateJobStore store(db.raw,false,db.clock()); c::JobSpec spec; spec.type="import.asset"; spec.params_json=opaque;
    auto job=value(store.enqueue(spec)); identity=job.job_id; EXPECT_EQ(job.params_json,opaque); EXPECT_EQ(job.position,1); EXPECT_EQ(job.attempt,0);
    job=value(store.transition(identity,JobState::queued,JobState::running)); EXPECT_EQ(job.attempt,1); ASSERT_TRUE(job.started_at);
    value(store.report(identity,{{1},{},"files","Reading"}));
    c::JobPatch question; question.decision_json=opaque;
    EXPECT_EQ(value(store.transition(identity,JobState::running,JobState::needs_decision,question)).decision_json,opaque);
    EXPECT_FALSE(value(store.transition(identity,JobState::needs_decision,JobState::queued)).decision_json);
    db.now+=std::chrono::seconds(2);
    job=value(store.transition(identity,JobState::queued,JobState::running)); EXPECT_EQ(job.attempt,2);
    c::JobPatch result; result.result_json=opaque;
    job=value(store.transition(identity,JobState::running,JobState::succeeded,result)); EXPECT_EQ(job.result_json,opaque); ASSERT_TRUE(job.finished_at); finished=*job.finished_at;
  }
  db.open(false); StateJobStore store(db.raw,false,db.clock()); const auto job=*value(store.get(identity));
  EXPECT_EQ(job.params_json,opaque); EXPECT_EQ(job.result_json,opaque); EXPECT_EQ(job.finished_at,finished); EXPECT_EQ(job.progress.done,1u); EXPECT_FALSE(job.progress.total);
}
TEST(StateJobs, ForbiddenTransitionsAndConcurrentCompareAndSwap) {
  RecordProperty("req","NFR-14.4.3"); Database db; StateJobStore store(db.raw,false,db.clock());
  for(int from=0;from<10;++from) for(int to=0;to<10;++to) {
    if(c::job_transition_allowed(static_cast<JobState>(from),static_cast<JobState>(to))) continue;
    const auto job=enqueue(store);
    db.exec("UPDATE jobs SET state='"+std::string(c::job_state_name(static_cast<JobState>(from)))+"' WHERE job_id='"+job.job_id+"'");
    expect_error(store.transition(job.job_id,static_cast<JobState>(from),static_cast<JobState>(to)),"am.jobs.transition_invalid");
    EXPECT_EQ(value(store.get(job.job_id))->state,static_cast<JobState>(from));
  }
  auto job=enqueue(store); expect_error(store.report(job.job_id,{}),"am.jobs.transition_invalid");
  sqlite3* peer_raw=nullptr; const auto peer_path=db.path.u8string();
  ASSERT_EQ(sqlite3_open_v2(reinterpret_cast<const char*>(peer_path.c_str()),&peer_raw,SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX,nullptr),SQLITE_OK);
  std::unique_ptr<sqlite3,decltype(&sqlite3_close)> peer(peer_raw,sqlite3_close);
  sqlite3_busy_timeout(peer_raw,5000); StateJobStore other(peer_raw,false,db.clock());
  std::barrier start(3); std::optional<am::Result<c::JobRecord>> a,b;
  {
    std::jthread first([&] { start.arrive_and_wait(); a.emplace(store.transition(job.job_id,JobState::queued,JobState::running)); });
    std::jthread second([&] { start.arrive_and_wait(); b.emplace(other.transition(job.job_id,JobState::queued,JobState::running)); });
    start.arrive_and_wait();
  }
  ASSERT_TRUE(a); ASSERT_TRUE(b); EXPECT_EQ(static_cast<int>(a->ok())+static_cast<int>(b->ok()),1);
  expect_error(a->ok() ? *b : *a,"am.jobs.state_conflict");
  EXPECT_EQ(value(store.transition(job.job_id,JobState::running,JobState::queued)).state,JobState::queued);
}
TEST(StateJobs, InputValidationDoesNotWriteAndOpaqueFieldsStayExact) {
  RecordProperty("req","NFR-14.4.3"); Database db; StateJobStore store(db.raw,false,db.clock());
  for(const auto& type:std::vector<std::string>{"","A",".a","a.","a..b","a-b",std::string(65,'a')}) {
    c::JobSpec s; s.type=type; expect_error(store.enqueue(s),"am.jobs.type_invalid");
  }
  c::JobSpec s; s.type="verify"; s.params_json="{"; expect_error(store.enqueue(s),"am.jobs.json_invalid");
  s.params_json="{}"; s.job_id="not-a-uuid"; expect_error(store.enqueue(s),"am.jobs.id_invalid");
  s.job_id=am::Uuid::v7().to_string(); expect_error(store.enqueue(s),"am.jobs.id_invalid");
  EXPECT_TRUE(value(store.list()).empty());
  const auto parent=enqueue(store); s.job_id=parent.job_id; expect_error(store.enqueue(s),"am.jobs.exists");
  s.job_id.reset(); s.parent_id=id(); expect_error(store.enqueue(s),"am.jobs.parent_invalid");
  const auto child=enqueue(store,parent.job_id); s.parent_id=child.job_id; expect_error(store.enqueue(s),"am.jobs.parent_invalid");
  value(store.transition(parent.job_id,JobState::queued,JobState::cancelled)); s.parent_id=parent.job_id; expect_error(store.enqueue(s),"am.jobs.parent_invalid");
  const auto job=enqueue(store); value(store.transition(job.job_id,JobState::queued,JobState::running));
  for(int field=0;field<3;++field) {
    c::JobPatch patch;
    if(field==0) patch.decision_json="[";
    if(field==1) patch.result_json="[";
    if(field==2) patch.error_json="[";
    expect_error(store.transition(job.job_id,JobState::running,field==0 ? JobState::needs_decision : JobState::failed,patch),"am.jobs.json_invalid");
    EXPECT_EQ(value(store.get(job.job_id))->state,JobState::running);
  }
  const auto excess=static_cast<std::uint64_t>(INT64_MAX)+1;
  expect_error(store.report(job.job_id,{{excess},{},"",""}),"am.jobs.progress_invalid");
  expect_error(store.report(job.job_id,{{},{excess},"",""}),"am.jobs.progress_invalid");
  expect_error(store.report(job.job_id,{{2},{1},"",""}),"am.jobs.progress_invalid");
  c::JobPatch error; error.error_json=opaque;
  EXPECT_EQ(value(store.transition(job.job_id,JobState::running,JobState::failed,error)).error_json,opaque);
}
TEST(StateJobs, OrderingMovePreservesPriorityAndSiblingPositionsAreUnique) {
  RecordProperty("req","NFR-14.4.3"); Database db; StateJobStore store(db.raw,false,db.clock());
  const auto a=enqueue(store), b=enqueue(store), d=enqueue(store), high=enqueue(store,{},10);
  const auto parent=enqueue(store), child=enqueue(store,parent.job_id), child2=enqueue(store,parent.job_id);
  EXPECT_EQ(child.position,1); EXPECT_EQ(child2.position,2); EXPECT_EQ(value(store.list())[0].job_id,high.job_id);
  auto unique=[&] { std::map<std::optional<std::string>,std::set<std::int64_t>> positions; for(const auto& j:value(store.list())) EXPECT_TRUE(positions[j.parent_id].insert(j.position).second); };
  value(store.move_before(d.job_id,a.job_id)); unique(); EXPECT_LT(value(store.get(d.job_id))->position,value(store.get(a.job_id))->position);
  value(store.move_before(d.job_id,b.job_id)); unique(); EXPECT_LT(value(store.get(a.job_id))->position,value(store.get(d.job_id))->position);
  value(store.move_before(d.job_id,{})); unique(); EXPECT_GT(value(store.get(d.job_id))->position,value(store.get(b.job_id))->position);
  value(store.move_before(a.job_id,high.job_id)); unique(); EXPECT_EQ(value(store.get(a.job_id))->priority,0); EXPECT_EQ(value(store.list())[0].job_id,high.job_id);
  expect_error(store.move_before(a.job_id,child.job_id),"am.jobs.parent_invalid");
  value(store.transition(a.job_id,JobState::queued,JobState::running)); expect_error(store.move_before(a.job_id,{}),"am.jobs.transition_invalid");
  value(store.transition(a.job_id,JobState::running,JobState::paused)); value(store.move_before(a.job_id,b.job_id)); unique();
  value(store.transition(b.job_id,JobState::queued,JobState::cancelled)); expect_error(store.move_before(b.job_id,{}),"am.jobs.transition_invalid");
}
TEST(StateJobs, HierarchyFiltersAndCleanupProtectUnfinishedChildren) {
  RecordProperty("req","NFR-14.4.3"); Database db; StateJobStore store(db.raw,false,db.clock());
  const auto parent=enqueue(store), one=enqueue(store,parent.job_id), two=enqueue(store,parent.job_id), three=enqueue(store,parent.job_id);
  c::JobFilter children; children.parent_id=parent.job_id; EXPECT_EQ(value(store.list(children)).size(),3u);
  c::JobFilter roots; roots.top_level_only=true; EXPECT_EQ(value(store.list(roots)).size(),1u);
  value(store.transition(parent.job_id,JobState::queued,JobState::cancelled));
  for(const auto& job:{one,two}) { value(store.transition(job.job_id,JobState::queued,JobState::running)); value(store.transition(job.job_id,JobState::running,job.job_id==one.job_id ? JobState::succeeded : JobState::failed)); }
  EXPECT_EQ(value(store.get(three.job_id))->state,JobState::queued);
  EXPECT_EQ(value(store.remove_finished("2026-11-19T00:00:00Z")),2u); EXPECT_TRUE(value(store.get(parent.job_id)));
  db.now+=std::chrono::hours(48); value(store.transition(three.job_id,JobState::queued,JobState::cancelled));
  // A finished child is deleted by the parent's ON DELETE CASCADE, even if newer.
  EXPECT_EQ(value(store.remove_finished("2026-11-19T00:00:00Z")),2u); EXPECT_TRUE(value(store.list()).empty());
}
TEST(StateJobs, ReadOnlyRejectsEveryWriteWithoutChangingDatabase) {
  RecordProperty("req","NFR-14.4.3"); Database db; std::string job;
  { StateJobStore store(db.raw,false,db.clock()); job=enqueue(store).job_id; }
  db.open(true); const auto before=bytes(db.path); StateJobStore store(db.raw,true,db.clock());
  EXPECT_TRUE(value(store.get(job))); EXPECT_EQ(value(store.list()).size(),1u);
  c::JobSpec spec; spec.type="verify"; expect_error(store.enqueue(spec),"am.catalog.read_only");
  expect_error(store.transition(job,JobState::queued,JobState::running),"am.catalog.read_only");
  expect_error(store.report(job,{}),"am.catalog.read_only"); expect_error(store.move_before(job,{}),"am.catalog.read_only");
  expect_error(store.remove_finished("2099-01-01T00:00:00Z"),"am.catalog.read_only"); expect_error(store.recover_running(),"am.catalog.read_only");
  OperationLog log(db.raw,true,db.clock()); LogEntry entry; entry.level="info";
  expect_error(log.append(entry),"am.catalog.read_only"); expect_error(log.prune_before("2099"),"am.catalog.read_only"); EXPECT_TRUE(value(log.query()).empty());
  EXPECT_EQ(bytes(db.path),before);
}
TEST(OperationLog, ExactJsonFiltersSequencePruningAndValidation) {
  RecordProperty("req","NFR-14.4.9"); Database db; OperationLog log(db.raw,false,db.clock());
  LogEntry e; e.seq=999; e.at="ignored"; e.level="info"; e.code="library.verify"; e.message="Technical message"; e.job_id=id(); e.asset_id=id(); e.detail_json=opaque;
  const auto first=value(log.append(e)); db.now+=std::chrono::seconds(2); const auto second=value(log.append(e)); EXPECT_GT(second,first);
  EXPECT_NE(first,999); EXPECT_EQ(value(log.query())[0].at,"2026-11-18T00:00:00Z");
  LogQuery q; q.limit=1; EXPECT_EQ(value(log.query(q))[0].seq,first);
  q.after_seq=first; EXPECT_EQ(value(log.query(q))[0].seq,second); EXPECT_EQ(value(log.query(q))[0].detail_json,opaque);
  q.job_id=id(); EXPECT_TRUE(value(log.query(q)).empty()); q.job_id=e.job_id; q.asset_id=id(); EXPECT_TRUE(value(log.query(q)).empty()); q.asset_id=e.asset_id; EXPECT_EQ(value(log.query(q)).size(),1u);
  EXPECT_EQ(value(log.prune_before("2026-11-18T00:00:01Z")),1u); EXPECT_EQ(value(log.query())[0].seq,second);
  e.level="trace"; expect_error(log.append(e),"am.jobs.log_invalid"); e.level="info"; e.detail_json="{"; expect_error(log.append(e),"am.jobs.json_invalid");
  EXPECT_EQ(value(log.query()).size(),1u);
  e.detail_json="{}"; EXPECT_GT(value(log.append(e)),second);
}
namespace {
namespace cat=am::catalog;
namespace lib=am::library;
namespace repo=am::repository;
lib::OpenOptions options() { lib::OpenOptions o; o.capabilities.emplace(); o.capabilities->mode=repo::StorageMode::full; o.application_version="test"; return o; }
fs::path fresh() { const auto root=fs::path(AM_LIBRARY_TEST_ROOT)/id(); fs::create_directories(root.parent_path()); value(lib::Library::init(root)); return root; }
repo::ChangeResult create(cat::Catalog& catalog) {
  repo::CreateRequest request; request.metadata=repo::Json{{"title","Log fixture"}}; return value(catalog.create(request)).change;
}
}
TEST(CatalogLog, OperationsHaveOneEntryAndUnloggedOperationsStayUnlogged) {
  RecordProperty("req","NFR-14.4.9|K-31"); const auto source_root=fresh(), root=fresh();
  auto source=value(cat::Catalog::open(source_root,options())); value(source.indexer().rebuild()); const auto source_asset=create(source);
  const auto ready=source_root/"assets"/source_asset.ref.asset_id.substr(34)/(source_asset.ref.asset_id+".zip");
  auto catalog=value(cat::Catalog::open(root,options())); value(catalog.indexer().rebuild());
  OperationLog log(catalog.library().database(lib::DatabaseKind::state)); EXPECT_TRUE(value(log.query()).empty());
  const auto imported=value(catalog.import_ready({ready,{}})); EXPECT_EQ(imported.status,"imported");
  EXPECT_EQ(value(catalog.import_ready({ready,{}})).status,"identical");
  repo::ChangeRequest change; change.ref=source_asset.ref; change.expected_rev=1;
  repo::SetMetadata title; title.field="title"; title.value.emplace("New title"); change.ops.emplace_back(title); value(source.change(change));
  EXPECT_EQ(value(catalog.import_ready({ready,{}})).status,"conflict");
  const auto bad=source_root.parent_path()/(id()+".zip"); { std::ofstream f(bad,std::ios::binary); f << "not a ZIP"; }
  EXPECT_FALSE(catalog.import_ready({bad,{}}));
  auto rows=value(log.query()); ASSERT_EQ(rows.size(),4u);
  for(const auto& row:rows) { EXPECT_EQ(row.code,"library.import_ready"); const auto detail=repo::Json::parse(*row.detail_json); EXPECT_TRUE(detail.contains("classification")); EXPECT_TRUE(detail.contains("status")); }
  EXPECT_EQ(rows[0].level,"info"); EXPECT_EQ(rows[1].level,"warn"); EXPECT_EQ(rows[2].level,"warn"); EXPECT_EQ(rows[3].level,"error");
  for(std::size_t i=0;i<3;++i) EXPECT_EQ(rows[i].asset_id,source_asset.ref.asset_id);
  const auto copied=value(catalog.copy_independent({imported.ref,{},{}})).copy;
  const repo::AssetRef absent{catalog.library().config().library_id,id()}; EXPECT_FALSE(catalog.copy_independent({absent,{},{}}));
  rows=value(log.query()); ASSERT_EQ(rows.size(),6u); EXPECT_EQ(rows[4].code,"library.copy"); EXPECT_EQ(rows[4].asset_id,copied.ref.asset_id);
  EXPECT_EQ(repo::Json::parse(*rows[4].detail_json)["source_asset"]["asset_id"],imported.ref.asset_id);
  EXPECT_EQ(rows[5].level,"error"); EXPECT_EQ(rows[5].asset_id,absent.asset_id); EXPECT_TRUE(repo::Json::parse(*rows[5].detail_json).contains("message_id"));
  value(catalog.trash(copied.ref)); value(catalog.restore(copied.ref)); value(catalog.trash(copied.ref)); value(catalog.purge(copied.ref));
  rows=value(log.query()); ASSERT_EQ(rows.size(),10u);
  const std::array<std::string,4> kinds{"library.trash","library.restore","library.trash","library.purge"};
  for(std::size_t i=0;i<kinds.size();++i) { const auto& row=rows[6+i]; EXPECT_EQ(row.code,kinds[i]); EXPECT_EQ(row.asset_id,copied.ref.asset_id); EXPECT_TRUE(repo::Json::parse(*row.detail_json).contains("op_id")); }
  cat::LibraryVerifyOptions full; full.level=cat::LibraryVerifyLevel::full; EXPECT_EQ(value(catalog.verify(full)).ok,1u);
  rows=value(log.query()); ASSERT_EQ(rows.size(),11u); EXPECT_EQ(rows.back().code,"library.verify"); EXPECT_FALSE(rows.back().asset_id);
  const auto detail=repo::Json::parse(*rows.back().detail_json);
  for(auto key:{"level","total","checked","ok","warn","corrupt","unsupported","unreadable","cancelled"}) EXPECT_TRUE(detail.contains(key));
  const auto added=create(catalog); change.ref=added.ref; change.expected_rev=1; value(catalog.change(change));
  value(catalog.indexer().rebuild()); value(catalog.sync()); EXPECT_EQ(value(log.query()).size(),11u);
}
TEST(CatalogLog, FailedAppendWarnsWithoutUndoingAssetOrMarkingIndexForRebuild) {
  RecordProperty("req","NFR-14.4.9"); const auto root=fresh(); bool fail=false; cat::IndexerHooks hooks;
  hooks.before_log=[&](auto) -> am::Result<void> { if(fail) return am::Error::make(am::Errc::io_error,"Injected log failure"); return {}; };
  auto catalog=value(cat::Catalog::open(root,options(),hooks)); value(catalog.indexer().rebuild()); const auto asset=create(catalog);
  OperationLog log(catalog.library().database(lib::DatabaseKind::state)); fail=true;
  const auto moved=value(catalog.trash(asset.ref)); ASSERT_EQ(moved.warnings.size(),1u); EXPECT_EQ(moved.warnings[0].message_id,"am.catalog.log_failed");
  EXPECT_TRUE(fs::exists(root/"trash"/(asset.ref.asset_id+".zip"))); EXPECT_TRUE(value(catalog.indexer().list(true))[0].in_trash);
  EXPECT_FALSE(value(catalog.indexer().rebuild_required())); EXPECT_TRUE(value(log.query()).empty());
  const auto verify=value(catalog.verify()); EXPECT_EQ(verify.ok,1u); ASSERT_EQ(verify.warnings.size(),1u); EXPECT_EQ(verify.warnings[0].message_id,"am.catalog.log_failed");
  EXPECT_FALSE(value(catalog.indexer().rebuild_required()));
  fail=false; value(catalog.restore(asset.ref)); EXPECT_EQ(value(log.query()).size(),1u);
}
