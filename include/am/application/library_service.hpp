#pragma once
#include "am/application/asset_service.hpp"
#include "am/storage/db/operation_log.hpp"
namespace am::application {
struct LibraryLogOutcome {
  std::string json;
  std::vector<db::LogEntry> entries;
  std::vector<Error> diagnostics;
};
Result<LibraryLogOutcome> library_log(const std::filesystem::path&, const db::LogQuery& = {});
Result<AssetOutcome> init_library(const std::filesystem::path&, const std::string& name = {});
Result<AssetOutcome> inspect_library(const std::filesystem::path&);
Result<AssetOutcome> rebuild_library(const std::filesystem::path&);
Result<AssetOutcome> list_library(const std::filesystem::path&, bool include_trash = false);
Result<AssetOutcome> sync_library(const std::filesystem::path&, bool full = false);
Result<AssetOutcome> copy_library(const std::filesystem::path&, const std::string& asset_id, bool verbose = false);
Result<AssetOutcome> verify_library(const std::filesystem::path&, bool full = false, bool include_trash = true, bool verbose = false);
Result<AssetOutcome> trash_library(const std::filesystem::path&, const std::vector<std::string>& asset_ids);
Result<AssetOutcome> restore_library(const std::filesystem::path&, const std::vector<std::string>& asset_ids);
Result<AssetOutcome> purge_library(const std::filesystem::path&, const std::vector<std::string>& asset_ids, bool all, bool confirmed);
Result<AssetOutcome> add_ready_library(const std::filesystem::path&, const std::vector<std::filesystem::path>& sources,
                                     const std::optional<std::string>& replace_expected = {}, bool verbose = false);
}  // namespace am::application
