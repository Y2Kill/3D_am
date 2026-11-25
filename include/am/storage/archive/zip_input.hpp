#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "am/storage/asset_zip/probe.hpp"

namespace am::archive {
struct ZipInputLimits {
  asset_zip::ZipLimits zip;
  std::uint64_t max_unpacked_bytes = 64ULL * 1024 * 1024 * 1024; // hypothesis, O-17
  double max_ratio = 200; // hypothesis, O-17
  std::chrono::duration<long double> max_seconds_per_container{3600}; // hypothesis, O-17
  std::uint64_t max_temp_bytes = 100ULL * 1024 * 1024 * 1024; // peak temporary bytes per job, hypothesis, O-17
  std::uint64_t max_depth = 8; // archive levels including the filesystem archive, hypothesis, O-17
};
// Values above these ceilings are lowered regardless of the plan (O-17 hypotheses).
inline constexpr std::uint64_t unconditional_max_depth = 8;
struct ZipInputHooks {
  // Per-job tests/fault injection, not plugin extension points.
  std::function<std::chrono::steady_clock::time_point()> clock;
  std::function<bool()> cancelled;
  std::function<void(const std::filesystem::path&)> on_open;
  std::function<Result<void>(const std::filesystem::path&, std::size_t)> before_read;
};
// One context for all archives, samples, passes and retries in a candidate job.
class ZipInputJob {
 public:
  explicit ZipInputJob(ZipInputLimits limits = {}, ZipInputHooks hooks = {});
  std::uint64_t remaining() const noexcept;
  std::string exceeded_limit() const;
  const ZipInputLimits& limits() const noexcept;
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
struct ZipInputIdentity {
  std::string raw_name, path, name_encoding;
  std::uint64_t size=0, compressed_size=0;
  std::uint32_t crc32=0;
  std::uint16_t method=0;
  asset_zip::EntryKind kind=asset_zip::EntryKind::regular;
  bool encrypted=false;
  bool operator==(const ZipInputIdentity&) const = default;
};
struct ZipInputSnapshot {
  // For a nested archive `path` is a label (parent label + "!" + record index), not a file.
  std::filesystem::path path;
  std::vector<ZipInputIdentity> entries;
  std::optional<std::string> sha256;
  std::shared_ptr<ZipInputJob> job;
  std::shared_ptr<const ZipInputSnapshot> parent;  // set for a nested archive
  std::size_t parent_index = 0;
  std::filesystem::path temp_dir;  // managed temporary area for compressed nested records
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
  static Result<asset_zip::ProbeResult> probe(std::shared_ptr<const asset_zip::RandomAccessSource>,
      const std::filesystem::path&, std::shared_ptr<ZipInputJob>, const manifest::CodecLimits& = {}, std::stop_token = {});
  static Result<ZipInput> open(const std::filesystem::path&, std::shared_ptr<ZipInputJob>);
  std::span<const ZipInputEntry> entries() const noexcept;
  const asset_zip::ArchiveSummary& summary() const noexcept;
  std::vector<ZipInputIdentity> identity() const;
  Result<std::string> sha256(std::stop_token = {}) const;
  Result<ZipInputStream> open_entry(std::size_t index, bool sha256 = false) const;
  Result<std::vector<std::byte>> sample(std::size_t index, std::size_t bytes) const;
  // Opens a regular record as a nested ZIP. A Stored record is read in place as a byte range after
  // one verifying pass; any other record is unpacked into a file under temp_dir that is removed when
  // the last reader or stream using it is gone. An empty temp_dir refuses compressed records with
  // am.archive.temp_unavailable. Depth and temporary bytes are charged to the shared job.
  Result<ZipInput> open_nested(std::size_t index, const std::filesystem::path& temp_dir, std::stop_token = {}) const;
  std::uint64_t depth() const noexcept;
  bool temporary() const noexcept;
  const std::filesystem::path& label() const noexcept;
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  explicit ZipInput(std::shared_ptr<Impl>);
  static ZipInput open_source(std::shared_ptr<const asset_zip::RandomAccessSource>, std::filesystem::path label,
                              std::shared_ptr<ZipInputJob>, std::uint64_t depth, bool temporary);
};
// Opens a snapshot's archive, following the chain of parents for nested archives.
Result<ZipInput> open_snapshot(const ZipInputSnapshot&, std::shared_ptr<ZipInputJob>, std::stop_token = {});
// Error categories for the closed import problem vocabulary.
std::string_view zip_input_failure_kind(const Error&) noexcept;
} // namespace am::archive
