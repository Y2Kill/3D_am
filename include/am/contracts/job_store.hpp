#pragma once
#include <array>
#include <optional>
#include "am/foundation/result.hpp"

namespace am::contracts {
enum class JobState { queued, running, paused, needs_decision, waiting_resources, resource_error,
                      cannot_continue_safely, succeeded, failed, cancelled };
inline constexpr std::array<std::string_view,10> job_state_names{
  "queued","running","paused","needs_decision","waiting_resources","resource_error",
  "cannot_continue_safely","succeeded","failed","cancelled"};
inline std::string_view job_state_name(JobState state) noexcept {
  const auto i = static_cast<std::size_t>(state);
  return i < job_state_names.size() ? job_state_names[i] : std::string_view{};
}
inline std::optional<JobState> parse_job_state(std::string_view name) noexcept {
  for (std::size_t i = 0; i < job_state_names.size(); ++i)
    if (job_state_names[i] == name) return static_cast<JobState>(i);
  return {};
}
constexpr bool job_terminal(JobState state) noexcept { return state == JobState::succeeded || state == JobState::cancelled; }
constexpr bool job_transition_allowed(JobState from, JobState to) noexcept {
  using enum JobState;
  switch (from) {
    case queued: return to == running || to == paused || to == cancelled;
    case running: return to == paused || to == needs_decision || to == waiting_resources || to == resource_error ||
      to == cannot_continue_safely || to == succeeded || to == failed || to == cancelled || to == queued;
    case paused: case needs_decision: case waiting_resources: case cannot_continue_safely:
      return to == queued || to == cancelled;
    case resource_error: return to == queued || to == failed || to == cancelled;
    case failed: return to == queued;
    default: return false;
  }
}
struct JobProgress { std::optional<std::uint64_t> done, total; std::string unit, stage; };
struct JobSpec {
  std::string type, params_json = "{}";
  std::optional<std::string> parent_id, job_id;
  int priority = 0;
};
struct JobRecord {
  std::string job_id, type, params_json;
  std::optional<std::string> parent_id;
  JobState state = JobState::queued;
  int priority = 0;
  std::int64_t position = 0;
  int attempt = 0;
  JobProgress progress;
  std::optional<std::string> decision_json, result_json, error_json;
  std::string created_at;
  std::optional<std::string> started_at, finished_at;
};
// queued -> running consumes an attempt. A cooperative shutdown may release it
// only on running -> queued; crash recovery keeps the previous attempt count.
// A decision answer is the original question plus {"answer":{"option":"..."}}.
// A result belongs to the last attempt and is cleared at queued -> running.
struct JobPatch {
  std::optional<std::string> decision_json, result_json, error_json;
  bool clear_decision = false, release_attempt = false;
};
struct JobFilter { std::vector<JobState> states; std::optional<std::string> parent_id; bool top_level_only = false; };
class IJobStore {
 public:
  virtual ~IJobStore() = default;
  virtual Result<JobRecord> enqueue(const JobSpec&) = 0;
  virtual Result<std::optional<JobRecord>> get(std::string_view) = 0;
  virtual Result<std::vector<JobRecord>> list(const JobFilter& = {}) = 0;
  virtual Result<JobRecord> transition(std::string_view, JobState, JobState, const JobPatch& = {}) = 0;
  virtual Result<void> report(std::string_view, const JobProgress&) = 0;
  virtual Result<void> move_before(std::string_view, std::optional<std::string_view>) = 0;
  virtual Result<std::size_t> remove_finished(std::string_view) = 0;
};
} // namespace am::contracts
