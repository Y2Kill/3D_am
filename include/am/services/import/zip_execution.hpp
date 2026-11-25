#pragma once
#include "am/services/import/run.hpp"
#include "am/services/import/zip_support.hpp"

namespace am::import {
// Attempt-local IDs and shared archive budgets never enter durable job parameters.
struct ZipExecution {
  std::vector<std::size_t> archives;
  std::map<std::size_t, PlanJson> ids;
  std::map<std::size_t, PlanProblem> failed;
  std::map<std::size_t, std::shared_ptr<archive::ZipInputSnapshot>> snapshots;
  std::shared_ptr<archive::ZipInputJob> job;
  // Managed temporary area for compressed nested archives; removed when the attempt ends.
  std::shared_ptr<TempArea> temp;
};
PlanEntry archive_node_decision(const PlanCandidate&, std::size_t node);
PlanEntry archive_entry_decision(const PlanView&, const PlanCandidate&, std::size_t entry);
ZipExecution zip_execution(const PlanView&, const PlanCandidate&, const RunHooks&, std::stop_token,
                           const std::filesystem::path& library);
Result<void> prepare_zip_execution(ZipExecution&, const PlanView&, const PlanCandidate&, std::stop_token);
void append_zip_execution(ZipExecution&, const PlanView&, const PlanCandidate&, const PlanJson& params,
                         PlanJson& input, repository::AddFiles&, paths::ZipPathMapper&,
                         const std::map<std::size_t, std::string>& prefixes);
} // namespace am::import
