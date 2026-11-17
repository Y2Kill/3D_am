#pragma once
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include "am/storage/asset_zip/reader.hpp"
#include "am/storage/asset_zip/sink.hpp"
#include "am/storage/asset_zip/compression_policy.hpp"

namespace am::asset_zip {
struct DosTime { std::uint16_t date = 0x0021, time = 0; };
struct WriterLimits { std::uint64_t max_file_size = std::numeric_limits<std::uint64_t>::max(); };
struct WriterOptions {
  ZipLimits zip_limits;
  WriterLimits limits;
  int deflate_level = 6; // ADR-0005
  DosTime dos_time;
};
struct EntryOptions {
  CompressionMethod method = CompressionMethod::deflate;
  std::uint64_t uncompressed_size = 0;
  std::optional<DosTime> dos_time;
  bool sha256 = true;
};
struct RawCopyOptions {
  std::vector<std::uint16_t> carry_extra_ids;
  bool carry_comment = false;
  // Explicit carry settings take priority over dropping unlisted metadata.
  bool drop_unlisted = false;
  std::optional<std::string> target_name;
};
struct WrittenEntry {
  std::size_t index = 0;
  std::string name;
  CompressionMethod method = CompressionMethod::stored;
  std::uint16_t flags = 0x800;
  std::uint32_t crc32 = 0;
  std::uint64_t compressed_size = 0, uncompressed_size = 0, local_offset = 0, data_offset = 0;
  std::optional<Sha256Digest> sha256;
  // Distinct discarded non-ZIP64 field IDs across local and central headers.
  std::size_t dropped_extra_fields = 0;
  bool dropped_comment = false;
};
struct WriteSummary {
  std::vector<WrittenEntry> entries;
  std::uint64_t directory_offset = 0, directory_size = 0, file_size = 0;
  bool zip64 = false;
  ZipLimits zip_limits;
};
class ZipWriter;
class EntryWriter {
 public:
  EntryWriter(EntryWriter&&) noexcept;
  EntryWriter& operator=(EntryWriter&&) noexcept;
  ~EntryWriter();
  EntryWriter(const EntryWriter&) = delete;
  EntryWriter& operator=(const EntryWriter&) = delete;
  Result<void> write(std::span<const std::byte> bytes);
  Result<WrittenEntry> finish();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit EntryWriter(std::unique_ptr<Impl> impl);
  friend class ZipWriter;
};
class ZipWriter {
 public:
  ZipWriter(ZipWriter&&) noexcept;
  ZipWriter& operator=(ZipWriter&&) noexcept;
  ~ZipWriter();
  ZipWriter(const ZipWriter&) = delete;
  ZipWriter& operator=(const ZipWriter&) = delete;
  static Result<ZipWriter> create(std::shared_ptr<RandomAccessSink> sink, const WriterOptions& options = {});
  Result<EntryWriter> begin_entry(std::string_view name, const EntryOptions& options);
  Result<WrittenEntry> copy_raw(const ZipArchive& source, std::size_t index, ReadBudget& budget, const RawCopyOptions& options = {});
  Result<WriteSummary> finish();
  Result<void> abort();
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  explicit ZipWriter(std::shared_ptr<Impl> impl);
  friend class EntryWriter;
};
Result<void> verify_written(std::shared_ptr<const RandomAccessSource> source, const WriteSummary& summary, ReadBudget& budget);
}  // namespace am::asset_zip
