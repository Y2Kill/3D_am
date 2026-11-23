#pragma once
#include "am/application/asset_service.hpp"
#include "am/jobs/executor.hpp"
#include "am/services/catalog/indexer.hpp"

namespace am::application {
struct JobServiceOptions {
  jobs::Profile profile = jobs::Profile::balanced;
  std::optional<unsigned> parallel_jobs;
  std::optional<jobs::ResourceLimits> limits;
  // Hypotheses, matching ExecutorOptions; tests may use zero reporting/retry intervals.
  std::chrono::milliseconds report_interval{250}, retry_interval{30000};
  int auto_retries = 3;
  std::function<void(const jobs::JobEvent&)> on_event;
};
class JobService {
 public:
  static Result<JobService> open(const std::filesystem::path&, const JobServiceOptions& = {}, library::OpenOptions = {},
                                 am::catalog::IndexerHooks = {});
  JobService(JobService&&) noexcept;
  JobService& operator=(JobService&&) noexcept;
  ~JobService();
  JobService(const JobService&) = delete;
  JobService& operator=(const JobService&) = delete;
  Result<contracts::JobRecord> enqueue(const contracts::JobSpec&);
  Result<jobs::RunSummary> run(std::stop_token = {});
  Result<void> control(std::string_view command, std::string_view id);
  Result<void> answer(std::string_view id, std::string_view option);
  Result<void> move(std::string_view id, std::optional<std::string_view> before);
  contracts::IJobStore& store() noexcept;
  am::catalog::Catalog& catalog() noexcept;
  std::vector<Error> diagnostics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit JobService(std::unique_ptr<Impl>);
};
// Like LibraryLogOutcome, the encoded JSON preserves arbitrary numeric lexemes
// accepted by IJobStore. value/records support the text and future GUI presenters.
struct JobCommandOutcome {
  Json value = Json::object();
  std::string json;
  std::vector<contracts::JobRecord> records;
  std::vector<Error> diagnostics;
  ExitCode status = ExitCode::success;
};
Result<JobCommandOutcome> list_jobs(const std::filesystem::path&, const contracts::JobFilter& = {}, bool all = false);
Result<JobCommandOutcome> show_job(const std::filesystem::path&, std::string_view id);
Result<JobCommandOutcome> run_jobs(const std::filesystem::path&, const JobServiceOptions& = {}, std::stop_token = {});
Result<JobCommandOutcome> control_jobs(const std::filesystem::path&, std::string_view, const std::vector<std::string>&);
Result<JobCommandOutcome> answer_job(const std::filesystem::path&, std::string_view id, std::string_view option);
Result<JobCommandOutcome> move_job(const std::filesystem::path&, std::string_view id,
                                   std::optional<std::string_view> before);
Result<JobCommandOutcome> enqueue_verify(const std::filesystem::path&, bool full = false, bool include_trash = true);
Result<JobCommandOutcome> enqueue_probe(const std::filesystem::path&, std::uint64_t steps, bool ask,
                                        const std::optional<std::string>& failure = {});
Result<JobCommandOutcome> inspect_media(const std::filesystem::path&);
}  // namespace am::application
