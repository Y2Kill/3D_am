#pragma once
#include "am/storage/repository/repository.hpp"

struct sqlite3;
namespace am::library {
using Mode = repository::LibraryMode;
// Library access checks supplement the existing filesystem classification.
// The base capabilities are passed unchanged to AssetRepository.
struct StorageCapabilities : repository::StorageCapabilities {
  bool readable = true;
  bool writable = true;
};
struct Config {
  std::string library_id, name, created_at;
  int schema_version = 1;
};
enum class DatabaseKind { index, userdata, state };
enum class DatabaseState { ok, missing, incompatible, newer, unavailable };
enum class DatabaseCheckLevel { quick, full };
struct DatabaseCheck {
  DatabaseKind database = DatabaseKind::index;
  DatabaseCheckLevel level = DatabaseCheckLevel::quick;
  std::string status, detail, message_id;
};
struct DatabaseInfo {
  DatabaseState state = DatabaseState::missing;
  std::optional<int> user_version;
  std::string reason, detail, read_mode;
  std::optional<bool> rebuild_required;
};
struct OwnerInfo { bool locked = false; std::string detail; };
// A test migration substitutes the supported version for one database. There
// are no production steps yet. Callbacks run inside one transaction.
struct Migration {
  DatabaseKind database = DatabaseKind::userdata;
  int from = 1;
  std::function<Result<void>(sqlite3*)> apply;
};
struct OpenOptions {
  Mode mode = Mode::read_write;
  std::string application_version;
  std::optional<StorageCapabilities> capabilities;
  std::vector<Migration> migrations;
  std::function<void(const repository::IndexedEvent&)> on_indexed;
  int max_attempts = 3; // hypothesis; job recovery policy, refined by measurements
  // Per-instance observer for deterministic check-count tests.
  std::function<void(DatabaseKind, DatabaseCheckLevel)> on_database_check;
};
struct InitOptions {
  std::string name;
  std::function<Result<void>()> before_config;
};
class Library {
 public:
  static Result<Config> init(const std::filesystem::path&, const InitOptions& = {});
  static Result<Library> open(const std::filesystem::path&, const OpenOptions& = {});
  Library(Library&&) noexcept;
  Library& operator=(Library&&) noexcept;
  ~Library();
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  const Config& config() const noexcept;
  const std::filesystem::path& root() const noexcept;
  const StorageCapabilities& capabilities() const noexcept;
  Mode mode() const noexcept;
  const OwnerInfo& owner() const noexcept;
  const DatabaseInfo& database_info(DatabaseKind) const noexcept;
  // Borrowed connection; valid only for this Library's lifetime. SQLite itself
  // rejects writes on read-only connections. Do not close this handle.
  sqlite3* database(DatabaseKind) const noexcept;
  Result<DatabaseCheck> check_database(DatabaseKind, DatabaseCheckLevel);
  // Discard a damaged disposable index before constructing its consumers.
  Result<void> reset_index();
  repository::AssetRepository& assets() noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit Library(std::unique_ptr<Impl>);
};
}  // namespace am::library
