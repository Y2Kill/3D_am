#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "am/storage/asset_zip/source.hpp"
#include "am/storage/asset_zip/sha256.hpp"

namespace am::asset_zip {
struct ZipLimits {
  std::uint64_t max_entries = 100000;                         // hypothesis, O-17
  std::uint64_t max_directory_bytes = 64ULL * 1024 * 1024;    // hypothesis, O-17
  std::uint64_t max_name_bytes = 4096;                       // hypothesis, O-17
  std::uint64_t max_entry_bytes = 64ULL * 1024 * 1024 * 1024; // hypothesis, O-17
};
enum class EntryKind { regular, directory, symlink, special };
struct ByteRange { std::uint64_t offset = 0, length = 0; };
struct EntryInfo {
  std::size_t index = 0;
  std::string raw_name, utf8_name;
  std::uint16_t method = 0, flags = 0, version_made_by = 0;
  bool encrypted = false, data_descriptor = false, utf8_flag = false, has_zip64 = false;
  std::uint32_t crc32 = 0, external_attributes = 0;
  std::uint64_t compressed_size = 0, uncompressed_size = 0, local_offset = 0, data_offset = 0, record_end = 0;
  EntryKind kind = EntryKind::regular;
  std::vector<std::byte> central_extra, local_extra;
  std::vector<std::byte> comment;
  std::uint16_t dos_time = 0, dos_date = 0;
};
struct ArchiveSummary {
  std::uint64_t entry_count = 0, directory_offset = 0, directory_size = 0, unaccounted_bytes = 0;
  bool zip64 = false;
  std::uint16_t comment_length = 0;
};
// Shared job-wide budget. Must outlive every stream borrowing it. Atomic so
// independent entry streams can share it concurrently without overspending.
class ReadBudget {
 public:
  explicit ReadBudget(std::uint64_t bytes) : remaining_(bytes) {}
  std::uint64_t remaining() const noexcept { return remaining_.load(); }
  bool consume(std::uint64_t bytes) noexcept;
 private:
  std::atomic<std::uint64_t> remaining_;
};
struct EntryVerification {
  std::uint64_t size = 0;
  std::uint32_t crc32 = 0;
  std::optional<Sha256Digest> sha256;
};
class ZipArchive;
class EntryStream {
 public:
  EntryStream(EntryStream&&) noexcept;
  EntryStream& operator=(EntryStream&&) noexcept;
  ~EntryStream();
  EntryStream(const EntryStream&) = delete;
  EntryStream& operator=(const EntryStream&) = delete;
  // Supply a nonempty buffer. 0 means validated EOF; errors are sticky.
  Result<std::size_t> read(std::span<std::byte> output);
  std::optional<EntryVerification> verification() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit EntryStream(std::unique_ptr<Impl> impl);
  friend class ZipArchive;
};
class ZipArchive {
 public:
  static Result<ZipArchive> open(std::shared_ptr<const RandomAccessSource> source, const ZipLimits& limits = {});
  std::span<const EntryInfo> entries() const noexcept;
  const ArchiveSummary& summary() const noexcept;
  Result<ByteRange> raw_range(std::size_t index) const;
  // Relative compressed offset; bounded by raw_range, without budget charges.
  Result<std::size_t> read_raw(std::size_t index, std::uint64_t offset, std::span<std::byte> output) const;
  Result<EntryStream> open_entry(std::size_t index, ReadBudget& budget, bool sha256 = false) const;
  Result<EntryVerification> verify_entry(std::size_t index, ReadBudget& budget, bool sha256 = false) const;
 private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  explicit ZipArchive(std::shared_ptr<const Impl> impl);
};
}  // namespace am::asset_zip
