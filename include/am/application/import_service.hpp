#pragma once
#include "am/application/asset_service.hpp"
#include "am/services/import/scan.hpp"
#include "am/application/job_service.hpp"

namespace am::application {
Result<import::ImportProfile> load_import_profile(std::string_view id_or_path = "default");
Result<AssetOutcome> scan_import(import::ScanRequest request, const std::filesystem::path& output);
Result<AssetOutcome> run_import(const std::filesystem::path& plan,const std::filesystem::path& library,
                                bool enqueue_only=false,const JobServiceOptions& = {},std::stop_token = {});
}  // namespace am::application
