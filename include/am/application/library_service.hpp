#pragma once
#include "am/application/asset_service.hpp"
namespace am::application {
Result<AssetOutcome> init_library(const std::filesystem::path&, const std::string& name = {});
Result<AssetOutcome> inspect_library(const std::filesystem::path&);
Result<AssetOutcome> rebuild_library(const std::filesystem::path&);
Result<AssetOutcome> list_library(const std::filesystem::path&, bool include_trash = false);
Result<AssetOutcome> sync_library(const std::filesystem::path&, bool full = false);
Result<AssetOutcome> trash_library(const std::filesystem::path&, const std::vector<std::string>& asset_ids);
Result<AssetOutcome> restore_library(const std::filesystem::path&, const std::vector<std::string>& asset_ids);
Result<AssetOutcome> purge_library(const std::filesystem::path&, const std::vector<std::string>& asset_ids, bool all, bool confirmed);
}  // namespace am::application
