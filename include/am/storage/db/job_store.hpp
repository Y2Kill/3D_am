#pragma once
#include <chrono>
#include <functional>
#include <mutex>
#include "am/contracts/job_store.hpp"
#include "am/storage/db/sqlite.hpp"

namespace am::db {
struct RecoveredJobs { std::vector<std::string> requeued, failed; };
class StateJobStore final : public contracts::IJobStore {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;
  // Borrowed connection; its owner must outlive this store. Three attempts is a hypothesis.
  explicit StateJobStore(sqlite3*, bool read_only = false, Clock = {}, int max_attempts = 3);
  Result<contracts::JobRecord> enqueue(const contracts::JobSpec&) override;
  Result<std::optional<contracts::JobRecord>> get(std::string_view) override;
  Result<std::vector<contracts::JobRecord>> list(const contracts::JobFilter& = {}) override;
  Result<contracts::JobRecord> transition(std::string_view, contracts::JobState, contracts::JobState,
                                         const contracts::JobPatch& = {}) override;
  Result<void> report(std::string_view, const contracts::JobProgress&) override;
  Result<void> move_before(std::string_view, std::optional<std::string_view>) override;
  Result<std::size_t> remove_finished(std::string_view) override;
  // Startup-only transition; ordinary transition() cannot requeue running jobs.
  Result<RecoveredJobs> recover_running();
 private:
  sqlite3* raw_;
  Connection db_;
  bool read_only_;
  Clock clock_;
  int max_attempts_;
  std::recursive_mutex mutex_;
  void writable() const;
};
} // namespace am::db
