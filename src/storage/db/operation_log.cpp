#include "am/storage/db/operation_log.hpp"
#include "am/foundation/json_text.hpp"
#include <sqlite3.h>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

namespace am::db {
namespace {
struct ConnectionLock {
  sqlite3_mutex* mutex;
  explicit ConnectionLock(sqlite3* raw) : mutex(raw ? sqlite3_db_mutex(raw) : nullptr) { sqlite3_mutex_enter(mutex); }
  ~ConnectionLock() { sqlite3_mutex_leave(mutex); }
};
template<class T> T take(Result<T> r) { if (!r) throw r.error(); return std::move(r).value(); }
void take(Result<void> r) { if (!r) throw r.error(); }
Error error(Errc c, std::string id) { return Error::make(c,"Operation log failed").in("storage.db").id(std::move(id)); }
template<class T, class F> Result<T> guard(F&& f) {
  try { return f(); }
  catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return error(Errc::resource_exhausted,"am.db.out_of_memory"); }
  catch (...) { return error(Errc::internal,"am.jobs.log_error"); }
}
void optional_bind(Statement& s, int i, const std::optional<std::string>& v) { if (v) take(s.bind(i,*v)); else take(s.bind_null(i)); }
std::optional<std::string> text(Statement& s, int i) { return s.is_null(i) ? std::nullopt : std::optional(take(s.text(i))); }
std::string timestamp(std::chrono::system_clock::time_point now) {
  const auto t = std::chrono::system_clock::to_time_t(now); std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm,&t);
#else
  gmtime_r(&t,&tm);
#endif
  std::ostringstream out; out << std::put_time(&tm,"%Y-%m-%dT%H:%M:%SZ"); return out.str();
}
}
OperationLog::OperationLog(sqlite3* raw, bool ro, Clock clock)
    : raw_(raw), db_(Connection::borrow(raw)), read_only_(ro), clock_(clock ? std::move(clock) : Clock(std::chrono::system_clock::now)) {}
Result<std::int64_t> OperationLog::append(const LogEntry& entry) {
  return guard<std::int64_t>([&]() -> Result<std::int64_t> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_);
    if (read_only_ || (raw_ && sqlite3_db_readonly(raw_,"main") == 1)) return error(Errc::read_only,"am.catalog.read_only");
    if (entry.level != "info" && entry.level != "warn" && entry.level != "error") return error(Errc::invalid_argument,"am.jobs.log_invalid");
    if (entry.detail_json && !json::parse_strict(*entry.detail_json)) return error(Errc::invalid_argument,"am.jobs.json_invalid");
    auto s = take(db_.prepare("INSERT INTO op_log(at,level,job_id,asset_id,code,message,detail) VALUES(?,?,?,?,?,?,?) RETURNING seq"));
    take(s.bind(1,timestamp(clock_()))); take(s.bind(2,entry.level)); optional_bind(s,3,entry.job_id); optional_bind(s,4,entry.asset_id);
    take(s.bind(5,entry.code)); take(s.bind(6,entry.message)); optional_bind(s,7,entry.detail_json);
    take(s.step()); const auto seq = s.integer(0); take(s.step()); return seq;
  });
}
Result<std::vector<LogEntry>> OperationLog::query(const LogQuery& q) const {
  return guard<std::vector<LogEntry>>([&]() -> Result<std::vector<LogEntry>> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_);
    if (q.limit > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) return error(Errc::invalid_argument,"am.jobs.log_invalid");
    auto s = take(db_.prepare("SELECT seq,at,level,code,job_id,asset_id,message,detail FROM op_log WHERE (? IS NULL OR job_id=?) AND (? IS NULL OR asset_id=?) AND (? IS NULL OR seq>?) ORDER BY seq LIMIT ?"));
    optional_bind(s,1,q.job_id); optional_bind(s,2,q.job_id); optional_bind(s,3,q.asset_id); optional_bind(s,4,q.asset_id);
    for (int i : {5,6}) { if (q.after_seq) take(s.bind(i,*q.after_seq)); else take(s.bind_null(i)); }
    take(s.bind(7,static_cast<std::int64_t>(q.limit))); std::vector<LogEntry> out;
    while (take(s.step()) == Step::row) {
      LogEntry e; e.seq=s.integer(0); e.at=take(s.text(1)); e.level=take(s.text(2)); e.code=take(s.text(3));
      e.job_id=text(s,4); e.asset_id=text(s,5); e.message=take(s.text(6)); e.detail_json=text(s,7);
      if (e.detail_json && !json::parse_strict(*e.detail_json)) return error(Errc::corrupt_data,"am.jobs.json_invalid");
      out.push_back(std::move(e));
    }
    return out;
  });
}
Result<std::size_t> OperationLog::prune_before(std::string_view at) {
  return guard<std::size_t>([&]() -> Result<std::size_t> {
    const std::lock_guard lock(mutex_); const ConnectionLock connection_lock(raw_);
    if (read_only_ || (raw_ && sqlite3_db_readonly(raw_,"main") == 1)) return error(Errc::read_only,"am.catalog.read_only");
    auto s=take(db_.prepare("DELETE FROM op_log WHERE at<? RETURNING seq")); take(s.bind(1,at)); std::size_t n=0;
    while (take(s.step()) == Step::row) ++n;
    return n;
  });
}
} // namespace am::db
