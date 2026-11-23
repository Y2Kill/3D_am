#pragma once
#include <chrono>
#include <functional>
#include <map>
#include <thread>

#include "am/contracts/job_store.hpp"
#include "am/jobs/resources.hpp"

namespace am::jobs {
enum class StopReason { none, pause, cancel, shutdown };
struct Decision {
  std::string kind;
  std::vector<std::string> options;
  std::string context_json = "{}";
};
struct ChildrenSummary {
  std::vector<contracts::JobRecord> records;
  std::map<contracts::JobState, std::size_t> counts;
  contracts::JobProgress progress;
  std::optional<Error> error;
  bool settled() const noexcept;
};
ChildrenSummary summarize_children(std::vector<contracts::JobRecord>);
enum class OutcomeKind {
  succeeded,
  failed,
  needs_decision,
  waiting_resources,
  resource_error,
  cannot_continue_safely,
  interrupted,
  await_children
};
struct JobOutcome {
  OutcomeKind kind = OutcomeKind::succeeded;
  std::string result_json = "{}";
  std::optional<Error> error;
  Decision decision;
  static JobOutcome success(std::string result = "{}");
  static JobOutcome failure(Error);
  static JobOutcome ask(Decision);
  static JobOutcome interrupted();
  static JobOutcome await_children();
};
struct ExecutorState;
struct Execution;
class JobContext {
 public:
  const contracts::JobRecord& record() const noexcept;
  std::stop_token stop() const noexcept;
  StopReason stop_reason() const noexcept;
  void report(const contracts::JobProgress&);
  Result<Lease> acquire(std::span<const ResourceRequest>);
  std::optional<std::string> answer() const;
  Profile profile() const noexcept;
  Result<contracts::JobRecord> enqueue_child(contracts::JobSpec);
  ChildrenSummary children() const;

 private:
  std::shared_ptr<ExecutorState> state_;
  std::shared_ptr<Execution> execution_;
  JobContext(std::shared_ptr<ExecutorState>, std::shared_ptr<Execution>);
  friend struct ExecutorState;
};
class JobHandler {
 public:
  virtual ~JobHandler() = default;
  virtual JobOutcome run(JobContext&) = 0;
};
class HandlerRegistry {
 public:
  using Factory = std::function<std::unique_ptr<JobHandler>(const contracts::JobRecord&)>;
  Result<void> add(std::string type, Factory);
  // Registration is completed before starting an executor.
  const Factory* find(std::string_view) const noexcept;

 private:
  std::map<std::string, Factory, std::less<>> factories_;
};
enum class EventKind { started, progress, state_changed, finished };
struct JobEvent {
  EventKind kind;
  contracts::JobRecord job;
  std::optional<double> rate;
};
class JobObserver {
 public:
  virtual ~JobObserver() = default;
  virtual void on_event(const JobEvent&) noexcept = 0;
};
struct ExecutorOptions {
  ResourceLimits limits = profile_limits(Profile::balanced, std::thread::hardware_concurrency());
  Profile profile = Profile::balanced;
  // Scheduling/progress/retry intervals and retry budget are hypotheses.
  std::chrono::milliseconds report_interval{250}, retry_interval{30000}, poll_interval{250};
  int auto_retries = 3;
  // Injectable monotonic clock for deterministic progress tests.
  std::function<std::chrono::steady_clock::time_point()> clock;
};
struct RunSummary {
  std::size_t processed = 0, succeeded = 0, failed = 0, attention = 0;
  bool interrupted = false;
  std::vector<contracts::JobRecord> jobs;
};
class Executor {
 public:
  Executor(contracts::IJobStore&, ResourceManager&, const HandlerRegistry&, ExecutorOptions, JobObserver* = nullptr);
  ~Executor();
  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;
  Result<RunSummary> run_until_idle(std::stop_token = {});
  Result<void> pause(std::string_view);
  Result<void> resume(std::string_view);
  Result<void> cancel(std::string_view);
  Result<void> retry(std::string_view);
  Result<void> answer(std::string_view, std::string_view);
  // Enqueuers sharing this store may wake the scheduler without waiting for its poll interval.
  void wake() noexcept;

 private:
  std::shared_ptr<ExecutorState> state_;
};
}  // namespace am::jobs
