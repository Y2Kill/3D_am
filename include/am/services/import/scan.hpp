#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>

#include "am/foundation/paths.hpp"
#include "am/foundation/result.hpp"

namespace am::import {
using PlanJson = nlohmann::json;
struct ImportProfile {
  PlanJson settings;
  std::string sha256;
};
// Validates all v1 sections, including those deferred to import run.
Result<void> validate_profile(const ImportProfile&);
struct ImportPlan {
  PlanJson value;
};
struct ScanProgress {
  std::uint64_t nodes = 0, files = 0;
};
enum class ScanIo { stat, enumerate, open, header, hash, shortcut, zip_probe };
struct ScanHooks {
  // Per-request tests/fault injection of raw OS metadata; not a plugin extension point.
  paths::NodeDataHook node_data;
  std::function<Result<void>(const std::filesystem::path&, ScanIo)> before_io;
  // All source opens are read-only; also observes ZIP and shortcut probes.
  std::function<void(const std::filesystem::path&, bool writable)> on_open;
  std::function<void(const std::filesystem::path&)> on_library_asset_read;
  std::function<std::chrono::system_clock::time_point()> clock;
};
struct ScanRequest {
  std::vector<std::filesystem::path> roots;
  ImportProfile profile;
  bool single_candidate = false;
  std::optional<std::filesystem::path> library;
  bool hash_inputs = false, skip_identical = false;
  std::function<bool()> cancelled;
  std::function<void(const ScanProgress&)> on_progress;
  ScanHooks hooks;
};
Result<ImportPlan> scan(const ScanRequest&);
// Shared preflight for source/library overlap and optional plan publication.
Result<void> validate_scan_paths(const ScanRequest&, const std::optional<std::filesystem::path>& output = {});
// Returns an empty string if serialization cannot complete.
std::string serialize_plan(const ImportPlan&) noexcept;
Result<ImportPlan> parse_plan(std::string_view);
}  // namespace am::import
