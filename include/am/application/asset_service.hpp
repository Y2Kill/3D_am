#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "am/foundation/result.hpp"

namespace am::application {
using Json = nlohmann::ordered_json;
enum class ExitCode { success = 0, check_failed = 1, usage = 2, partial = 3, operation_failed = 4 };
enum class VerificationLevel { quick, selective, full };
// Structured application result, shared by command-line and GUI presenters.
struct AssetOutcome {
  Json value = Json::object();
  ExitCode status = ExitCode::success;
  std::vector<Error> diagnostics;
};
struct CreateAssetOptions {
  std::vector<std::filesystem::path> files;
  std::filesystem::path output;
  std::optional<std::string> title;
  // Test-only publication races; empty in normal operation.
  std::function<void(const std::filesystem::path&)> before_publish;
  std::function<void(const std::filesystem::path&)> after_publish;
};
struct VerifyAssetOptions {
  VerificationLevel level = VerificationLevel::full;
  std::vector<std::string> file_ids;
};
struct ExtractAssetOptions {
  std::filesystem::path directory;
  std::vector<std::string> file_ids;
  bool manifest = false;
};
Result<AssetOutcome> create_asset(const CreateAssetOptions&);
Result<AssetOutcome> inspect_asset(const std::filesystem::path&);
Result<AssetOutcome> validate_asset(const std::filesystem::path&);
Result<AssetOutcome> verify_asset(const std::filesystem::path&, const VerifyAssetOptions& = {});
Result<AssetOutcome> extract_asset(const std::filesystem::path&, const ExtractAssetOptions&);
std::string_view message_text(std::string_view message_id) noexcept;
std::string_view message_text(const Error&) noexcept;
ExitCode error_exit_code(const Error&) noexcept;
Result<Json> error_json(const Error&);
}  // namespace am::application
