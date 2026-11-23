#pragma once
#include "am/application/asset_service.hpp"
#include "am/services/import/scan.hpp"

namespace am::application {
Result<import::ImportProfile> load_import_profile(std::string_view id_or_path = "default");
Result<AssetOutcome> scan_import(import::ScanRequest request, const std::filesystem::path& output);
}  // namespace am::application
