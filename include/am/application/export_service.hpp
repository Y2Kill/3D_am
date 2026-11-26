#pragma once

#include "am/application/asset_service.hpp"
#include "am/application/job_service.hpp"
#include "am/services/export/run.hpp"

namespace am::application {
struct ExportOptions {
  exporting::ExportRequest request;
  bool enqueue_only = false;
  JobServiceOptions jobs;
  library::OpenOptions library;
};
Result<AssetOutcome> export_assets(const std::filesystem::path& library, const ExportOptions&,
                                    std::stop_token = {});
}  // namespace am::application
