#include "am/storage/library/library.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "state_schema.hpp"
#include "index_schema.hpp"
#include "userdata_schema.hpp"
#include <sqlite3.h>
#include <toml++/toml.hpp>
#include <array>
#include <algorithm>
#include <climits>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace am::library {
namespace {
namespace fs = std::filesystem;
using Path = fs::path;
std::string u8(const Path& p) { auto s = p.u8string(); return {s.begin(), s.end()}; }
std::string read(const Path& p) {
  std::ifstream in(paths::native(p), std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
Error error(Errc c, std::string id, std::string detail = {}) {
  return Error::make(c, "Library operation failed", std::move(detail)).in("storage.library").id(std::move(id));
}
[[noreturn]] void fail(Errc c, const char* id, std::string detail = {}) { throw error(c, id, std::move(detail)); }
template<class T> T take(Result<T> r) { if (!r) throw r.error(); return std::move(r).value(); }
void take(Result<void> r) { if (!r) throw r.error(); }
template<class T, class F> Result<T> guard(F&& f) {
  try { return f(); }
  catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return error(Errc::resource_exhausted, "am.library.out_of_memory"); }
  catch (const std::exception& e) { return error(Errc::io_error, "am.library.io_error", e.what()); }
  catch (...) { return error(Errc::internal, "am.library.internal"); }
}
std::string utc() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
  const auto time = std::chrono::system_clock::to_time_t(seconds);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
  return out.str();
}
std::string suffix() {
  auto text = utc();
  for (auto& c : text) if (c == ':') c = '-';
  return text + "-" + Uuid::v7().to_string();
}
bool valid_time(const std::string& s) {
  if (!std::regex_match(s, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)"))) return false;
  using namespace std::chrono;
  const year_month_day date{year{std::stoi(s.substr(0, 4))}, month{static_cast<unsigned>(std::stoi(s.substr(5, 2)))},
                            day{static_cast<unsigned>(std::stoi(s.substr(8, 2)))}};
  return date.ok() && std::stoi(s.substr(11, 2)) < 24 && std::stoi(s.substr(14, 2)) < 60 && std::stoi(s.substr(17, 2)) < 60;
}
Config load_config(const Path& root) {
  const auto path = root / "am-library.toml";
  if (!fs::exists(paths::native(path))) fail(Errc::not_found, "am.library.not_found", u8(path));
  toml::table table;
  try { table = toml::parse(read(path)); }
  catch (const toml::parse_error& e) { fail(Errc::corrupt_data, "am.library.config_invalid", std::string(e.description())); }
  auto string = [&](const char* key) {
    auto v = table[key].value<std::string>();
    if (!v) fail(Errc::corrupt_data, "am.library.config_invalid", key);
    return *v;
  };
  if (string("format") != "am-library") fail(Errc::corrupt_data, "am.library.config_invalid", "format");
  auto version = table["schema_version"].value<std::int64_t>();
  if (!table["schema_version"].is_integer() || !version || *version < 1) fail(Errc::corrupt_data, "am.library.config_invalid", "schema_version");
  if (*version > 1) fail(Errc::unsupported, "am.library.config_newer", "schema_version=" + std::to_string(*version));
  Config c;
  c.library_id = string("library_id"); c.name = string("name"); c.created_at = string("created_at");
  const auto id = Uuid::parse_canonical(c.library_id);
  if (!id || id->version() != 7 || (id->bytes()[8] & 0xc0) != 0x80)
    fail(Errc::corrupt_data, "am.library.config_invalid", "library_id");
  if (!valid_time(c.created_at)) fail(Errc::corrupt_data, "am.library.config_invalid", "created_at");
  return c;
}
bool directory_access(const Path& p, bool write) {
#ifdef _WIN32
  const auto access = write ? (FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY) : FILE_LIST_DIRECTORY;
  HANDLE handle = CreateFileW(paths::native(p).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  CloseHandle(handle); return true;
#else
  return ::access(p.c_str(), (write ? W_OK : R_OK) | X_OK) == 0;
#endif
}
StorageCapabilities detect_capabilities(const Path& root, const OpenOptions& options) {
  if (options.capabilities) return *options.capabilities;
  StorageCapabilities c;
  static_cast<repository::StorageCapabilities&>(c) = take(repository::detect_storage_capabilities(root));
  c.readable = directory_access(root / ".am", false);
  c.writable = directory_access(root / ".am", true);
  return c;
}
class OwnerLock {
 public:
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  OVERLAPPED overlap{};
  OwnerLock() { overlap.OffsetHigh = 0x40000000; }
  ~OwnerLock() {
    if (handle != INVALID_HANDLE_VALUE) { if (held) UnlockFileEx(handle, 0, 1, 0, &overlap); CloseHandle(handle); }
  }
#else
  int handle = -1;
  ~OwnerLock() { if (handle >= 0) { if (held) flock(handle, LOCK_UN); ::close(handle); } }
#endif
  bool held = false;
  bool open(const Path& p, bool writer) {
#ifdef _WIN32
    handle = CreateFileW(paths::native(p).c_str(), writer ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, writer ? OPEN_ALWAYS : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle != INVALID_HANDLE_VALUE;
#else
    handle = ::open(p.c_str(), writer ? O_RDWR | O_CREAT | O_CLOEXEC : O_RDONLY | O_CLOEXEC, 0600);
    return handle >= 0;
#endif
  }
  bool acquire() {
#ifdef _WIN32
    held = LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap) != 0;
#else
    held = flock(handle, LOCK_EX | LOCK_NB) == 0;
#endif
    return held;
  }
  void write(const std::string& text) {
#ifdef _WIN32
    LARGE_INTEGER zero{};
    DWORD written = 0;
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(handle) ||
        !WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size() ||
        !FlushFileBuffers(handle)) fail(Errc::io_error, "am.library.io_error", "library.lock diagnostic write");
#else
    if (ftruncate(handle, 0) != 0 || lseek(handle, 0, SEEK_SET) < 0)
      fail(Errc::io_error, "am.library.io_error", "library.lock diagnostic truncate");
    std::size_t offset = 0;
    while (offset < text.size()) {
      const auto n = ::write(handle, text.data() + offset, text.size() - offset);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) fail(Errc::io_error, "am.library.io_error", "library.lock diagnostic write");
      offset += static_cast<std::size_t>(n);
    }
    if (fsync(handle) != 0) fail(Errc::io_error, "am.library.io_error", "library.lock diagnostic flush");
#endif
  }
};
std::string diagnostic(const std::string& version) {
  std::array<char, 256> host{};
#ifdef _WIN32
  std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> name{};
  DWORD length = static_cast<DWORD>(name.size());
  if (GetComputerNameW(name.data(), &length))
    WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(length), host.data(), static_cast<int>(host.size()) - 1, nullptr, nullptr);
  const auto pid = GetCurrentProcessId();
#else
  (void)gethostname(host.data(), host.size() - 1);
  const auto pid = getpid();
#endif
  return repository::Json{{"pid", pid}, {"host", host.data()}, {"application_version", version}, {"acquired_at", utc()}}.dump() + '\n';
}
struct Close { void operator()(sqlite3* p) const noexcept { if (p) sqlite3_close_v2(p); } };
using Connection = std::unique_ptr<sqlite3, Close>;
struct Database { Connection handle; DatabaseInfo info; };
const char* name(DatabaseKind k) {
  switch (k) { case DatabaseKind::index: return "index"; case DatabaseKind::userdata: return "userdata"; default: return "state"; }
}
const char* schema(DatabaseKind k) {
  switch (k) { case DatabaseKind::index: return index_schema_sql; case DatabaseKind::userdata: return userdata_schema_sql; default: return state_schema_sql; }
}
void exec(sqlite3* db, const std::string& sql) {
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
    fail(Errc::io_error, "am.library.db_error", sqlite3_errmsg(db));
}
std::string scalar(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    fail(Errc::io_error, "am.library.db_error", sqlite3_errmsg(db));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
  const auto rc = sqlite3_step(raw);
  if (rc == SQLITE_DONE) return {};
  if (rc != SQLITE_ROW) fail(Errc::io_error, "am.library.db_error", sqlite3_errmsg(db));
  const auto* text = sqlite3_column_text(raw, 0);
  return text ? reinterpret_cast<const char*>(text) : "";
}
Connection connect(const Path& path, int flags, bool immutable = false) {
  auto filename = u8(paths::native(path));
  if (immutable) {
    // Percent-encode UTF-8 bytes so path characters cannot become URI options.
    filename = "file:";
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : u8(path.generic_u8string())) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == ':') filename += static_cast<char>(c);
      else { filename += '%'; filename += hex[c >> 4]; filename += hex[c & 15]; }
    }
    filename += "?mode=ro&immutable=1";
    flags |= SQLITE_OPEN_URI;
  }
  sqlite3* raw = nullptr;
  const auto rc = sqlite3_open_v2(filename.c_str(), &raw, flags | SQLITE_OPEN_FULLMUTEX, nullptr);
  Connection connection(raw);
  if (rc != SQLITE_OK) fail(Errc::io_error, "am.library.db_error", raw ? sqlite3_errmsg(raw) : "sqlite3_open_v2 failed");
  return connection;
}
std::string quote(const std::string& value) {
  std::string out = "'";
  for (auto c : value) { out += c; if (c == '\'') out += c; }
  return out + "'";
}
bool schema_present(sqlite3* db, DatabaseKind kind) {
  const char* tables = kind == DatabaseKind::index ? "'assets','meta','fts_assets','fts_names','fts_docs','fts_names_owner','fts_docs_owner'" :
                       kind == DatabaseKind::state ? "'pending_ops','pending_ops_new','snapshots','jobs','op_log','edit_sessions','edit_session_files'" :
                       "'relations','collections','collection_items','saved_searches','tag_dict','tag_aliases','tag_relations','category_tree','field_defs','suggestion_decisions','dedup_reviews'";
  const auto expected = kind == DatabaseKind::userdata ? "11" : "7";
  return scalar(db, "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name IN (" + std::string(tables) + ")") == expected;
}
int supported(DatabaseKind kind, const OpenOptions& options) {
  int target = kind == DatabaseKind::index ? 2 : 1;
  std::map<int, const Migration*> chain;
  for (const auto& step : options.migrations) if (step.database == kind) {
    if (kind == DatabaseKind::index || step.from < 1 || step.from == INT_MAX || !step.apply || !chain.emplace(step.from, &step).second)
      fail(Errc::invalid_argument, "am.library.migration_invalid", name(kind));
    target = std::max(target, step.from + 1);
  }
  for (int v = 1; kind != DatabaseKind::index && v < target; ++v) if (!chain.contains(v))
    fail(Errc::invalid_argument, "am.library.migration_invalid", name(kind));
  return target;
}
void migrate(sqlite3* db, DatabaseKind kind, int version, int target, const Path& root, const OpenOptions& options) {
  fs::create_directories(paths::native(root / ".am/backup"));
  const auto backup = root / ".am/backup" / (std::string(name(kind)) + "-v" + std::to_string(version) + "-" + suffix() + ".sqlite");
  exec(db, "VACUUM INTO " + quote(u8(paths::native(backup))));
  exec(db, "BEGIN IMMEDIATE");
  try {
    for (int v = version; v < target; ++v) {
      const Migration* step = nullptr;
      for (const auto& s : options.migrations) if (s.database == kind && s.from == v) step = &s;
      if (!step) fail(Errc::unsupported, "am.library.db_version", name(kind));
      take(step->apply(db));
      exec(db, "PRAGMA user_version=" + std::to_string(v + 1));
      if (scalar(db, "PRAGMA integrity_check") != "ok" || std::stoi(scalar(db, "PRAGMA user_version")) != v + 1)
        fail(Errc::corrupt_data, "am.library.migration_failed", name(kind));
    }
    exec(db, "COMMIT");
  } catch (...) { (void)sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}
void stale(const Path& path) {
  const auto tail = ".stale-" + suffix();
  const auto target = Path(path.native() + Path(tail).native());
  // Connections have been closed before preserving the whole WAL file set.
  for (const auto* extension : {"-wal", "-shm", ""}) {
    const auto source = Path(path.native() + Path(extension).native());
    if (fs::exists(paths::native(source))) fs::rename(paths::native(source), paths::native(Path(target.native() + Path(extension).native())));
  }
}
Database open_database(const Path& root, DatabaseKind kind, const OpenOptions& options, const StorageCapabilities& caps) {
  Database out;
  const auto path = root / ".am" / (std::string(name(kind)) + ".sqlite");
  const bool exists = fs::exists(paths::native(path));
  const bool rw = options.mode == Mode::read_write;
  if (!exists && !rw) return out;
  const int target = supported(kind, options);
  bool immutable = !rw && !caps.writable;
  if (immutable) {
    const auto wal = Path(path.native() + Path("-wal").native());
    if (fs::exists(paths::native(wal)) && fs::file_size(paths::native(wal)) != 0) {
      out.info.state = DatabaseState::unavailable; out.info.reason = "wal_pending"; return out;
    }
  }
  try {
    // Inspect the index read-only before deciding whether it is disposable.
    out.handle = connect(path, rw && (kind != DatabaseKind::index || !exists) ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY, immutable);
  } catch (const Error& e) {
    if (rw && kind != DatabaseKind::index) throw;
    out.info.state = DatabaseState::unavailable; out.info.reason = "open_failed"; out.info.detail = e.ctx.detail;
  }
  bool compatible = false;
  if (out.handle) {
    try {
      const int version = std::stoi(scalar(out.handle.get(), "PRAGMA user_version"));
      out.info.user_version = version;
      // Read the schema B-tree only; data pages are checked explicitly by verify/recovery.
      const bool intact = version == 0 || schema_present(out.handle.get(), kind);
      compatible = intact && version == target;
      if (compatible && kind == DatabaseKind::index)
        out.info.rebuild_required = scalar(out.handle.get(), "SELECT value FROM meta WHERE key='rebuild_required'") == "1";
      if (compatible && kind == DatabaseKind::index && rw &&
          scalar(out.handle.get(), "SELECT value FROM meta WHERE key='database_check_failed'") == "1") compatible = false;
      if (rw && kind != DatabaseKind::index) {
        if (!intact) fail(Errc::corrupt_data, "am.library.db_error", std::string(name(kind)) + ": required schema missing");
        if (version == 0 && scalar(out.handle.get(), "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'") == "0") {
          exec(out.handle.get(), schema(kind)); compatible = true;
          if (target > 1) migrate(out.handle.get(), kind, 1, target, root, options);
        } else if (version >= 1 && version < target) {
          migrate(out.handle.get(), kind, version, target, root, options); compatible = true;
        } else if (version != target) {
          auto e = error(Errc::unsupported, "am.library.db_version", std::string(name(kind)) + ": found=" + std::to_string(version) + ", supported=" + std::to_string(target));
          if (version > target) e.actions.push_back(ErrorAction::open_read_only);
          throw e;
        }
      }
      out.info.state = compatible ? DatabaseState::ok : intact && version > target && kind != DatabaseKind::index ? DatabaseState::newer : DatabaseState::incompatible;
    } catch (const Error& e) {
      if (rw && kind != DatabaseKind::index) throw;
      compatible = false;
      const auto code = sqlite3_errcode(out.handle.get());
      const bool damaged = code == SQLITE_CORRUPT || code == SQLITE_NOTADB || code == SQLITE_ERROR;
      out.info.state = damaged ? DatabaseState::incompatible : DatabaseState::unavailable;
      if (!damaged) out.info.reason = "open_failed";
      out.info.detail = e.ctx.detail;
    }
  }
  if (rw && kind == DatabaseKind::index) {
    if (out.info.state == DatabaseState::unavailable)
      fail(Errc::io_error, "am.library.db_error", out.info.detail);
    out.handle.reset();
    if (!compatible) {
      if (exists) { stale(path); out.info.reason = "recreated"; }
      out.handle = connect(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
      exec(out.handle.get(), schema(kind));
      exec(out.handle.get(), "INSERT INTO meta(key,value) VALUES('rebuild_required','1')");
    } else out.handle = connect(path, SQLITE_OPEN_READWRITE);
    out.info.state = DatabaseState::ok;
  }
  if (rw && out.handle) {
    exec(out.handle.get(), "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;");
    exec(out.handle.get(), kind == DatabaseKind::index ? "PRAGMA synchronous=NORMAL" : "PRAGMA synchronous=FULL");
    out.info.user_version = std::stoi(scalar(out.handle.get(), "PRAGMA user_version"));
  }
  if (out.handle) {
    exec(out.handle.get(), "PRAGMA foreign_keys=ON");
    out.info.read_mode = immutable ? "immutable" : "wal";
    if (kind == DatabaseKind::index && out.info.state == DatabaseState::ok)
      out.info.rebuild_required = scalar(out.handle.get(), "SELECT value FROM meta WHERE key='rebuild_required'") == "1";
  }
  return out;
}
}  // namespace

struct Library::Impl {
  // Destruction order keeps the owner lock until all connections are closed.
  std::unique_ptr<OwnerLock> lock;
  Config config;
  Path root;
  StorageCapabilities caps;
  Mode mode = Mode::read_only;
  OwnerInfo owner;
  std::array<Database, 3> databases;
  std::optional<repository::AssetRepository> assets;
  std::function<void(DatabaseKind, DatabaseCheckLevel)> on_database_check;
};
Library::Library(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
Library::Library(Library&&) noexcept = default;
Library& Library::operator=(Library&&) noexcept = default;
Library::~Library() = default;
const Config& Library::config() const noexcept { return impl_->config; }
const std::filesystem::path& Library::root() const noexcept { return impl_->root; }
const StorageCapabilities& Library::capabilities() const noexcept { return impl_->caps; }
Mode Library::mode() const noexcept { return impl_->mode; }
const OwnerInfo& Library::owner() const noexcept { return impl_->owner; }
const DatabaseInfo& Library::database_info(DatabaseKind k) const noexcept { return impl_->databases[static_cast<std::size_t>(k)].info; }
sqlite3* Library::database(DatabaseKind k) const noexcept { return impl_->databases[static_cast<std::size_t>(k)].handle.get(); }
repository::AssetRepository& Library::assets() noexcept { return *impl_->assets; }
Result<DatabaseCheck> Library::check_database(DatabaseKind kind, DatabaseCheckLevel level) {
  return guard<DatabaseCheck>([&]() -> Result<DatabaseCheck> {
    DatabaseCheck out; out.database = kind; out.level = level;
    auto* db = database(kind);
    if (!db) {
      out.status = "unavailable"; out.detail = database_info(kind).detail;
      out.message_id = "am.library.db_error"; return out;
    }
    if (impl_->on_database_check) impl_->on_database_check(kind, level);
    sqlite3_stmt* raw = nullptr;
    const char* sql = level == DatabaseCheckLevel::full ? "PRAGMA integrity_check" : "PRAGMA quick_check";
    int rc = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    bool ok = true, saw_row = false;
    if (rc == SQLITE_OK) {
      while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
        saw_row = true;
        const auto* text = sqlite3_column_text(raw, 0);
        const std::string finding = text ? reinterpret_cast<const char*>(text) : "";
        if (finding != "ok") { ok = false; if (!out.detail.empty()) out.detail += "\n"; out.detail += finding; }
      }
    }
    if (rc != SQLITE_DONE) { ok = false; out.detail = sqlite3_errmsg(db); }
    out.status = ok && saw_row ? "ok" : "corrupt";
    if (out.status != "ok") {
      out.message_id = "am.library.db_error";
      if (kind == DatabaseKind::index && mode() == Mode::read_write) {
        // Preserve the finding even if damage prevents persisting the disposable-index flag.
        impl_->databases[static_cast<std::size_t>(kind)].info.rebuild_required = true;
        (void)sqlite3_exec(db, "INSERT INTO meta(key,value) VALUES('rebuild_required','1') "
                             "ON CONFLICT(key) DO UPDATE SET value='1'; "
                             "INSERT INTO meta(key,value) VALUES('database_check_failed','1') "
                             "ON CONFLICT(key) DO UPDATE SET value='1'", nullptr, nullptr, nullptr);
      }
    }
    return out;
  });
}
Result<void> Library::reset_index() {
  return guard<void>([&]() -> Result<void> {
    if (mode() != Mode::read_write) return error(Errc::read_only, "am.library.storage_read_only");
    auto& index = impl_->databases[static_cast<std::size_t>(DatabaseKind::index)];
    index.handle.reset();
    stale(impl_->root / ".am/index.sqlite");
    OpenOptions options; options.mode = Mode::read_write;
    index = open_database(impl_->root, DatabaseKind::index, options, impl_->caps);
    index.info.reason = "recreated";
    return {};
  });
}
Result<Config> Library::init(const Path& path, const InitOptions& options) {
  return guard<Config>([&] {
    const auto root = fs::absolute(path).lexically_normal();
    if (fs::exists(paths::native(root))) {
      if (!fs::is_directory(paths::native(root)) || !fs::is_empty(paths::native(root))) fail(Errc::conflict, "am.library.not_empty", u8(root));
    } else {
      if (!fs::is_directory(paths::native(root.parent_path()))) fail(Errc::not_found, "am.library.parent_missing", u8(root.parent_path()));
      fs::create_directory(paths::native(root));
    }
    for (const auto* dir : {"assets", "trash", ".am/cache", ".am/sessions", ".am/.am-tmp", ".am/locks"})
      fs::create_directories(paths::native(root / dir));
    for (const auto kind : {DatabaseKind::index, DatabaseKind::userdata, DatabaseKind::state}) {
      auto db = connect(root / ".am" / (std::string(name(kind)) + ".sqlite"), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
      exec(db.get(), schema(kind));
      if (kind == DatabaseKind::index) exec(db.get(), "INSERT INTO meta(key,value) VALUES('rebuild_required','0'); PRAGMA synchronous=NORMAL");
    }
    Config c; c.library_id = Uuid::v7().to_string(); c.name = options.name; c.created_at = utc();
    if (options.before_config) take(options.before_config());
    const auto temporary = root / ".am/.am-tmp" / (Uuid::v7().to_string() + ".toml");
    {
      std::ofstream file(paths::native(temporary), std::ios::binary);
      file << toml::table{{"format", "am-library"}, {"schema_version", 1}, {"library_id", c.library_id}, {"name", c.name}, {"created_at", c.created_at}};
      file.close();
      if (!file) fail(Errc::io_error, "am.library.io_error", u8(temporary));
    }
    fs::rename(paths::native(temporary), paths::native(root / "am-library.toml"));
    return c;
  });
}
Result<Library> Library::open(const Path& path, const OpenOptions& options) {
  return guard<Library>([&] {
    const auto root = fs::weakly_canonical(fs::absolute(path));
    auto p = std::make_unique<Impl>(); p->config = load_config(root); p->mode = options.mode;
    p->caps = detect_capabilities(root, options);
    const auto lock_path = root / ".am/library.lock";
    if (options.mode == Mode::read_write) {
      if (p->caps.mode == repository::StorageMode::read_only_default)
        throw error(Errc::read_only, "am.library.storage_read_only", p->caps.filesystem).allow(ErrorAction::open_read_only);
      if (!p->caps.writable) throw error(Errc::read_only, "am.library.not_writable").allow(ErrorAction::open_read_only);
      p->lock = std::make_unique<OwnerLock>();
      if (!p->lock->open(lock_path, true)) throw error(Errc::read_only, "am.library.not_writable", u8(lock_path)).allow(ErrorAction::open_read_only);
      if (!p->lock->acquire()) throw error(Errc::locked, "am.library.locked", read(lock_path)).allow(ErrorAction::open_read_only);
      p->lock->write(diagnostic(options.application_version)); p->owner.locked = true;
    } else if (fs::exists(paths::native(lock_path))) {
      OwnerLock probe;
      if (!probe.open(lock_path, false) || !probe.acquire()) { p->owner.locked = true; p->owner.detail = read(lock_path); }
    }
    for (const auto kind : {DatabaseKind::userdata, DatabaseKind::state, DatabaseKind::index})
      p->databases[static_cast<std::size_t>(kind)] = open_database(root, kind, options, p->caps);
    repository::RepositoryOptions repo;
    repo.library_id = p->config.library_id; repo.mode = p->mode; repo.application_version = options.application_version;
    repo.capabilities = static_cast<const repository::StorageCapabilities&>(p->caps);
    repo.on_indexed = options.on_indexed;
    p->assets.emplace(take(repository::AssetRepository::open(root, std::move(repo))));
    p->root = root;
    p->on_database_check = options.on_database_check;
    return Library(std::move(p));
  });
}
}  // namespace am::library
