#include "am/storage/asset_zip/reader.hpp"
#include "am/storage/asset_zip/probe.hpp"
#include "am/foundation/unicode.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>
#include <set>
#include <zlib.h>

namespace am::asset_zip {
namespace {
using U64 = std::uint64_t;
using Bytes = std::span<const std::byte>;
constexpr std::uint32_t eocd_sig = 0x06054b50, local_sig = 0x04034b50, central_sig = 0x02014b50;
constexpr std::uint32_t locator_sig = 0x07064b50, end64_sig = 0x06064b50, descriptor_sig = 0x08074b50;
constexpr U64 max32 = 0xffffffffULL;
constexpr std::uint16_t encryption_flags = 1 | 64 | 0x2000;
constexpr std::string_view format_id = "urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128";
Error error(Errc code, std::string_view id, std::string detail) {
  return Error::make(code, "ZIP reader rejected input or operation", std::move(detail))
      .in("storage.asset_zip").op("zip.read").id(std::string(id));
}
[[noreturn]] void fail(std::string detail) { throw error(Errc::corrupt_data, "am.zip.invalid_structure", std::move(detail)); }
[[noreturn]] void overlap(std::string detail) { throw error(Errc::corrupt_data, "am.zip.overlapping_entries", std::move(detail)); }
[[noreturn]] void limit(std::string detail) { throw error(Errc::resource_exhausted, "am.zip.limit_exceeded", std::move(detail)); }
[[noreturn]] void unsupported(std::string detail) { throw error(Errc::unsupported, "am.zip.unsupported_feature", std::move(detail)); }
[[noreturn]] void corrupt(std::string detail) { throw error(Errc::corrupt_data, "am.zip.entry_corrupt", std::move(detail)); }
Error oom() { return error(Errc::resource_exhausted, "am.zip.out_of_memory", "Allocation failed"); }
U64 end_at(U64 offset, U64 length, U64 bound) {
  if (offset > bound || length > bound - offset) fail("Range outside file: offset=" + std::to_string(offset));
  return offset + length;
}
std::uint16_t u16(Bytes b, std::size_t p) {
  if (p > b.size() || b.size() - p < 2) fail("Truncated 16-bit field");
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[p]) | (std::to_integer<unsigned>(b[p + 1]) << 8));
}
std::uint32_t u32(Bytes b, std::size_t p) { return static_cast<std::uint32_t>(u16(b, p)) | (static_cast<std::uint32_t>(u16(b, p + 2)) << 16); }
U64 u64(Bytes b, std::size_t p) { return u32(b, p) | (static_cast<U64>(u32(b, p + 4)) << 32); }
void exact(const RandomAccessSource& source, U64 offset, std::span<std::byte> output, bool entry = false) {
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
std::vector<std::byte> bytes(const RandomAccessSource& source, U64 offset, std::size_t length) {
  (void)end_at(offset, length, source.size());
  std::vector<std::byte> out(length);
  exact(source, offset, out);
  return out;
}
std::string text(Bytes bytes) {
  return bytes.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
std::string entry_detail(const EntryInfo& e) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out = "entry=" + std::to_string(e.index) + " local_offset=" + std::to_string(e.local_offset) +
      " data_offset=" + std::to_string(e.data_offset) + " name_hex=";
  for (unsigned char c : e.raw_name) { out += hex[c >> 4]; out += hex[c & 15]; }
  return out;
}
struct EndSearch { std::optional<U64> offset; bool signature_seen = false; };
EndSearch find_end(const RandomAccessSource& source) {
  const auto count = static_cast<std::size_t>(std::min<U64>(source.size(), 22 + 65535));
  const auto start = source.size() - count;
  const auto tail = bytes(source, start, count);
  EndSearch result;
  for (std::size_t p = tail.size(); p-- > 0;) {
    if (tail.size() - p < 4 || u32(tail, p) != eocd_sig) continue;
    result.signature_seen = true;
    if (tail.size() - p >= 22 && static_cast<std::size_t>(u16(tail, p + 20)) == tail.size() - p - 22) {
      result.offset = start + p;
      return result;
    }
  }
  return result;
}
// ZIP64 fields occur only for saturated base fields, in APPNOTE 4.5.3 order.
bool extras(Bytes data, U64& usize, U64& csize, U64& offset, std::uint32_t& disk) {
  const bool need_u = usize == max32, need_c = csize == max32, need_o = offset == max32, need_d = disk == 0xffff;
  bool seen = false;
  for (std::size_t p = 0; p < data.size();) {
    if (data.size() - p < 4) fail("Truncated extra-field header");
    const auto id = u16(data, p), size = u16(data, p + 2); p += 4;
    if (size > data.size() - p) fail("Extra field exceeds declared length");
    const auto payload = data.subspan(p, size); p += size;
    if (id != 1) continue;
    if (seen) fail("Duplicate ZIP64 extra field");
    seen = true;
    std::size_t cursor = 0;
    auto read64 = [&] { const auto value = u64(payload, cursor); cursor += 8; return value; };
    if (need_u) usize = read64();
    if (need_c) csize = read64();
    if (need_o) offset = read64();
    if (need_d) { disk = u32(payload, cursor); cursor += 4; }
    if (cursor != payload.size()) fail("Unexpected ZIP64 extra-field values");
  }
  if ((need_u || need_c || need_o || need_d) && !seen) fail("Missing required ZIP64 extra field");
  return seen;
}
struct Region { U64 begin, end; };
struct Parsed { std::vector<EntryInfo> entries; ArchiveSummary summary; };
Parsed parse_archive(const RandomAccessSource& source, const ZipLimits& limits) {
  const auto search = find_end(source);
  if (!search.offset) fail(search.signature_seen ? "No EOCD with exact comment boundary" : "EOCD not found");
  const auto end_offset = *search.offset;
  const auto end = bytes(source, end_offset, 22);
  const auto disk16 = u16(end, 4), cd_disk16 = u16(end, 6), count_disk16 = u16(end, 8), count16 = u16(end, 10);
  U64 count = count16, cd_size = u32(end, 12), cd_offset = u32(end, 16);
  Parsed parsed;
  std::vector<bool> central_wide_sizes;
  parsed.summary.comment_length = u16(end, 20);
  std::vector<Region> metadata{{end_offset, source.size()}};
  U64 metadata_begin = end_offset;
  const bool zip64 = disk16 == 0xffff || cd_disk16 == 0xffff || count_disk16 == 0xffff || count16 == 0xffff || cd_size == max32 || cd_offset == max32;
  if (zip64) {
    if (end_offset < 20) fail("Missing ZIP64 locator");
    const auto locator_offset = end_offset - 20;
    const auto locator = bytes(source, locator_offset, 20);
    if (u32(locator, 0) != locator_sig) fail("Invalid ZIP64 locator signature");
    if (u32(locator, 4) != 0 || u32(locator, 16) != 1) unsupported("Multi-disk ZIP64 locator");
    const auto record_offset = u64(locator, 8);
    const auto record = bytes(source, record_offset, 56);
    if (u32(record, 0) != end64_sig || u64(record, 4) < 44) fail("Invalid ZIP64 EOCD record");
    const auto record_length = u64(record, 4);
    if (record_offset > locator_offset || locator_offset - record_offset < 12 || record_length > locator_offset - record_offset - 12)
      fail("ZIP64 record overlaps locator");
    if (u32(record, 16) != 0 || u32(record, 20) != 0 || (disk16 != 0 && disk16 != 0xffff) || (cd_disk16 != 0 && cd_disk16 != 0xffff))
      unsupported("Multi-disk ZIP64 archive");
    count = u64(record, 32); cd_size = u64(record, 40); cd_offset = u64(record, 48);
    if (u64(record, 24) != count) unsupported("ZIP64 entries span disks");
    if ((count16 != 0xffff && count16 != count) || (count_disk16 != 0xffff && count_disk16 != count) ||
        (u32(end, 12) != max32 && u32(end, 12) != cd_size) || (u32(end, 16) != max32 && u32(end, 16) != cd_offset))
      fail("EOCD and ZIP64 values disagree");
    metadata.push_back({record_offset, record_offset + 12 + record_length});
    metadata.push_back({locator_offset, end_offset});
    metadata_begin = record_offset;
  } else {
    if (disk16 != 0 || cd_disk16 != 0 || count_disk16 != count16) unsupported("Multi-disk archive");
  }
  // Check metadata budgets before reserving/reading count-dependent structures.
  if (count > limits.max_entries || count > std::numeric_limits<std::size_t>::max()) limit("Entry count exceeds hypothesis max_entries");
  if (cd_size > limits.max_directory_bytes || cd_size > std::numeric_limits<std::size_t>::max()) limit("Directory exceeds hypothesis max_directory_bytes");
  const auto cd_end = end_at(cd_offset, cd_size, source.size());
  if (cd_end > metadata_begin || count > cd_size / 46) fail("Central directory boundary/count is inconsistent");
  parsed.summary = {count, cd_offset, cd_size, 0, zip64, parsed.summary.comment_length};
  if (cd_size) metadata.push_back({cd_offset, cd_end});
  const auto directory = bytes(source, cd_offset, static_cast<std::size_t>(cd_size));
  parsed.entries.reserve(static_cast<std::size_t>(count));
  std::size_t cursor = 0;
  for (U64 i = 0; i < count; ++i) {
    if (cursor > directory.size() || directory.size() - cursor < 46) fail("Truncated central directory entry");
    const auto header = Bytes(directory).subspan(cursor, 46);
    if (u32(header, 0) != central_sig) fail("Invalid central directory signature");
    const auto name_length = u16(header, 28), extra_length = u16(header, 30), comment_length = u16(header, 32);
    if (name_length > limits.max_name_bytes) limit("Entry name exceeds hypothesis max_name_bytes");
    const auto variable_length = static_cast<std::size_t>(name_length) + extra_length + comment_length;
    cursor += 46;
    if (variable_length > directory.size() - cursor) fail("Central directory variable fields exceed directory");
    EntryInfo entry;
    entry.index = parsed.entries.size(); entry.version_made_by = u16(header, 4); entry.flags = u16(header, 8); entry.method = u16(header, 10);
    entry.crc32 = u32(header, 16); entry.compressed_size = u32(header, 20); entry.uncompressed_size = u32(header, 24);
    central_wide_sizes.push_back(entry.compressed_size == max32 || entry.uncompressed_size == max32);
    entry.external_attributes = u32(header, 38); entry.local_offset = u32(header, 42);
    entry.raw_name = text(Bytes(directory).subspan(cursor, name_length));
    const auto extra = Bytes(directory).subspan(cursor + name_length, extra_length);
    entry.central_extra.assign(extra.begin(), extra.end());
    std::uint32_t start_disk = u16(header, 34);
    entry.has_zip64 = extras(extra, entry.uncompressed_size, entry.compressed_size, entry.local_offset, start_disk);
    if (start_disk != 0) unsupported("Entry starts on another disk");
    if (entry.uncompressed_size > limits.max_entry_bytes) limit("Entry size exceeds hypothesis max_entry_bytes");
    entry.encrypted = (entry.flags & encryption_flags) != 0 || entry.method == 99;
    entry.data_descriptor = (entry.flags & 8) != 0; entry.utf8_flag = (entry.flags & 0x800) != 0;
    if (entry.utf8_flag && unicode::is_valid_utf8(entry.raw_name)) entry.utf8_name = entry.raw_name;
    const auto mode = (entry.external_attributes >> 16) & 0xf000;
    if ((entry.version_made_by >> 8) == 3 && mode == 0xa000) entry.kind = EntryKind::symlink;
    else if (entry.raw_name.ends_with('/')) entry.kind = EntryKind::directory;
    else if ((entry.version_made_by >> 8) == 3 && mode != 0 && mode != 0x8000) entry.kind = EntryKind::special;
    parsed.summary.zip64 = parsed.summary.zip64 || entry.has_zip64;
    parsed.entries.push_back(std::move(entry));
    cursor += variable_length;
  }
  if (cursor != directory.size()) fail("Central directory size/count mismatch");
  std::vector<std::size_t> order(parsed.entries.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(), [&](auto a, auto b) { return parsed.entries[a].local_offset < parsed.entries[b].local_offset; });
  for (std::size_t i = 1; i < order.size(); ++i)
    if (parsed.entries[order[i - 1]].local_offset == parsed.entries[order[i]].local_offset) overlap("Entries share one local header");
  std::set<std::string_view> names;
  for (const auto& entry : parsed.entries)
    if (!names.insert(entry.raw_name).second) throw error(Errc::corrupt_data, "am.zip.duplicate_name", entry_detail(entry));
  for (std::size_t rank = 0; rank < order.size(); ++rank) {
    auto& entry = parsed.entries[order[rank]];
    const auto boundary = rank + 1 < order.size() ? parsed.entries[order[rank + 1]].local_offset : cd_offset;
    (void)end_at(entry.local_offset, 30, source.size());
    if (entry.local_offset > cd_offset || cd_offset - entry.local_offset < 30) overlap("Local header overlaps metadata");
    const auto local = bytes(source, entry.local_offset, 30);
    if (u32(local, 0) != local_sig) fail("Invalid local header signature: " + entry_detail(entry));
    const auto name_length = u16(local, 26), extra_length = u16(local, 28);
    if (name_length > limits.max_name_bytes) limit("Local name exceeds hypothesis max_name_bytes");
    const auto variable_length = static_cast<std::size_t>(name_length) + extra_length;
    entry.data_offset = end_at(entry.local_offset, 30 + variable_length, source.size());
    const auto data_end = end_at(entry.data_offset, entry.compressed_size, source.size());
    if (entry.data_offset > boundary || data_end > boundary || data_end > cd_offset) overlap("Entry range overlaps another record: " + entry_detail(entry));
    const auto variable = bytes(source, entry.local_offset + 30, variable_length);
    if (text(Bytes(variable).first(name_length)) != entry.raw_name || u16(local, 8) != entry.method ||
        (u16(local, 6) & encryption_flags) != (entry.flags & encryption_flags))
      fail("Local/central name, method or encryption flags disagree: " + entry_detail(entry));
    const auto extra = Bytes(variable).subspan(name_length, extra_length);
    entry.local_extra.assign(extra.begin(), extra.end());
    U64 local_u = u32(local, 22), local_c = u32(local, 18), unused_offset = 0;
    std::uint32_t unused_disk = 0;
    const bool sizes64 = central_wide_sizes[entry.index] || local_u == max32 || local_c == max32 ||
        entry.uncompressed_size >= max32 || entry.compressed_size >= max32;
    entry.has_zip64 = extras(extra, local_u, local_c, unused_offset, unused_disk) || entry.has_zip64;
    parsed.summary.zip64 = parsed.summary.zip64 || entry.has_zip64;
    if (!entry.data_descriptor && (u32(local, 14) != entry.crc32 || local_u != entry.uncompressed_size || local_c != entry.compressed_size))
      fail("Local/central CRC or sizes disagree: " + entry_detail(entry));
    entry.record_end = data_end;
    if (entry.data_descriptor) {
      const std::size_t base = sizes64 ? 20 : 12;
      const auto available = boundary - data_end;
      if (available < base) fail("Missing/truncated data descriptor: " + entry_detail(entry));
      const auto descriptor = bytes(source, data_end, static_cast<std::size_t>(std::min<U64>(available, base + 4)));
      auto matches = [&](std::size_t start) {
        if (descriptor.size() < start + base || u32(descriptor, start) != entry.crc32) return false;
        const auto compressed = sizes64 ? u64(descriptor, start + 4) : u32(descriptor, start + 4);
        const auto unpacked = sizes64 ? u64(descriptor, start + 12) : u32(descriptor, start + 8);
        return compressed == entry.compressed_size && unpacked == entry.uncompressed_size;
      };
      if (u32(descriptor, 0) == descriptor_sig && matches(4)) entry.record_end += base + 4;
      else if (matches(0)) entry.record_end += base;
      else fail("Data descriptor disagrees with central directory: " + entry_detail(entry));
    }
    metadata.push_back({entry.local_offset, entry.record_end});
  }
  std::sort(metadata.begin(), metadata.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
  U64 last = 0, gaps = 0;
  for (const auto& region : metadata) {
    if (region.begin < last) overlap("ZIP record ranges overlap");
    gaps += region.begin - last;
    last = region.end;
  }
  parsed.summary.unaccounted_bytes = gaps;
  return parsed;
}
}  // namespace
struct ZipArchive::Impl {
  std::shared_ptr<const RandomAccessSource> source;
  ZipLimits limits;
  Parsed parsed;
};
ZipArchive::ZipArchive(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
Result<ZipArchive> ZipArchive::open(std::shared_ptr<const RandomAccessSource> source, const ZipLimits& limits) {
  try {
    if (!source) return error(Errc::io_error, "am.zip.io_error", "Null ZIP source");
    auto impl = std::make_shared<Impl>();
    impl->source = std::move(source); impl->limits = limits;
    impl->parsed = parse_archive(*impl->source, limits);
    return ZipArchive(std::move(impl));
  } catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return error(Errc::io_error, "am.zip.io_error", e.what()); }
}
std::span<const EntryInfo> ZipArchive::entries() const noexcept { return impl_->parsed.entries; }
const ArchiveSummary& ZipArchive::summary() const noexcept { return impl_->parsed.summary; }
Result<ByteRange> ZipArchive::raw_range(std::size_t index) const {
  try {
    if (index >= entries().size()) return error(Errc::io_error, "am.zip.io_error", "Invalid entry index");
    const auto& e = entries()[index];
    return ByteRange{e.data_offset, e.compressed_size};
  } catch (const std::bad_alloc&) { return oom(); }
}
bool ReadBudget::consume(U64 bytes) noexcept {
  auto left = remaining_.load();
  do { if (bytes > left) return false; } while (!remaining_.compare_exchange_weak(left, left - bytes));
  return true;
}
struct EntryStream::Impl {
  std::shared_ptr<const RandomAccessSource> source;
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
  ~Impl() { if (initialized) inflateEnd(&inflate_state); }
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
      exact(*source, entry.data_offset + compressed_read, std::span(output).first(count), true);
      compressed_read += count;
      ended = compressed_read == entry.compressed_size;
    } else {
      for (;;) {
        if (inflate_state.avail_in == 0 && compressed_read < entry.compressed_size) {
          const auto n = static_cast<std::size_t>(std::min<U64>(input.size(), entry.compressed_size - compressed_read));
          exact(*source, entry.data_offset + compressed_read, std::span(input).first(n), true);
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
    if (count) std::memcpy(destination.data(), output.data(), count);
    return count;
  }
};
EntryStream::EntryStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
EntryStream::EntryStream(EntryStream&&) noexcept = default;
EntryStream& EntryStream::operator=(EntryStream&&) noexcept = default;
EntryStream::~EntryStream() = default;
Result<EntryStream> ZipArchive::open_entry(std::size_t index, ReadBudget& budget, bool sha256) const {
  try {
    if (index >= entries().size()) return error(Errc::io_error, "am.zip.io_error", "Invalid entry index");
    const auto& e = entries()[index];
    if (e.encrypted || (e.method != 0 && e.method != 8)) unsupported("Cannot read encryption/compression method: " + entry_detail(e));
    if (e.uncompressed_size > impl_->limits.max_entry_bytes) limit("Entry exceeds hypothesis max_entry_bytes");
    if (e.uncompressed_size != 0 && budget.remaining() == 0) limit("Job read budget exhausted: " + entry_detail(e));
    auto stream = std::make_unique<EntryStream::Impl>();
    stream->source = impl_->source; stream->entry = e; stream->budget = &budget;
    if (sha256) { auto hash = Sha256::create(); if (!hash) return hash.error(); stream->hash.emplace(std::move(hash).value()); }
    if (e.method == 8) {
      const auto status = inflateInit2(&stream->inflate_state, -MAX_WBITS);
      if (status == Z_MEM_ERROR) return oom();
      if (status != Z_OK) corrupt("inflateInit2 failed");
      stream->initialized = true;
    }
    return EntryStream(std::move(stream));
  } catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return error(Errc::io_error, "am.zip.io_error", e.what()); }
}
Result<std::size_t> EntryStream::read(std::span<std::byte> output) {
  try {
    if (impl_->failure) return *impl_->failure;
    return impl_->read(output);
  } catch (const Error& e) { impl_->failure = e; return e; }
  catch (const std::bad_alloc&) { impl_->failure = oom(); return *impl_->failure; }
  catch (const std::exception& e) { impl_->failure = error(Errc::io_error, "am.zip.io_error", e.what()); return *impl_->failure; }
}
std::optional<EntryVerification> EntryStream::verification() const noexcept { return impl_->result; }
Result<EntryVerification> ZipArchive::verify_entry(std::size_t index, ReadBudget& budget, bool sha256) const {
  try {
    auto stream = open_entry(index, budget, sha256); if (!stream) return stream.error();
    std::array<std::byte, 65536> buffer{};
    for (;;) { auto count = stream->read(buffer); if (!count) return count.error(); if (*count == 0) break; }
    return *stream->verification();
  } catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return error(Errc::io_error, "am.zip.io_error", e.what()); }
}
namespace {
Error probe_error(const Error& cause) {
  return Error::make(cause.code, "Container probe failed", cause.ctx.detail)
      .in("storage.asset_zip").op("zip.probe").id("am.zip.entry_corrupt").caused_by(cause);
}
bool first_has_marker(const ZipArchive& archive, U64 cap) {
  if (archive.entries().empty()) return false;
  const auto first = std::min_element(archive.entries().begin(), archive.entries().end(), [](const auto& a, const auto& b) { return a.local_offset < b.local_offset; });
  ReadBudget budget(cap);
  auto stream = archive.open_entry(first->index, budget);
  if (!stream) return false;
  std::array<std::byte, 65536> buffer{};
  std::string tail;
  while (budget.remaining()) {
    auto n = stream->read(buffer); if (!n || *n == 0) return false;
    tail.append(reinterpret_cast<const char*>(buffer.data()), *n);
    if (tail.find(format_id) != std::string::npos) return true;
    if (tail.size() >= format_id.size()) tail.erase(0, tail.size() - format_id.size() + 1);
  }
  return false;
}
}
Result<ProbeResult> probe_container(std::shared_ptr<const RandomAccessSource> source, const ZipLimits& limits, const manifest::CodecLimits& codec_limits) {
  try {
    if (!source) return error(Errc::io_error, "am.zip.io_error", "Null ZIP source");
    const auto search = find_end(*source);
    if (!search.signature_seen) return ProbeResult{ProbeOutcome::not_zip, {}, {}};
    auto archive = ZipArchive::open(source, limits);
    if (!archive) {
      auto outer = Error::make(archive.error().code, "ZIP mechanism rejected archive", archive.error().ctx.detail)
          .in("storage.asset_zip").op("zip.probe").id(archive.error().message_id).caused_by(archive.error());
      return ProbeResult{ProbeOutcome::invalid_zip, {}, std::move(outer)};
    }
    const auto entry = std::find_if(archive->entries().begin(), archive->entries().end(), [](const auto& e) { return e.raw_name == "manifest.json"; });
    if (entry == archive->entries().end()) return ProbeResult{ProbeOutcome::foreign_zip, {}, {}};
    std::optional<Error> failure;
    std::string json;
    if (entry->uncompressed_size > codec_limits.max_bytes) failure = error(Errc::resource_exhausted, "am.zip.limit_exceeded", "Manifest exceeds CodecLimits::max_bytes");
    else {
      ReadBudget budget(codec_limits.max_bytes);
      auto stream = archive->open_entry(entry->index, budget);
      if (!stream) failure = stream.error();
      else {
        std::array<std::byte, 65536> buffer{};
        for (;;) {
          auto n = stream->read(buffer);
          if (!n) { failure = n.error(); break; }
          if (*n == 0) break;
          json.append(reinterpret_cast<const char*>(buffer.data()), *n);
        }
      }
    }
    if (failure) return ProbeResult{first_has_marker(*archive, codec_limits.max_bytes) ? ProbeOutcome::own_corrupt : ProbeOutcome::invalid_zip, {}, probe_error(*failure)};
    auto document = manifest::parse_manifest(json, codec_limits);
    if (document) {
      const auto& dom = document->known();
      const auto format = dom.is_object() ? dom.find("format") : dom.end();
      if (format != dom.end() && format->is_object()) {
        const auto id = format->find("id");
        const auto version = format->find("version");
        if (id != format->end() && id->is_string() && id->get_ref<const std::string&>() != format_id)
          return ProbeResult{ProbeOutcome::foreign_zip, {}, {}};
        if (id != format->end() && id->is_string() && id->get_ref<const std::string&>() == format_id && version != format->end() && version->is_object()) {
          const auto major = version->find("major"), minor = version->find("minor");
          if (major != version->end() && major->is_number_integer()) {
            FormatVersion value{major->dump(), minor != version->end() && minor->is_number_integer() ? minor->dump() : std::string{}};
            return ProbeResult{value.major == "1" ? ProbeOutcome::own : ProbeOutcome::own_unsupported_version, std::move(value), {}};
          }
        }
      }
      failure = error(Errc::corrupt_data, "am.zip.entry_corrupt", "Manifest format fields are not readable");
    } else failure = document.error();
    return ProbeResult{first_has_marker(*archive, codec_limits.max_bytes) ? ProbeOutcome::own_corrupt : ProbeOutcome::foreign_zip, {}, probe_error(*failure)};
  } catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return error(Errc::io_error, "am.zip.io_error", e.what()); }
}
}  // namespace am::asset_zip
