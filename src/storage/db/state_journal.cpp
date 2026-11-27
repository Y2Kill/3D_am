#include "am/storage/db/commit_journal.hpp"
#include "am/storage/db/sqlite.hpp"
#include "am/foundation/paths.hpp"
#include <sqlite3.h>
#include "state_schema.hpp"
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>

namespace am::db {
namespace {
Error error(std::string detail, Errc code = Errc::io_error, std::string id = "am.commit.journal_error") {
  return Error::make(code, "Commit journal operation failed", std::move(detail)).in("storage.db").id(std::move(id));
}
[[noreturn]] void bad(sqlite3* db) {
  if (auto space = insufficient_space_error(db, sqlite3_extended_errcode(db))) throw *space;
  throw error(sqlite3_errmsg(db));
}
std::string utc_now() {
  const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out; out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ"); return out.str();
}
std::string utf8(const std::filesystem::path& p) { auto value = p.u8string(); return {reinterpret_cast<const char*>(value.data()), value.size()}; }
std::filesystem::path path(std::string_view text) {
  std::u8string value(text.size(), u8'\0'); if (!text.empty()) std::memcpy(value.data(), text.data(), text.size()); return std::filesystem::path(value);
}
struct JournalStatement {
  sqlite3* db; sqlite3_stmt* value = nullptr;
  JournalStatement(sqlite3* database, const char* sql) : db(database) { if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK) bad(db); }
  ~JournalStatement() { sqlite3_finalize(value); }
  void text(int i, std::string_view v) { if (sqlite3_bind_text64(value, i, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK) bad(db); }
  void number(int i, std::int64_t v) { if (sqlite3_bind_int64(value, i, v) != SQLITE_OK) bad(db); }
  void number(int i, std::uint64_t v) { if (v > static_cast<std::uint64_t>(INT64_MAX)) throw error("Value exceeds SQLite signed INTEGER", Errc::invalid_argument); number(i, static_cast<std::int64_t>(v)); }
  void done() { if (sqlite3_step(value) != SQLITE_DONE) bad(db); }
};
void exec(sqlite3* db, const char* sql) { if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) bad(db); }
struct JournalTransaction {
  sqlite3* db; bool committed = false;
  explicit JournalTransaction(sqlite3* value) : db(value) { exec(db, "BEGIN IMMEDIATE"); }
  ~JournalTransaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
  void finish() { exec(db, "COMMIT"); committed = true; }
};
std::string column(sqlite3_stmt* p, int i) {
  const auto* bytes = sqlite3_column_text(p, i); const auto size = sqlite3_column_bytes(p, i);
  return bytes ? std::string(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(size)) : std::string{};
}
CommitState decode_state(std::string_view s) {
  for (const auto v : {CommitState::prepared, CommitState::verified, CommitState::committed, CommitState::indexed,
                       CommitState::failed})
    if (state_name(v) == s) return v;
  throw error("Unsupported pending operation state", Errc::unsupported);
}
constexpr const char* select_rows = "SELECT p.op_id,p.asset_id,p.kind,p.target_path,p.temp_path,p.base_rev,p.base_size,p.base_mtime_ns,p.base_manifest_sha256,p.state,p.expected_rev,p.started_at,p.updated_at,p.job_id,n.new_size,n.new_file_sha256,n.new_manifest_sha256 FROM pending_ops p LEFT JOIN pending_ops_new n ON p.op_id=n.op_id";
PendingOperation decode(sqlite3_stmt* p) {
  PendingOperation r; r.op_id = column(p, 0); r.asset_id = column(p, 1); r.kind = column(p, 2); r.target_path = path(column(p, 3)); r.temp_path = path(column(p, 4));
  if (sqlite3_column_type(p, 6) != SQLITE_NULL) {
    if (sqlite3_column_int64(p, 6) < 0) throw error("Negative base size");
    r.base = BaseSnapshot{static_cast<std::uint64_t>(sqlite3_column_int64(p, 6)), sqlite3_column_int64(p, 7), column(p, 8), {}};
    if (sqlite3_column_type(p, 5) != SQLITE_NULL) r.base->revision = sqlite3_column_int64(p, 5);
  }
  r.state = decode_state(column(p, 9)); if (sqlite3_column_type(p, 10) != SQLITE_NULL) r.expected_rev = sqlite3_column_int64(p, 10);
  r.started_at = column(p, 11); r.updated_at = column(p, 12); if (sqlite3_column_type(p, 13) != SQLITE_NULL) r.job_id = column(p, 13);
  if (sqlite3_column_type(p, 14) != SQLITE_NULL) {
    if (sqlite3_column_int64(p, 14) < 0) throw error("Negative verified size");
    r.verified = NewFileEvidence{static_cast<std::uint64_t>(sqlite3_column_int64(p, 14)), column(p, 15), column(p, 16)};
  }
  return r;
}
}
std::string_view state_name(CommitState s) noexcept {
  switch (s) {
    case CommitState::prepared:
      return "prepared";
    case CommitState::verified:
      return "verified";
    case CommitState::committed:
      return "committed";
    case CommitState::indexed:
      return "indexed";
    case CommitState::failed:
      return "failed";
  }
  return "";
}
struct StateJournal::Impl {
  sqlite3* db = nullptr;
  std::mutex mutex;
  ~Impl() { if (db) sqlite3_close_v2(db); }
  template<class T, class F> Result<T> run(F&& f) {
    try { std::lock_guard lock(mutex); return f(); }
    catch (const Error& e) { return e; }
    catch (const std::bad_alloc&) { return error("Allocation failed", Errc::resource_exhausted, "am.commit.out_of_memory"); }
    catch (const std::exception& e) { return error(e.what()); }
  }
};
StateJournal::StateJournal(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
StateJournal::~StateJournal() = default;
Result<std::shared_ptr<StateJournal>> StateJournal::open(const std::filesystem::path& root) {
  try {
    const auto dir = paths::native(std::filesystem::absolute(root / ".am")); std::filesystem::create_directories(dir);
    const auto file = dir / "state.sqlite"; const bool existed = std::filesystem::exists(file);
    auto impl = std::make_unique<Impl>();
    if (sqlite3_open_v2(utf8(file).c_str(), &impl->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) bad(impl->db);
    if (!existed) exec(impl->db, state_schema_sql);
    {
      JournalStatement version(impl->db, "PRAGMA user_version"); if (sqlite3_step(version.value) != SQLITE_ROW) bad(impl->db);
      if (sqlite3_column_int(version.value, 0) != 1) return error("Unsupported state.sqlite user_version", Errc::unsupported, "am.commit.journal_version");
    }
    exec(impl->db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
    return std::shared_ptr<StateJournal>(new StateJournal(std::move(impl)));
  } catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return error("Allocation failed", Errc::resource_exhausted, "am.commit.out_of_memory"); }
  catch (const std::exception& e) { return error(e.what()); }
}
Result<void> StateJournal::insert(const PendingOperation& r) {
  return impl_->run<void>([&]() -> Result<void> {
    if (r.state != CommitState::prepared || r.verified) return error("Insert must be prepared without evidence", Errc::conflict);
    JournalTransaction tx(impl_->db);
    JournalStatement s(impl_->db, "INSERT INTO pending_ops(op_id,asset_id,kind,target_path,temp_path,backup_path,base_rev,base_size,base_mtime_ns,base_manifest_sha256,state,expected_rev,started_at,updated_at,job_id) VALUES(?,?,?,?,?,NULL,?,?,?,?,?,?,?,?,?)");
    s.text(1, r.op_id);
    s.text(2, r.asset_id);
    s.text(3, r.kind);
    s.text(4, utf8(r.target_path));
    if (r.kind != "purge") s.text(5, utf8(r.temp_path));
    if (r.base) { if (r.base->revision) s.number(6, *r.base->revision); s.number(7, r.base->size); s.number(8, r.base->mtime_ns); s.text(9, r.base->manifest_sha256); }
    s.text(10, "prepared"); if (r.expected_rev) s.number(11, *r.expected_rev); const auto now = utc_now();
    s.text(12, r.started_at.empty() ? now : r.started_at); s.text(13, r.updated_at.empty() ? now : r.updated_at); if (r.job_id) s.text(14, *r.job_id);
    s.done(); tx.finish(); return {};
  });
}
Result<void> StateJournal::advance(std::string_view id, CommitState from, CommitState to, const std::optional<NewFileEvidence>& evidence) {
  return impl_->run<void>([&]() -> Result<void> {
    JournalTransaction tx(impl_->db);
    bool location_transition = false;
    if ((from == CommitState::prepared && to == CommitState::committed) || to == CommitState::failed) {
      JournalStatement kind(impl_->db, "SELECT kind FROM pending_ops WHERE op_id=? AND state=?");
      kind.text(1, id);
      kind.text(2, state_name(from));
      if (sqlite3_step(kind.value) == SQLITE_ROW) {
        const auto name = column(kind.value, 0);
        location_transition = (name == "trash" || name == "restore" || name == "purge") && from != CommitState::failed;
      }
    }
    if (!((from == CommitState::prepared && to == CommitState::verified) ||
          (from == CommitState::verified && to == CommitState::committed) ||
          (from == CommitState::committed && to == CommitState::indexed) || location_transition) ||
        ((to == CommitState::verified) != evidence.has_value()))
      return error("Invalid journal transition/evidence", Errc::conflict);
    JournalStatement s(impl_->db, "UPDATE pending_ops SET state=?,updated_at=? WHERE op_id=? AND state=?");
    s.text(1, state_name(to));
    s.text(2, utc_now());
    s.text(3, id);
    s.text(4, state_name(from));
    s.done();
    if (sqlite3_changes(impl_->db) != 1) return error("Journal compare-and-advance conflict", Errc::conflict);
    if (evidence) {
      JournalStatement n(impl_->db, "INSERT INTO pending_ops_new(op_id,new_size,new_file_sha256,new_manifest_sha256) VALUES(?,?,?,?)");
      n.text(1, id);
      n.number(2, evidence->size);
      n.text(3, evidence->file_sha256);
      n.text(4, evidence->manifest_sha256);
      n.done();
    }
    tx.finish();
    return {};
  });
}
Result<void> StateJournal::remove(std::string_view id) {
  return impl_->run<void>([&]() -> Result<void> { JournalTransaction tx(impl_->db); JournalStatement s(impl_->db, "DELETE FROM pending_ops WHERE op_id=?"); s.text(1, id); s.done(); tx.finish(); return {}; });
}
Result<std::vector<PendingOperation>> StateJournal::list() {
  return impl_->run<std::vector<PendingOperation>>([&]() -> Result<std::vector<PendingOperation>> {
    JournalStatement s(impl_->db, (std::string(select_rows) + " ORDER BY p.op_id").c_str()); std::vector<PendingOperation> rows;
    int status; while ((status = sqlite3_step(s.value)) == SQLITE_ROW) rows.push_back(decode(s.value)); if (status != SQLITE_DONE) bad(impl_->db); return rows;
  });
}
Result<std::optional<PendingOperation>> StateJournal::find(std::string_view id) {
  return impl_->run<std::optional<PendingOperation>>([&]() -> Result<std::optional<PendingOperation>> {
    JournalStatement s(impl_->db, (std::string(select_rows) + " WHERE p.op_id=?").c_str()); s.text(1, id); const auto status = sqlite3_step(s.value);
    if (status == SQLITE_DONE) return std::optional<PendingOperation>{}; if (status != SQLITE_ROW) bad(impl_->db); return std::optional<PendingOperation>{decode(s.value)};
  });
}
}  // namespace am::db
