#pragma once
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include "am/storage/db/sqlite.hpp"

namespace am::db {
struct LogEntry {
  std::int64_t seq = 0;
  std::string at, level, code;
  std::optional<std::string> job_id, asset_id;
  std::string message;
  std::optional<std::string> detail_json;
};
struct LogQuery {
  std::optional<std::string> job_id, asset_id;
  std::optional<std::int64_t> after_seq;
  std::size_t limit = 100;
};
class OperationLog {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;
  explicit OperationLog(sqlite3*, bool read_only = false, Clock = {});
  Result<std::int64_t> append(const LogEntry&);
  Result<std::vector<LogEntry>> query(const LogQuery& = {}) const;
  Result<std::size_t> prune_before(std::string_view);
 private:
  sqlite3* raw_;
  Connection db_;
  bool read_only_;
  Clock clock_;
  mutable std::mutex mutex_;
};
} // namespace am::db
