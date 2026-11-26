#pragma once

#include "am/jobs/executor.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/services/export/categories.hpp"
#include "am/storage/export/prepare.hpp"

namespace am::exporting {
using ExportJson = nlohmann::ordered_json;
enum class ConflictPolicy { fail, rename };
struct ExportRequest {
  std::vector<std::string> asset_ids;
  std::filesystem::path target_dir;
  std::vector<Preset> presets;
  std::vector<std::string> file_ids;
  std::optional<std::string> name;
  ConflictPolicy on_conflict = ConflictPolicy::fail;
  bool verify_full = false;
};
struct RunHooks {
  storage::exporting::ExportHooks storage;
  // Request-local tests/fault injection only; no plugin extension points.
  std::function<void(const repository::AssetSnapshot&)> after_snapshot;
  std::function<void(const std::filesystem::path&)> before_publish, after_publish;
};
Result<void> validate_request(catalog::Catalog&, const ExportRequest&);
Result<std::string> enqueue_export(catalog::Catalog&, const ExportRequest&);
Result<ExportJson> export_summary(catalog::Catalog&, std::string_view parent_id);
Result<ExportJson> export_direct(catalog::Catalog&, const ExportRequest&, std::stop_token = {}, RunHooks = {});
Result<void> register_export_jobs(jobs::HandlerRegistry&, catalog::Catalog&, jobs::ResourceManager&,
                                   RunHooks = {});
}  // namespace am::exporting
