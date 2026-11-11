#pragma once
// Shared ZIP structure primitives and bounded stream validation; private to storage.
#include "am/storage/asset_zip/reader.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <zlib.h>
namespace am::asset_zip::detail {
using U64 = std::uint64_t;
using Bytes = std::span<const std::byte>;
inline constexpr std::uint32_t eocd_sig = 0x06054b50, local_sig = 0x04034b50, central_sig = 0x02014b50;
inline constexpr std::uint32_t locator_sig = 0x07064b50, end64_sig = 0x06064b50, descriptor_sig = 0x08074b50;
inline constexpr U64 max32 = 0xffffffffULL;
inline constexpr std::uint16_t encryption_flags = 1 | 64 | 0x2000;
inline constexpr std::string_view format_id = "urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128";
inline Error error(Errc code, std::string_view id, std::string detail) {
  return Error::make(code, "ZIP reader rejected input or operation", std::move(detail))
      .in("storage.asset_zip").op("zip.read").id(std::string(id));
}
[[noreturn]] inline void fail(std::string detail) { throw error(Errc::corrupt_data, "am.zip.invalid_structure", std::move(detail)); }
[[noreturn]] inline void overlap(std::string detail) { throw error(Errc::corrupt_data, "am.zip.overlapping_entries", std::move(detail)); }
[[noreturn]] inline void limit(std::string detail) { throw error(Errc::resource_exhausted, "am.zip.limit_exceeded", std::move(detail)); }
[[noreturn]] inline void unsupported(std::string detail) { throw error(Errc::unsupported, "am.zip.unsupported_feature", std::move(detail)); }
[[noreturn]] inline void corrupt(std::string detail) { throw error(Errc::corrupt_data, "am.zip.entry_corrupt", std::move(detail)); }
inline Error oom() { return error(Errc::resource_exhausted, "am.zip.out_of_memory", "Allocation failed"); }
inline U64 end_at(U64 offset, U64 length, U64 bound) {
  if (offset > bound || length > bound - offset) fail("Range outside file: offset=" + std::to_string(offset));
  return offset + length;
}
inline std::uint16_t u16(Bytes b, std::size_t p) {
  if (p > b.size() || b.size() - p < 2) fail("Truncated 16-bit field");
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[p]) | (std::to_integer<unsigned>(b[p + 1]) << 8));
}
inline std::uint32_t u32(Bytes b, std::size_t p) { return static_cast<std::uint32_t>(u16(b, p)) | (static_cast<std::uint32_t>(u16(b, p + 2)) << 16); }
inline U64 u64(Bytes b, std::size_t p) { return u32(b, p) | (static_cast<U64>(u32(b, p + 4)) << 32); }
inline void exact(const RandomAccessSource& source, U64 offset, std::span<std::byte> output, bool entry = false) {
  std::size_t done = 0;
  while (done < output.size()) {
    if (offset > std::numeric_limits<U64>::max() - done) fail("Read offset overflow");
    auto read = source.read_at(offset + done, output.subspan(done));
    if (!read) throw read.error();
    if (*read > output.size() - done) fail("Source violated read span contract");
    if (*read == 0) {
      const auto detail = "Unexpected EOF at offset=" + std::to_string(offset + done);
      if (entry) corrupt(detail); else fail(detail);
    }
    done += *read;
  }
}
inline std::vector<std::byte> bytes(const RandomAccessSource& source, U64 offset, std::size_t length) {
  (void)end_at(offset, length, source.size());
  std::vector<std::byte> out(length);
  exact(source, offset, out);
  return out;
}
inline std::string text(Bytes bytes) {
  return bytes.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
inline std::string entry_detail(const EntryInfo& e) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out = "entry=" + std::to_string(e.index) + " local_offset=" + std::to_string(e.local_offset) +
      " data_offset=" + std::to_string(e.data_offset) + " name_hex=";
  for (unsigned char c : e.raw_name) { out += hex[c >> 4]; out += hex[c & 15]; }
  return out;
}
struct EntryDecoder {
  std::function<Result<std::size_t>(U64, std::span<std::byte>)> read_input;
  std::function<Result<void>(Bytes)> accepted_input;
  std::size_t input_length = 0;
  EntryInfo entry;
  ReadBudget* budget = nullptr;
  U64 compressed_read = 0, produced = 0;
  std::uint32_t crc = 0;
  z_stream inflate_state{};
  bool initialized = false;
  std::array<std::byte, 65536> input{}, output{}; // Buffer sizes, not resource-policy limits.
  std::optional<Sha256> hash;
  std::optional<Error> failure;
  std::optional<EntryVerification> result;
  ~EntryDecoder() { if (initialized) inflateEnd(&inflate_state); }
  void initialize(bool sha256) {
    if (sha256) { auto created = Sha256::create(); if (!created) throw created.error(); hash.emplace(std::move(created).value()); }
    if (entry.method == 8) {
      const auto status = inflateInit2(&inflate_state, -MAX_WBITS);
      if (status == Z_MEM_ERROR) throw oom();
      if (status != Z_OK) corrupt("inflateInit2 failed");
      initialized = true;
    }
  }
  void read_exact(U64 offset, std::span<std::byte> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
      auto n = read_input(offset + done, bytes.subspan(done)); if (!n) throw n.error();
      if (*n > bytes.size() - done) fail("Source violated read span contract");
      if (!*n) corrupt("Unexpected EOF at offset=" + std::to_string(entry.data_offset + offset + done));
      done += *n;
    }
  }
  void finish() {
    if (produced != entry.uncompressed_size || crc != entry.crc32) corrupt("Entry size or CRC mismatch: " + entry_detail(entry));
    EntryVerification verified{produced, crc, {}};
    if (hash) {
      auto digest = hash->finish(); if (!digest) throw digest.error();
      verified.sha256 = *digest;
    }
    result = verified;
  }
  std::size_t read(std::span<std::byte> destination) {
    if (result) return 0;
    if (destination.empty()) throw error(Errc::io_error, "am.zip.io_error", "Output span must be nonempty before EOF");
    const auto expected_left = entry.uncompressed_size - produced;
    if (expected_left != 0 && budget->remaining() == 0) limit("Job read budget exhausted: " + entry_detail(entry));
    auto capacity = static_cast<std::size_t>(std::min<U64>({destination.size(), output.size(), expected_left == 0 ? 1 : expected_left}));
    if (expected_left != 0) capacity = static_cast<std::size_t>(std::min<U64>(capacity, budget->remaining()));
    if (capacity == 0) limit("Job read budget exhausted: " + entry_detail(entry));
    std::size_t count = 0;
    bool ended = false;
    if (entry.method == 0) {
      if (entry.compressed_size != entry.uncompressed_size) corrupt("Stored sizes disagree: " + entry_detail(entry));
      count = static_cast<std::size_t>(std::min<U64>(capacity, entry.compressed_size - compressed_read));
      read_exact(compressed_read, std::span(output).first(count));
      compressed_read += count;
      ended = compressed_read == entry.compressed_size;
    } else {
      for (;;) {
        if (inflate_state.avail_in == 0 && compressed_read < entry.compressed_size) {
          if (accepted_input && input_length) {
            auto accepted = accepted_input(std::span(input).first(input_length));
            if (!accepted) throw accepted.error();
            input_length = 0;
          }
          const auto n = static_cast<std::size_t>(std::min<U64>(input.size(), entry.compressed_size - compressed_read));
          read_exact(compressed_read, std::span(input).first(n));
          input_length = n;
          compressed_read += n;
          inflate_state.next_in = reinterpret_cast<Bytef*>(input.data()); inflate_state.avail_in = static_cast<uInt>(n);
        }
        inflate_state.next_out = reinterpret_cast<Bytef*>(output.data()); inflate_state.avail_out = static_cast<uInt>(capacity);
        const auto old_in = inflate_state.avail_in;
        const auto status = ::inflate(&inflate_state, Z_NO_FLUSH);
        count = capacity - inflate_state.avail_out;
        if (status == Z_MEM_ERROR) throw oom();
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) corrupt("Inflate failed: " + entry_detail(entry));
        if (status == Z_STREAM_END) {
          if (inflate_state.avail_in != 0 || compressed_read != entry.compressed_size) corrupt("Deflate ends before compressed_size: " + entry_detail(entry));
          ended = true;
          break;
        }
        if (count != 0) break;
        if (old_in == inflate_state.avail_in || (inflate_state.avail_in == 0 && compressed_read == entry.compressed_size))
          corrupt("Truncated/non-progressing Deflate stream: " + entry_detail(entry));
      }
    }
    // At the declared end, a single fixed-buffer probe distinguishes EOF from
    // the first excess byte, even when an exactly sufficient budget is empty.
    if (count > expected_left) corrupt("Entry exceeds declared size: " + entry_detail(entry));
    if (!budget->consume(count)) limit("Job read budget exhausted: " + entry_detail(entry));
    produced += count;
    crc = static_cast<std::uint32_t>(::crc32(crc, reinterpret_cast<const Bytef*>(output.data()), static_cast<uInt>(count)));
    if (hash && count) { auto updated = hash->update(std::span(output).first(count)); if (!updated) throw updated.error(); }
    if (ended) finish();
    if (count == 0 && !result) corrupt("Entry did not reach a validated end");
    if (accepted_input) {
      if (entry.method == 0 && count) { auto accepted = accepted_input(std::span(output).first(count)); if (!accepted) throw accepted.error(); }
      else if (entry.method == 8 && inflate_state.avail_in == 0 && input_length && (compressed_read < entry.compressed_size || result)) {
        auto accepted = accepted_input(std::span(input).first(input_length)); if (!accepted) throw accepted.error(); input_length = 0;
      }
    }
    if (count) std::memcpy(destination.data(), output.data(), count);
    return count;
  }
};
}  // namespace am::asset_zip::detail
