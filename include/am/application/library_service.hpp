#pragma once
#include "am/application/asset_service.hpp"
namespace am::application {
Result<AssetOutcome> init_library(const std::filesystem::path&, const std::string& name = {});
Result<AssetOutcome> inspect_library(const std::filesystem::path&);
}  // namespace am::application
