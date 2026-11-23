#include "am/foundation/log.hpp"
#include "am/storage/db/sqlite.hpp"

#include <sqlite3.h>

#include <climits>
#include <exception>

#include "am/foundation/paths.hpp"

namespace am::db {
namespace {
Error error(sqlite3* db, std::string id, int code = SQLITE_ERROR) {
  const auto primary = code & 0xff;
  const auto kind = primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB ? Errc::corrupt_data
                    : code == SQLITE_READONLY                        ? Errc::read_only
                    : code == SQLITE_BUSY || code == SQLITE_LOCKED ? Errc::locked
                                                                   : Errc::io_error;
  const char* database="unknown";
  if(db)if(const char* path=sqlite3_db_filename(db,"main")){const auto text=std::string_view(path);const auto slash=text.find_last_of("/\\");const auto file=slash==std::string_view::npos?text:text.substr(slash+1);if(file=="state.sqlite")database="state";else if(file=="userdata.sqlite")database="userdata";else if(file=="index.sqlite")database="index";}
  auto result=Error::make(kind, "SQLite operation failed", db ? sqlite3_errmsg(db) : "No database connection")
      .in("storage.db")
      .id(std::move(id));
  if(kind==Errc::corrupt_data)AM_LOG(error,"storage.db","db.corrupt","Database corruption",{{"database",std::string(database)}},&result);
  return result;
}
template <class T, class F>
Result<T> guard(F&& f) {
  try {
    return f();
  } catch (const std::bad_alloc&) {
    return Error::make(Errc::resource_exhausted, "SQLite allocation failed").id("am.db.out_of_memory");
  } catch (const std::exception& e) {
    return Error::make(Errc::io_error, "SQLite operation failed", e.what()).id("am.db.error");
  } catch (...) {
    return Error::make(Errc::internal, "SQLite operation failed").id("am.db.error");
  }
}
}  // namespace
Statement::Statement(sqlite3_stmt* s) noexcept : stmt_(s) {}
Statement::Statement(Statement&& s) noexcept : stmt_(std::exchange(s.stmt_, nullptr)) {}
Statement& Statement::operator=(Statement&& s) noexcept {
  if (this != &s) {
    sqlite3_finalize(stmt_);
    stmt_ = std::exchange(s.stmt_, nullptr);
  }
  return *this;
}
Statement::~Statement() { sqlite3_finalize(stmt_); }
Result<void> Statement::bind(int i, std::string_view s) {
  return guard<void>([&]() -> Result<void> {
    const int rc = sqlite3_bind_text64(stmt_, i, s.empty() ? "" : s.data(), s.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    if (rc != SQLITE_OK) return error(sqlite3_db_handle(stmt_), "am.db.bind", rc);
    return {};
  });
}
Result<void> Statement::bind(int i, std::int64_t v) {
  return guard<void>([&]() -> Result<void> {
    const int rc = sqlite3_bind_int64(stmt_, i, v);
    if (rc != SQLITE_OK) return error(sqlite3_db_handle(stmt_), "am.db.bind", rc);
    return {};
  });
}
Result<void> Statement::bind(int i, double v) {
  return guard<void>([&]() -> Result<void> {
    const int rc = sqlite3_bind_double(stmt_, i, v);
    if (rc != SQLITE_OK) return error(sqlite3_db_handle(stmt_), "am.db.bind", rc);
    return {};
  });
}
Result<void> Statement::bind_null(int i) {
  return guard<void>([&]() -> Result<void> {
    const int rc = sqlite3_bind_null(stmt_, i);
    if (rc != SQLITE_OK) return error(sqlite3_db_handle(stmt_), "am.db.bind", rc);
    return {};
  });
}
Result<Step> Statement::step() {
  return guard<Step>([&]() -> Result<Step> {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return Step::row;
    if (rc == SQLITE_DONE) return Step::done;
    return error(sqlite3_db_handle(stmt_), "am.db.step", rc);
  });
}
bool Statement::is_null(int c) const noexcept { return sqlite3_column_type(stmt_, c) == SQLITE_NULL; }
Result<std::string> Statement::text(int c) const {
  return guard<std::string>([&]() -> Result<std::string> {
    const auto* s = sqlite3_column_text(stmt_, c);
    const auto n = sqlite3_column_bytes(stmt_, c);
    if (!s && !is_null(c)) return error(sqlite3_db_handle(stmt_), "am.db.error", SQLITE_NOMEM);
    return s ? std::string(reinterpret_cast<const char*>(s), static_cast<std::size_t>(n)) : std::string{};
  });
}
std::int64_t Statement::integer(int c) const noexcept { return sqlite3_column_int64(stmt_, c); }
double Statement::real(int c) const noexcept { return sqlite3_column_double(stmt_, c); }
Connection::Connection(sqlite3* p, bool owned) noexcept : db_(p), owned_(owned) {}
Connection Connection::borrow(sqlite3* p) noexcept { return Connection(p, false); }
Connection::Connection(Connection&& c) noexcept
    : db_(std::exchange(c.db_, nullptr)), owned_(std::exchange(c.owned_, false)) {}
Connection& Connection::operator=(Connection&& c) noexcept {
  if (this != &c) {
    if (owned_) sqlite3_close_v2(db_);
    db_ = std::exchange(c.db_, nullptr);
    owned_ = std::exchange(c.owned_, false);
  }
  return *this;
}
Connection::~Connection() {
  if (owned_) sqlite3_close_v2(db_);
}
Result<Connection> Connection::open(const std::filesystem::path& p, bool ro) {
  return guard<Connection>([&]() -> Result<Connection> {
    const auto name = paths::native(p).u8string();
    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(
        reinterpret_cast<const char*>(name.c_str()), &raw,
        (ro ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) | SQLITE_OPEN_FULLMUTEX, nullptr);
    Connection c(raw, true);
    if (rc != SQLITE_OK) return error(raw, "am.db.open", rc);
    return c;
  });
}
Result<Statement> Connection::prepare(std::string_view sql) const {
  return guard<Statement>([&]() -> Result<Statement> {
    if (!db_ || sql.size() > INT_MAX) return error(db_, "am.db.prepare");
    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &raw, nullptr);
    Statement s(raw);
    if (rc != SQLITE_OK || !raw) return error(db_, "am.db.prepare", rc);
    return s;
  });
}
Result<void> Connection::exec(std::string_view sql) const {
  return guard<void>([&]() -> Result<void> {
    if (!db_) return error(db_, "am.db.exec");
    const std::string text(sql);
    const int rc = sqlite3_exec(db_, text.c_str(), nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) return error(db_, "am.db.exec", rc);
    return {};
  });
}
Transaction::Transaction(Connection& c) noexcept : db_(&c) {}
Result<Transaction> Transaction::begin(Connection& c) {
  auto r = c.exec("BEGIN IMMEDIATE");
  if (!r) return r.error();
  return Transaction(c);
}
Transaction::Transaction(Transaction&& t) noexcept : db_(std::exchange(t.db_, nullptr)) {}
Transaction& Transaction::operator=(Transaction&& t) noexcept {
  if (this != &t) {
    if (db_) (void)db_->exec("ROLLBACK");
    db_ = std::exchange(t.db_, nullptr);
  }
  return *this;
}
Transaction::~Transaction() {
  if (db_) (void)db_->exec("ROLLBACK");
}
Result<void> Transaction::commit() {
  if (!db_) return Error::make(Errc::invalid_argument, "Transaction is not active").id("am.db.transaction");
  auto r = db_->exec("COMMIT");
  if (r) db_ = nullptr;
  return r;
}
}  // namespace am::db
