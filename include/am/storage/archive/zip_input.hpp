#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "am/storage/asset_zip/reader.hpp"

namespace am::archive {
struct ZipInputLimits {
  asset_zip::ZipLimits zip;
  std::uint64_t max_unpacked_bytes = 64ULL * 1024 * 1024 * 1024; // hypothesis, O-17
  double max_ratio = 200; // hypothesis, O-17
  std::chrono::seconds max_seconds_per_container{3600}; // hypothesis, O-17
};
struct ZipInputHooks {
  // Per-job tests/fault injection, not plugin extension points.
  std::function<std::chrono::steady_clock::time_point()> clock;
  std::function<void(const std::filesystem::path&)> on_open;
  std::function<Result<void>(const std::filesystem::path&, std::size_t)> before_read;
};
// One context for all archives, samples, passes and retries in a candidate job.
class ZipInputJob {
 public:
  explicit ZipInputJob(ZipInputLimits limits = {}, ZipInputHooks hooks = {});
  std::uint64_t remaining() const noexcept;
  std::string exceeded_limit() const;
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  friend class ZipInput;
  friend class ZipInputStream;
};
struct ZipInputEntry {
  asset_zip::EntryInfo info;
  std::string path, name_encoding, problem_code, path_problem;
  bool encoding_exact = true;
  std::optional<double> encoding_confidence;
};
class ZipInputStream {
 public:
  ZipInputStream(ZipInputStream&&) noexcept;
  ZipInputStream& operator=(ZipInputStream&&) noexcept;
  ~ZipInputStream();
  Result<std::size_t> read(std::span<std::byte>);
  std::optional<asset_zip::EntryVerification> verification() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit ZipInputStream(std::unique_ptr<Impl>);
  friend class ZipInput;
};
class ZipInput {
 public:
  static Result<ZipInput> open(const std::filesystem::path&, std::shared_ptr<ZipInputJob>);
  std::span<const ZipInputEntry> entries() const noexcept;
  const asset_zip::ArchiveSummary& summary() const noexcept;
  Result<ZipInputStream> open_entry(std::size_t index, bool sha256 = false) const;
  Result<std::vector<std::byte>> sample(std::size_t index, std::size_t bytes) const;
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  explicit ZipInput(std::shared_ptr<Impl>);
};
// Error categories for the closed import problem vocabulary.
std::string_view zip_input_failure_kind(const Error&) noexcept;
} // namespace am::archive
