#pragma once
#include "am/jobs/executor.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/services/import/plan_view.hpp"
#include "am/services/import/scan.hpp"

namespace am::import {
struct RunHooks {
  // Per-registration tests/fault injection, not plugin extension points.
  paths::NodeDataHook node_data;
  std::function<void(const std::filesystem::path&)> on_open;
  std::function<void(repository::CreateRequest&)> before_attempt;
  std::function<Result<void>(std::string_view)> after_created;
};
struct QueuedRun {
  std::string job_id, plan_sha256;
  bool reused = false;
};
Result<QueuedRun> enqueue_run(catalog::Catalog&, const std::filesystem::path& plan);
// Returns one row per candidate, including candidates without a child job.
Result<PlanJson> run_summary(catalog::Catalog&, const QueuedRun&);
Result<void> register_import_jobs(jobs::HandlerRegistry&, catalog::Catalog&, std::string device, RunHooks = {});
// Validates the plan and overlap before opening a writable library.
Result<PlanView> validate_run_plan(const std::filesystem::path& plan, const std::filesystem::path& library);
}  // namespace am::import
