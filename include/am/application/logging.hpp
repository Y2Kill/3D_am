#pragma once
#include <chrono>
#include <filesystem>
#include <functional>

#include "am/foundation/log.hpp"
#include "am/foundation/paths.hpp"

namespace am::application {
struct LoggingOptions {
  std::filesystem::path directory;
  log::Level level = log::Level::info;
  // Hypotheses; not storage or performance requirements.
  std::uint64_t max_file_bytes = 10ull << 20, max_total_bytes = 100ull << 20;
  unsigned max_files_per_process = 3;
  std::chrono::milliseconds flush_interval{2000};
  // Per-instance fault injection; empty in production.
  std::function<Result<void>()> before_write;
  std::function<Result<void>(const std::filesystem::path&)> before_remove;
};
Result<std::shared_ptr<log::Sink>> open_file_log(const LoggingOptions&);
struct DiagnosticOptions {
  // Per-request raw metadata tests/fault injection; not a plugin extension point.
  paths::NodeDataHook node_data;
  std::filesystem::path output;
  std::optional<std::filesystem::path> log_directory, library;
  std::optional<std::string> since;
  bool dry_run = false;
  // Redaction values/roots supplied by tests in addition to identities from the OS/environment.
  std::vector<std::string> users, hosts, roots;
  std::function<void(const std::filesystem::path&)> on_read;
};
struct UnavailableDiagnosticSource {
  std::string source_kind, name, reason;
};
struct DiagnosticReport {
  std::vector<std::string> entries;
  std::uint64_t path_replacements = 0, name_replacements = 0, user_replacements = 0, host_replacements = 0;
  bool logs_available = false;
  std::vector<UnavailableDiagnosticSource> unavailable;
  std::string skipped_summary;
};
Result<DiagnosticReport> export_diagnostics(const DiagnosticOptions&);
// Strict RFC3339 UTC, allowing optional fractional seconds and numeric offsets.
bool valid_diagnostic_timestamp(std::string_view) noexcept;
}  // namespace am::application
