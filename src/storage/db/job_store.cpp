#include "am/storage/db/job_store.hpp"
#include "am/foundation/json_text.hpp"
#include "am/foundation/uuid.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace am::db {
namespace {
struct ConnectionLock {
  sqlite3_mutex* mutex;
  explicit ConnectionLock(sqlite3* raw) : mutex(raw ? sqlite3_db_mutex(raw) : nullptr) { sqlite3_mutex_enter(mutex); }
  ~ConnectionLock() { sqlite3_mutex_leave(mutex); }
};
using namespace contracts;
constexpr auto columns = "job_id,type,params_json,parent_id,state,priority,position,attempt,progress_done,progress_total,progress_unit,stage,decision_json,result_json,error_json,created_at,started_at,finished_at";
template<class T> T take(Result<T> r) { if (!r) throw r.error(); return std::move(r).value(); }
void take(Result<void> r) { if (!r) throw r.error(); }
Error error(std::string id, Errc code = Errc::invalid_argument) {
  return Error::make(code,"Job store operation failed").in("storage.db").id(std::move(id));
}
template<class T, class F> Result<T> guard(F&& f) {
  try { return f(); }
  catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return error("am.db.out_of_memory",Errc::resource_exhausted); }
  catch (...) { return error("am.jobs.store_error",Errc::internal); }
}
void validate_id(std::string_view id) {
  const auto u=Uuid::parse_canonical(id);
  if (!u || u->version()!=4 || (u->bytes()[8]&0xc0)!=0x80) throw error("am.jobs.id_invalid");
}
void validate_json(const std::optional<std::string>& value) {
  if (value && !json::parse_strict(*value)) throw error("am.jobs.json_invalid");
}
void optional_bind(Statement& s,int i,const std::optional<std::string>& value) { if(value) take(s.bind(i,*value)); else take(s.bind_null(i)); }
std::optional<std::string> text(Statement& s,int i) { return s.is_null(i) ? std::nullopt : std::optional(take(s.text(i))); }
JobRecord record(Statement& s) {
  JobRecord r; r.job_id=take(s.text(0)); r.type=take(s.text(1)); r.params_json=take(s.text(2)); r.parent_id=text(s,3);
  const auto state=parse_job_state(take(s.text(4))); if(!state) throw error("am.jobs.store_error",Errc::corrupt_data);
  r.state=*state; r.priority=static_cast<int>(s.integer(5)); r.position=s.integer(6); r.attempt=static_cast<int>(s.integer(7));
  if(!s.is_null(8)) r.progress.done=static_cast<std::uint64_t>(s.integer(8));
  if(!s.is_null(9)) r.progress.total=static_cast<std::uint64_t>(s.integer(9));
  r.progress.unit=take(s.text(10)); r.progress.stage=take(s.text(11));
  r.decision_json=text(s,12); r.result_json=text(s,13); r.error_json=text(s,14);
  r.created_at=take(s.text(15)); r.started_at=text(s,16); r.finished_at=text(s,17); return r;
}
std::string timestamp(std::chrono::system_clock::time_point now) {
  const auto t=std::chrono::system_clock::to_time_t(now); std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm,&t);
#else
  gmtime_r(&t,&tm);
#endif
  std::ostringstream out; out << std::put_time(&tm,"%Y-%m-%dT%H:%M:%SZ"); return out.str();
}
bool valid_type(std::string_view type) {
  if(type.empty() || type.size()>64) return false;
  bool first=true;
  for(char c:type) {
    if(c=='.') { if(first) return false; first=true; continue; }
    const bool lower=c>='a' && c<='z';
    if(first && !lower) return false;
    if(!lower && !(c>='0' && c<='9') && c!='_') return false;
    first=false;
  }
  return !first;
}
}
StateJobStore::StateJobStore(sqlite3* raw,bool ro,Clock clock,int maximum)
  : raw_(raw),db_(Connection::borrow(raw)),read_only_(ro),clock_(clock ? std::move(clock) : Clock(std::chrono::system_clock::now)),max_attempts_(maximum) {}
void StateJobStore::writable() const {
  if(read_only_ || (raw_ && sqlite3_db_readonly(raw_,"main")==1)) throw error("am.catalog.read_only",Errc::read_only);
}
Result<std::optional<JobRecord>> StateJobStore::get(std::string_view id) {
  return guard<std::optional<JobRecord>>([&]() -> Result<std::optional<JobRecord>> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_); validate_id(id);
    auto s=take(db_.prepare(std::string("SELECT ")+columns+" FROM jobs WHERE job_id=?")); take(s.bind(1,id));
    if(take(s.step())!=Step::row) return std::optional<JobRecord>{};
    return std::optional(record(s));
  });
}
Result<std::vector<JobRecord>> StateJobStore::list(const JobFilter& filter) {
  return guard<std::vector<JobRecord>>([&]() -> Result<std::vector<JobRecord>> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_);
    if(filter.parent_id) validate_id(*filter.parent_id);
    std::string sql=std::string("SELECT ")+columns+" FROM jobs WHERE 1=1";
    if(filter.parent_id) sql+=" AND parent_id=?";
    if(filter.top_level_only) sql+=" AND parent_id IS NULL";
    const std::set<JobState> states(filter.states.begin(),filter.states.end());
    if(!states.empty()) {
      sql+=" AND state IN ("; bool first=true;
      for(auto state:states) { (void)state; sql+=first ? "?" : ",?"; first=false; }
      sql+=')';
    }
    auto s=take(db_.prepare(sql+" ORDER BY priority DESC,position ASC,job_id ASC"));
    int binding=0; if(filter.parent_id) take(s.bind(++binding,*filter.parent_id)); for(auto state:states) take(s.bind(++binding,job_state_name(state)));
    std::vector<JobRecord> out;
    while(take(s.step())==Step::row) {
      auto r=record(s);
      out.push_back(std::move(r));
    }
    return out;
  });
}
Result<JobRecord> StateJobStore::enqueue(const JobSpec& spec) {
  return guard<JobRecord>([&]() -> Result<JobRecord> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_); writable();
    if(!valid_type(spec.type)) throw error("am.jobs.type_invalid");
    validate_json(spec.params_json); const auto id=spec.job_id.value_or(Uuid::v4().to_string()); validate_id(id);
    auto tx=take(Transaction::begin(db_));
    if(take(get(id))) throw error("am.jobs.exists",Errc::conflict);
    if(spec.parent_id) {
      const auto u=Uuid::parse_canonical(*spec.parent_id);
      if(!u || u->version()!=4 || (u->bytes()[8]&0xc0)!=0x80) throw error("am.jobs.parent_invalid");
      const auto parent=take(get(*spec.parent_id));
      if(!parent || job_terminal(parent->state) || parent->parent_id) throw error("am.jobs.parent_invalid");
    }
    std::int64_t position=0;
    { auto q=take(db_.prepare("SELECT COALESCE(MAX(position),0) FROM jobs WHERE parent_id IS ?")); optional_bind(q,1,spec.parent_id); take(q.step()); position=q.integer(0); }
    if(position==(std::numeric_limits<std::int64_t>::max)()) throw error("am.jobs.position_overflow");
    auto q=take(db_.prepare("INSERT INTO jobs(job_id,type,params_json,parent_id,state,priority,position,created_at) VALUES(?,?,?,?,'queued',?,?,?)"));
    take(q.bind(1,id)); take(q.bind(2,spec.type)); take(q.bind(3,spec.params_json)); optional_bind(q,4,spec.parent_id);
    take(q.bind(5,static_cast<std::int64_t>(spec.priority))); take(q.bind(6,position+1)); take(q.bind(7,timestamp(clock_()))); take(q.step());
    auto r=take(get(id)); take(tx.commit()); return *r;
  });
}
Result<JobRecord> StateJobStore::transition(std::string_view id,JobState from,JobState to,const JobPatch& patch) {
  return guard<JobRecord>([&]() -> Result<JobRecord> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_); writable(); validate_id(id);
    if(!job_transition_allowed(from,to) || (patch.release_attempt && (from!=JobState::running || to!=JobState::queued)) ||
       (to==JobState::needs_decision && (!patch.decision_json || patch.clear_decision)) ||
       (patch.decision_json && to!=JobState::needs_decision && from!=JobState::needs_decision))
      throw error("am.jobs.transition_invalid");
    validate_json(patch.decision_json); validate_json(patch.result_json); validate_json(patch.error_json);
    const bool starting=to==JobState::running;
    const bool finished=job_terminal(to) || to==JobState::failed;
    const auto at=timestamp(clock_());
    auto q=take(db_.prepare(std::string("UPDATE jobs SET state=?,attempt=max(0,attempt+?),started_at=CASE WHEN ? THEN ? ELSE started_at END,finished_at=CASE WHEN ? THEN ? WHEN ? THEN NULL ELSE finished_at END,decision_json=CASE WHEN ? THEN ? ELSE COALESCE(?,decision_json) END,result_json=CASE WHEN ?3 THEN CASE WHEN COALESCE(json_type(result_json),'')='object' AND (SELECT COUNT(*) FROM json_each(result_json))=1 AND COALESCE(json_type(result_json,'$.checkpoint'),'')='object' THEN result_json ELSE NULL END ELSE COALESCE(?,result_json) END,error_json=COALESCE(?,error_json) WHERE job_id=? AND state=? RETURNING ")+columns));
    take(q.bind(1,job_state_name(to))); take(q.bind(2,static_cast<std::int64_t>(starting ? 1 : patch.release_attempt ? -1 : 0)));
    take(q.bind(3,static_cast<std::int64_t>(starting))); take(q.bind(4,at));
    take(q.bind(5,static_cast<std::int64_t>(finished))); take(q.bind(6,at)); take(q.bind(7,static_cast<std::int64_t>(starting)));
    take(q.bind(8,static_cast<std::int64_t>(patch.clear_decision || from==JobState::needs_decision)));
    optional_bind(q,9,patch.decision_json); optional_bind(q,10,patch.decision_json); optional_bind(q,11,patch.result_json); optional_bind(q,12,patch.error_json);
    take(q.bind(13,id)); take(q.bind(14,job_state_name(from)));
    if(take(q.step())!=Step::row) throw error("am.jobs.state_conflict",Errc::conflict);
    auto r=record(q); take(q.step()); return r;
  });
}
Result<void> StateJobStore::report(std::string_view id,const JobProgress& progress) {
  return guard<void>([&]() -> Result<void> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_); writable(); validate_id(id);
    constexpr auto max=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if((progress.done && *progress.done>max) || (progress.total && *progress.total>max) ||
       (progress.done && progress.total && *progress.done>*progress.total)) throw error("am.jobs.progress_invalid");
    auto q=take(db_.prepare("UPDATE jobs SET progress_done=?,progress_total=?,progress_unit=?,stage=? WHERE job_id=? AND state='running' RETURNING job_id"));
    if(progress.done) take(q.bind(1,static_cast<std::int64_t>(*progress.done))); else take(q.bind_null(1));
    if(progress.total) take(q.bind(2,static_cast<std::int64_t>(*progress.total))); else take(q.bind_null(2));
    take(q.bind(3,progress.unit)); take(q.bind(4,progress.stage)); take(q.bind(5,id));
    if(take(q.step())!=Step::row) throw error("am.jobs.transition_invalid");
    take(q.step()); return {};
  });
}
Result<void> StateJobStore::checkpoint(std::string_view id, std::string_view json) {
  return guard<void>([&]() -> Result<void> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_); writable(); validate_id(id);
    take(contracts::validate_job_checkpoint(json));
    auto q = take(db_.prepare("UPDATE jobs SET result_json=? WHERE job_id=? AND state='running' RETURNING job_id"));
    take(q.bind(1,json)); take(q.bind(2,id));
    if (take(q.step()) != Step::row) throw error("am.jobs.state_conflict",Errc::conflict);
    take(q.step()); return {};
  });
}
Result<void> StateJobStore::move_before(std::string_view id,std::optional<std::string_view> before) {
  return guard<void>([&]() -> Result<void> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_); writable(); validate_id(id); if(before) validate_id(*before);
    auto tx=take(Transaction::begin(db_)); const auto moving=take(get(id));
    if(!moving || (moving->state!=JobState::queued && moving->state!=JobState::paused)) throw error("am.jobs.transition_invalid");
    if(before) {
      const auto target=take(get(*before));
      if(!target || target->parent_id!=moving->parent_id) throw error("am.jobs.parent_invalid");
    }
    std::vector<std::string> ids;
    { auto q=take(db_.prepare("SELECT job_id FROM jobs WHERE parent_id IS ? ORDER BY position,job_id")); optional_bind(q,1,moving->parent_id);
      while(take(q.step())==Step::row) { auto next=take(q.text(0)); if(next!=id) ids.push_back(std::move(next)); } }
    if(before && *before==id) return tx.commit();
    auto at=before ? std::find(ids.begin(),ids.end(),*before) : ids.end(); ids.insert(at,std::string(id));
    std::int64_t position=0;
    for(const auto& next:ids) { auto q=take(db_.prepare("UPDATE jobs SET position=? WHERE job_id=?")); take(q.bind(1,++position)); take(q.bind(2,next)); take(q.step()); }
    return tx.commit();
  });
}
Result<std::size_t> StateJobStore::remove_finished(std::string_view before) {
  return guard<std::size_t>([&]() -> Result<std::size_t> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_); writable();
    auto tx=take(Transaction::begin(db_));
    std::int64_t original=0;
    { auto count=take(db_.prepare("SELECT count(*) FROM jobs")); take(count.step()); original=count.integer(0); }
    auto q=take(db_.prepare("DELETE FROM jobs WHERE state IN ('succeeded','cancelled','failed') AND finished_at<? AND NOT EXISTS(SELECT 1 FROM jobs child WHERE child.parent_id=jobs.job_id AND (child.state NOT IN ('succeeded','cancelled','failed') OR child.finished_at IS NULL)) RETURNING job_id"));
    take(q.bind(1,before)); while(take(q.step())==Step::row) {}
    std::int64_t remaining=0;
    { auto count=take(db_.prepare("SELECT count(*) FROM jobs")); take(count.step()); remaining=count.integer(0); }
    take(tx.commit()); return static_cast<std::size_t>(original-remaining);
  });
}
Result<RecoveredJobs> StateJobStore::recover_running() {
  return guard<RecoveredJobs>([&]() -> Result<RecoveredJobs> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_); writable();
    if(max_attempts_<1) throw error("am.jobs.attempts_invalid");
    auto tx=take(Transaction::begin(db_)); JobFilter filter; filter.states={JobState::running};
    const auto jobs=take(list(filter)); RecoveredJobs out;
    for(const auto& job:jobs) {
      const bool exhausted=job.attempt>=max_attempts_;
      auto q=take(db_.prepare("UPDATE jobs SET state=?,error_json=CASE WHEN ? THEN ? ELSE error_json END,finished_at=CASE WHEN ? THEN ? ELSE finished_at END WHERE job_id=? AND state='running' RETURNING job_id"));
      take(q.bind(1,exhausted ? std::string_view("failed") : std::string_view("queued")));
      take(q.bind(2,static_cast<std::int64_t>(exhausted)));
      take(q.bind(3,std::string_view(R"({"code":"am.incomplete","message_id":"am.jobs.attempts_exhausted"})")));
      take(q.bind(4,static_cast<std::int64_t>(exhausted))); take(q.bind(5,timestamp(clock_()))); take(q.bind(6,job.job_id));
      if(take(q.step())!=Step::row) throw error("am.jobs.state_conflict",Errc::conflict);
      take(q.step()); (exhausted ? out.failed : out.requeued).push_back(job.job_id);
    }
    take(tx.commit()); return out;
  });
}
} // namespace am::db
