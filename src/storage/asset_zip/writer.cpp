#include "am/storage/asset_zip/writer.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/unicode.hpp"
#include "writer_internal.hpp"
#include <set>

namespace am::asset_zip {
using namespace detail;
namespace {
struct Record {
  WrittenEntry entry;
  DosTime dos;
  bool local64 = false;
  std::vector<std::byte> local_extra, central_extra, comment;
};
std::vector<std::byte> local_header(const Record& r) {
  const auto& e = r.entry;
  std::vector<std::byte> extra;
  if (r.local64) { append(extra, 1, 2); append(extra, 16, 2); append(extra, e.uncompressed_size, 8); append(extra, e.compressed_size, 8); }
  append_bytes(extra, r.local_extra);
  if (extra.size() > 65535) write_fail(Errc::unsupported, "am.zip.write.extra_not_preserved", "Local extra fields exceed ZIP length field");
  std::vector<std::byte> out(30);
  put(out, 0, local_sig, 4); put(out, 4, r.local64 || e.local_offset >= max32 ? 45 : 20, 2); put(out, 6, e.flags, 2); put(out, 8, static_cast<std::uint16_t>(e.method), 2);
  put(out, 10, r.dos.time, 2); put(out, 12, r.dos.date, 2); put(out, 14, e.crc32, 4);
  put(out, 18, r.local64 ? max32 : e.compressed_size, 4); put(out, 22, r.local64 ? max32 : e.uncompressed_size, 4);
  put(out, 26, e.name.size(), 2); put(out, 28, extra.size(), 2);
  append_name(out, e.name); append_bytes(out, extra); return out;
}
std::vector<std::byte> central_record(const Record& r) {
  const auto& e = r.entry;
  std::vector<std::byte> fields;
  if (e.uncompressed_size >= max32) append(fields, e.uncompressed_size, 8);
  if (e.compressed_size >= max32) append(fields, e.compressed_size, 8);
  if (e.local_offset >= max32) append(fields, e.local_offset, 8);
  std::vector<std::byte> extra;
  if (!fields.empty()) { append(extra, 1, 2); append(extra, fields.size(), 2); append_bytes(extra, fields); }
  append_bytes(extra, r.central_extra);
  if (extra.size() > 65535 || r.comment.size() > 65535) write_fail(Errc::unsupported, "am.zip.write.extra_not_preserved", "Central extra/comment exceeds ZIP length field");
  std::vector<std::byte> out(46);
  put(out, 0, central_sig, 4); put(out, 4, 45, 2); put(out, 6, r.local64 || !fields.empty() ? 45 : 20, 2);
  put(out, 8, e.flags, 2); put(out, 10, static_cast<std::uint16_t>(e.method), 2); put(out, 12, r.dos.time, 2); put(out, 14, r.dos.date, 2);
  put(out, 16, e.crc32, 4); put(out, 20, std::min(e.compressed_size, max32), 4); put(out, 24, std::min(e.uncompressed_size, max32), 4);
  put(out, 28, e.name.size(), 2); put(out, 30, extra.size(), 2); put(out, 32, r.comment.size(), 2);
  put(out, 42, std::min(e.local_offset, max32), 4);
  append_name(out, e.name); append_bytes(out, extra); append_bytes(out, r.comment); return out;
}
std::vector<std::byte> carried_extra(Bytes source, const RawCopyOptions& options,
                                    std::set<std::uint16_t>& dropped_ids) {
  std::vector<std::byte> out;
  for (std::size_t p = 0; p < source.size();) {
    const auto start = p;
    const auto id = u16(source, p), size = u16(source, p + 2); p += 4;
    if (size > source.size() - p) fail("Invalid source extra length");
    p += size;
    if (id == 1) continue;
    if (options.target_name && id == 0x7075) { dropped_ids.insert(id); continue; }
    if (std::find(options.carry_extra_ids.begin(), options.carry_extra_ids.end(), id) == options.carry_extra_ids.end()) {
      if (options.drop_unlisted) {
        dropped_ids.insert(id);
        continue;
      }
      write_fail(Errc::unsupported, "am.zip.write.extra_not_preserved", "Extra id=" + std::to_string(id) + " is not authorized for transfer");
    }
    append_bytes(out, source.subspan(start, p - start));
  }
  return out;
}
}
struct ZipWriter::Impl {
  std::shared_ptr<RandomAccessSink> sink;
  WriterOptions options;
  std::vector<Record> records;
  std::set<std::string> keys;
  std::optional<Error> failure;
  U64 position = 0;
  bool open = false, finished = false, aborted = false, discarded = false, zip64 = false;
  void check() const { if (failure) throw *failure; if (finished || aborted) throw invalid_state(); }
  Error fatal(Error e) {
    if (failure) return *failure;
    failure = std::move(e);
    open = false;
    try { auto result = sink->discard(); discarded = result.ok(); } catch (...) { /* Preserve the original error during cleanup. */ }
    return *failure;
  }
  void emit_at(U64 offset, Bytes data) {
    if (offset > options.limits.max_file_size || data.size() > options.limits.max_file_size - offset)
      write_fail(Errc::resource_exhausted, "am.zip.write.file_too_large", "Output range exceeds max_file_size at offset=" + std::to_string(offset));
    auto written = sink->write_at(offset, data); if (!written) throw written.error();
  }
  void emit(Bytes bytes) { emit_at(position, bytes); position += bytes.size(); }
  void validate_name(std::string_view name, U64 size) {
    if (name.empty() || name.size() > paths::ZipPathMapper::kMaxZipPathBytes)
      write_fail(Errc::invalid_argument, "am.zip.write.invalid_name", "Path is empty or exceeds the container path byte limit");
    const auto key = unicode::path_collision_key(name);
    if (key.empty() && !name.empty()) throw write_oom();
    if (keys.contains(key)) write_fail(Errc::invalid_argument, "am.zip.write.name_collision", "Container path collides with an earlier entry");
    auto valid = paths::check_container_path(name);
    if (!valid) {
      if (valid.error().code == Errc::resource_exhausted) throw write_oom();
      throw write_error(Errc::invalid_argument, "am.zip.write.invalid_name", valid.error().ctx.detail);
    }
    if (records.size() >= options.zip_limits.max_entries || name.size() > options.zip_limits.max_name_bytes || size > options.zip_limits.max_entry_bytes)
      limit("Entry exceeds configured ZipLimits");
  }
  void start(Record& r) {
    r.entry.index = records.size(); r.entry.local_offset = position;
    const auto header = local_header(r);
    r.entry.data_offset = position + header.size();
    // Validate variable fields before any bytes of this entry are emitted.
    (void)central_record(r);
    auto key = unicode::path_collision_key(r.entry.name);
    if (key.empty()) throw write_oom();
    keys.insert(std::move(key)); open = true; zip64 = zip64 || r.local64 || position >= max32;
    emit(header);
  }
  WrittenEntry complete(Record r) {
    const auto header = local_header(r);
    emit_at(r.entry.local_offset, header);
    WrittenEntry written = r.entry;
    records.push_back(std::move(r)); open = false; return written;
  }
};
struct EntryWriter::Impl {
  std::shared_ptr<ZipWriter::Impl> writer;
  Record record;
  z_stream stream{};
  bool initialized = false, finished = false;
  U64 received = 0;
  std::uint32_t crc = 0;
  std::array<std::byte, 65536> output{};
  std::optional<Sha256> hash;
  ~Impl() { if (initialized) deflateEnd(&stream); }
  void check() const { writer->check(); if (finished || !writer->open) throw invalid_state(); }
  void compress(Bytes bytes, bool final) {
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(bytes.data())); stream.avail_in = static_cast<uInt>(bytes.size());
    int status = Z_OK;
    do {
      stream.next_out = reinterpret_cast<Bytef*>(output.data()); stream.avail_out = static_cast<uInt>(output.size());
      status = deflate(&stream, final ? Z_FINISH : Z_NO_FLUSH);
      if (status == Z_MEM_ERROR) throw write_oom();
      if (status == Z_BUF_ERROR && !final && stream.avail_in == 0) break;
      if (status != Z_OK && status != Z_STREAM_END) write_fail(Errc::io_error, "am.zip.io_error", "deflate failed");
      const auto count = output.size() - stream.avail_out;
      if (count) writer->emit(std::span(output).first(count));
    } while (final ? status != Z_STREAM_END : stream.avail_in != 0 || stream.avail_out == 0);
  }
};
ZipWriter::ZipWriter(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
ZipWriter::ZipWriter(ZipWriter&&) noexcept = default;
ZipWriter& ZipWriter::operator=(ZipWriter&& other) noexcept {
  if (this != &other) { if (impl_ && !impl_->finished) { auto ignored = abort(); (void)ignored; } impl_ = std::move(other.impl_); }
  return *this;
}
ZipWriter::~ZipWriter() { if (impl_ && !impl_->finished) { auto ignored = abort(); (void)ignored; } }
EntryWriter::EntryWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
EntryWriter::EntryWriter(EntryWriter&&) noexcept = default;
EntryWriter& EntryWriter::operator=(EntryWriter&& other) noexcept {
  if (this != &other) {
    if (impl_ && !impl_->finished && !impl_->writer->failure && !impl_->writer->aborted) {
      try { impl_->writer->fatal(invalid_state()); } catch (...) { }
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}
EntryWriter::~EntryWriter() {
  if (impl_ && !impl_->finished && !impl_->writer->failure && !impl_->writer->aborted) {
    try { impl_->writer->fatal(invalid_state()); } catch (...) { }
  }
}
Result<ZipWriter> ZipWriter::create(std::shared_ptr<RandomAccessSink> sink, const WriterOptions& options) {
  try {
    if (!sink || sink->size() != 0 || options.deflate_level < 0 || options.deflate_level > 9) return invalid_state();
    auto impl = std::make_shared<Impl>(); impl->sink = std::move(sink); impl->options = options; return ZipWriter(std::move(impl));
  } catch (const std::bad_alloc&) { return write_oom(); }
  catch (const std::exception& e) { return write_error(Errc::io_error, "am.zip.io_error", e.what()); }
}
Result<EntryWriter> ZipWriter::begin_entry(std::string_view name, const EntryOptions& options) {
  bool began = false;
  try {
    impl_->check(); if (impl_->open) return invalid_state();
    impl_->validate_name(name, options.uncompressed_size);
    if (options.method != CompressionMethod::stored && options.method != CompressionMethod::deflate) unsupported("Unsupported writer compression method");
    auto entry = std::make_unique<EntryWriter::Impl>(); entry->writer = impl_;
    auto& r = entry->record; r.entry.name = name; r.entry.method = options.method; r.entry.uncompressed_size = options.uncompressed_size;
    r.dos = options.dos_time.value_or(impl_->options.dos_time);
    if (options.sha256) { auto hash = Sha256::create(); if (!hash) throw hash.error(); entry->hash.emplace(std::move(hash).value()); }
    U64 bound = options.uncompressed_size;
    if (options.method == CompressionMethod::deflate) {
      const auto status = deflateInit2(&entry->stream, impl_->options.deflate_level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
      if (status == Z_MEM_ERROR) throw write_oom();
      if (status != Z_OK) throw invalid_state();
      entry->initialized = true;
      // z_size_t is 64-bit on this target; avoid uLong truncation on Windows.
      if (bound < max32) bound = deflateBound_z(&entry->stream, static_cast<z_size_t>(bound));
    }
    r.local64 = options.uncompressed_size >= max32 || bound >= max32;
    began = true; impl_->start(r);
    return EntryWriter(std::move(entry));
  } catch (const Error& e) {
    if (!began && (e.code == Errc::invalid_argument || e.code == Errc::unsupported)) return e;
    return impl_->fatal(e);
  } catch (const std::bad_alloc&) { return impl_->fatal(write_oom()); }
  catch (const std::exception& e) { return impl_->fatal(write_error(Errc::io_error, "am.zip.io_error", e.what())); }
}
Result<void> EntryWriter::write(Bytes bytes) {
  bool began = false;
  try {
    impl_->check();
    began = true;
    if (bytes.size() > impl_->record.entry.uncompressed_size - impl_->received)
      write_fail(Errc::conflict, "am.zip.write.size_mismatch", "Input exceeds declared uncompressed_size");
    while (!bytes.empty()) {
      const auto chunk = bytes.first(std::min<std::size_t>(bytes.size(), impl_->output.size()));
      impl_->crc = static_cast<std::uint32_t>(::crc32(impl_->crc, reinterpret_cast<const Bytef*>(chunk.data()), static_cast<uInt>(chunk.size())));
      if (impl_->hash) { auto updated = impl_->hash->update(chunk); if (!updated) throw updated.error(); }
      if (impl_->record.entry.method == CompressionMethod::stored) impl_->writer->emit(chunk); else impl_->compress(chunk, false);
      impl_->received += chunk.size(); bytes = bytes.subspan(chunk.size());
    }
    return {};
  } catch (const Error& e) { if (!began && e.message_id == "am.zip.write.invalid_state") return e; return impl_->writer->fatal(e); }
  catch (const std::bad_alloc&) { return impl_->writer->fatal(write_oom()); }
  catch (const std::exception& e) { return impl_->writer->fatal(write_error(Errc::io_error, "am.zip.io_error", e.what())); }
}
Result<WrittenEntry> EntryWriter::finish() {
  bool began = false;
  try {
    impl_->check();
    began = true;
    if (impl_->received != impl_->record.entry.uncompressed_size)
      write_fail(Errc::conflict, "am.zip.write.size_mismatch", "Input ended before declared uncompressed_size");
    if (impl_->record.entry.method == CompressionMethod::deflate) impl_->compress({}, true);
    auto& e = impl_->record.entry;
    e.compressed_size = impl_->writer->position - e.data_offset; e.crc32 = impl_->crc;
    if (impl_->hash) { auto digest = impl_->hash->finish(); if (!digest) throw digest.error(); e.sha256 = *digest; }
    auto written = impl_->writer->complete(std::move(impl_->record)); impl_->finished = true; return written;
  } catch (const Error& e) { if (!began && e.message_id == "am.zip.write.invalid_state") return e; return impl_->writer->fatal(e); }
  catch (const std::bad_alloc&) { return impl_->writer->fatal(write_oom()); }
  catch (const std::exception& e) { return impl_->writer->fatal(write_error(Errc::io_error, "am.zip.io_error", e.what())); }
}
Result<WrittenEntry> ZipWriter::copy_raw(const ZipArchive& archive, std::size_t index, ReadBudget& budget, const RawCopyOptions& options) {
  bool started = false;
  try {
    impl_->check(); if (impl_->open) return invalid_state();
    if (index >= archive.entries().size()) return write_error(Errc::io_error, "am.zip.io_error", "Invalid raw-copy entry index");
    const auto& source = archive.entries()[index];
    const auto& name = options.target_name ? *options.target_name : source.raw_name;
    impl_->validate_name(name, source.uncompressed_size);
    if (options.target_name && std::find(options.carry_extra_ids.begin(), options.carry_extra_ids.end(), 0x7075) != options.carry_extra_ids.end())
      return write_error(Errc::invalid_argument, "am.zip.write.invalid_name", "Unicode Path extra cannot accompany raw-copy renaming");
    if (source.encrypted || (source.method != 0 && source.method != 8)) unsupported("Raw-copy compression/encryption cannot be verified");
    Record r; r.entry.name = name; r.entry.method = static_cast<CompressionMethod>(source.method);
    r.entry.uncompressed_size = source.uncompressed_size; r.entry.compressed_size = source.compressed_size; r.entry.crc32 = source.crc32;
    r.dos = {source.dos_date, source.dos_time}; r.local64 = source.uncompressed_size >= max32 || source.compressed_size >= max32;
    std::set<std::uint16_t> dropped_ids;
    r.local_extra = carried_extra(source.local_extra, options, dropped_ids);
    r.central_extra = carried_extra(source.central_extra, options, dropped_ids);
    r.entry.dropped_extra_fields = dropped_ids.size();
    if (!source.comment.empty() && !options.carry_comment) {
      if (!options.drop_unlisted) write_fail(Errc::unsupported, "am.zip.write.extra_not_preserved", "Entry comment is not authorized for transfer");
      r.entry.dropped_comment = true;
    }
    if (options.carry_comment) r.comment = source.comment;
    auto decoder = std::make_unique<EntryDecoder>(); decoder->entry = source; decoder->budget = &budget;
    decoder->read_input = [&](U64 offset, std::span<std::byte> bytes) { return archive.read_raw(index, offset, bytes); };
    decoder->accepted_input = [&](Bytes bytes) -> Result<void> { impl_->emit(bytes); return {}; };
    decoder->initialize(true);
    r.entry.local_offset = impl_->position;
    (void)local_header(r); (void)central_record(r);
    started = true; impl_->start(r);
    std::array<std::byte, 65536> output{};
    while (decoder->read(output) != 0) { }
    r.entry.sha256 = decoder->result->sha256;
    if (impl_->position - r.entry.data_offset != source.compressed_size) corrupt("Raw-copy byte count mismatch");
    return impl_->complete(std::move(r));
  } catch (const Error& e) {
    if (!started && (e.code == Errc::invalid_argument || e.code == Errc::unsupported)) return e;
    return impl_->fatal(e);
  } catch (const std::bad_alloc&) { return impl_->fatal(write_oom()); }
  catch (const std::exception& e) { return impl_->fatal(write_error(Errc::io_error, "am.zip.io_error", e.what())); }
}
Result<WriteSummary> ZipWriter::finish() {
  bool began = false;
  try {
    impl_->check(); if (impl_->open) return invalid_state();
    began = true;
    U64 directory_size = 0;
    for (const auto& record : impl_->records) {
      const auto next = central_record(record).size();
      if (directory_size > impl_->options.zip_limits.max_directory_bytes || next > impl_->options.zip_limits.max_directory_bytes - directory_size) limit("Central directory exceeds configured ZipLimits");
      directory_size += next;
    }
    WriteSummary summary; summary.zip_limits = impl_->options.zip_limits; summary.directory_offset = impl_->position; summary.directory_size = directory_size;
    summary.entries.reserve(impl_->records.size());
    for (const auto& r : impl_->records) { summary.entries.push_back(r.entry); impl_->emit(central_record(r)); }
    const U64 count = impl_->records.size();
    const bool end64 = count >= 0xffff || directory_size >= max32 || summary.directory_offset >= max32;
    if (end64) {
      const auto end_offset = impl_->position; std::vector<std::byte> record(56);
      put(record, 0, end64_sig, 4); put(record, 4, 44, 8); put(record, 12, 45, 2); put(record, 14, 45, 2);
      put(record, 24, count, 8); put(record, 32, count, 8); put(record, 40, directory_size, 8); put(record, 48, summary.directory_offset, 8); impl_->emit(record);
      std::array<std::byte, 20> locator{}; put(locator, 0, locator_sig, 4); put(locator, 8, end_offset, 8); put(locator, 16, 1, 4); impl_->emit(locator);
    }
    std::array<std::byte, 22> end{}; put(end, 0, eocd_sig, 4); put(end, 8, std::min<U64>(count, 0xffff), 2); put(end, 10, std::min<U64>(count, 0xffff), 2);
    put(end, 12, std::min(directory_size, max32), 4); put(end, 16, std::min(summary.directory_offset, max32), 4); impl_->emit(end);
    auto flushed = impl_->sink->flush(); if (!flushed) throw flushed.error();
    auto closed = impl_->sink->close(); if (!closed) throw closed.error();
    summary.zip64 = impl_->zip64 || end64; summary.file_size = impl_->position; impl_->finished = true; return summary;
  } catch (const Error& e) { if (!began && e.message_id == "am.zip.write.invalid_state") return e; return impl_->fatal(e); }
  catch (const std::bad_alloc&) { return impl_->fatal(write_oom()); }
  catch (const std::exception& e) { return impl_->fatal(write_error(Errc::io_error, "am.zip.io_error", e.what())); }
}
Result<void> ZipWriter::abort() {
  try {
    if (impl_->finished) return invalid_state();
    impl_->aborted = true; impl_->open = false;
    if (!impl_->discarded) {
      auto discarded = impl_->sink->discard(); if (!discarded) return discarded.error(); impl_->discarded = true;
    }
    if (impl_->failure) return *impl_->failure;
    return {};
  } catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return write_oom(); }
  catch (const std::exception& e) { return write_error(Errc::io_error, "am.zip.io_error", e.what()); }
}
Result<void> verify_written(std::shared_ptr<const RandomAccessSource> source, const WriteSummary& summary, ReadBudget& budget) {
  std::size_t index = 0;
  auto failed = [&](std::optional<Error> cause = {}) -> Error {
    auto result = write_error(Errc::corrupt_data, "am.zip.write.verification_failed", "Written archive differs at entry=" + std::to_string(index));
    if (cause) result.cause = std::make_shared<const Error>(std::move(*cause)); return result;
  };
  try {
    if (!source || source->size() != summary.file_size) return failed();
    auto archive = ZipArchive::open(source, summary.zip_limits); if (!archive) return failed(archive.error());
    if (archive->entries().size() != summary.entries.size() || archive->summary().unaccounted_bytes || archive->summary().comment_length ||
        archive->summary().directory_offset != summary.directory_offset || archive->summary().directory_size != summary.directory_size || archive->summary().zip64 != summary.zip64) return failed();
    for (const auto& expected : summary.entries) {
      const auto& actual = archive->entries()[index];
      if (actual.raw_name != expected.name || actual.method != static_cast<std::uint16_t>(expected.method) || actual.flags != expected.flags || actual.crc32 != expected.crc32 ||
          actual.compressed_size != expected.compressed_size || actual.uncompressed_size != expected.uncompressed_size || actual.local_offset != expected.local_offset || actual.data_offset != expected.data_offset) return failed();
      auto verified = archive->verify_entry(index, budget, expected.sha256.has_value());
      if (!verified) return failed(verified.error());
      if (expected.sha256 && verified->sha256 != expected.sha256) return failed();
      ++index;
    }
    return {};
  } catch (const Error& e) { return failed(e); }
  catch (const std::bad_alloc&) { return write_oom(); }
  catch (const std::exception& e) { return failed(write_error(Errc::io_error, "am.zip.io_error", e.what())); }
}
}  // namespace am::asset_zip
