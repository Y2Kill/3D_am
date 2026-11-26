#pragma once

#include <chrono>
#include <filesystem>

#include "am/foundation/result.hpp"

namespace am::storage::fs {
// The source and target must be on the same filesystem. Never replaces any
// existing directory entry; POSIX also flushes the destination directory.
Result<void> publish_no_replace(const std::filesystem::path& temporary, const std::filesystem::path& target);
struct OrphanOptions {
  std::chrono::seconds age{24 * 60 * 60};  // hypothesis, T-033
  std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now();
};
Result<std::size_t> cleanup_export_orphans(const std::filesystem::path&, const OrphanOptions& = {});
}  // namespace am::storage::fs
