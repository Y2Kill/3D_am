#pragma once

#include "am/storage/asset_zip/writer.hpp"
#include "am/storage/repository/repository.hpp"

namespace am::storage::exporting {
struct ExportFile {
  std::size_t file_index;
  std::string target_name;
};
// Request-local test/fault injection, never a plugin extension point.
struct ExportHooks {
  std::function<void()> before_copy;
  std::function<void(const std::filesystem::path&)> after_write;
  std::function<void(std::uint64_t)> on_read;
  std::function<std::uintmax_t(const std::filesystem::path&)> available_space;
  std::optional<std::uint64_t> max_file_size;
  // Observes actual repository verification reports for tests. The bool marks
  // the temporary asset container; plain ZIPs have no manifest verification.
  std::function<void(bool, const repository::VerificationReport&)> on_container_verification;
};
struct PrepareOptions {
  std::filesystem::path target_dir;
  bool full = true, verify_full = false;
  std::vector<ExportFile> files;
  std::uint64_t read_budget = repository::ContainerVerifyOptions{}.read_budget;
  std::stop_token stop;
  ExportHooks hooks;
};
class PreparedExport {
 public:
  PreparedExport();
  PreparedExport(PreparedExport&&) noexcept;
  PreparedExport& operator=(PreparedExport&&) noexcept;
  ~PreparedExport();
  PreparedExport(const PreparedExport&) = delete;
  PreparedExport& operator=(const PreparedExport&) = delete;
  std::filesystem::path temporary;
  std::string sha256;
  std::uint64_t size = 0;
  bool owned = false;
 private:
  // Keeps a live export distinguishable from an aged crash orphan, including
  // the checkpoint/publication interval after all writer handles are closed.
  std::shared_ptr<void> lease_;
  friend Result<PreparedExport> prepare(const repository::AssetSnapshot&, const PrepareOptions&);
};
Result<void> validate_target(const std::filesystem::path& library, const std::filesystem::path& target);
Result<PreparedExport> prepare(const repository::AssetSnapshot&, const PrepareOptions&);
// Idempotency probe: missing/foreign/nonregular targets return false. Never
// accepts a link target, and checks the whole file with bounded memory.
Result<bool> matches(const std::filesystem::path&, std::uint64_t size, std::string_view sha256,
                      std::stop_token = {});
}  // namespace am::storage::exporting
