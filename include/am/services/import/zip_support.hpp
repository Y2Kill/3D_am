#pragma once
#include "am/services/import/scan.hpp"
#include "am/services/import/plan_view.hpp"
#include "am/storage/archive/zip_input.hpp"

namespace am::import {
PlanLimits snapshot_import_limits(const ImportProfile&);
archive::ZipInputLimits archive_limits(const PlanLimits&);
std::shared_ptr<archive::ZipInputJob> scan_archive_job(const ScanRequest&);
PlanJson effective_archive_limits(const PlanLimits&, const archive::ZipInputJob&);
PlanJson archive_limit_problem(const PlanLimits&, const archive::ZipInputJob&);
bool specialized_zip_hint(std::string_view path,std::string_view detected);
// Per-scan or per-attempt directory under <library>/.am/.am-tmp/import/, removed with its contents
// on destruction. Files appear only when a compressed nested record is unpacked.
class TempArea {
 public:
  explicit TempArea(const std::filesystem::path& library);
  TempArea(const TempArea&)=delete;
  TempArea& operator=(const TempArea&)=delete;
  ~TempArea();
  std::filesystem::path path;
};
Result<void> inventory_zip_entries(ImportPlan&, const ScanRequest&, std::shared_ptr<archive::ZipInputJob>);
} // namespace am::import
