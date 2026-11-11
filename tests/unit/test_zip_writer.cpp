#include <gtest/gtest.h>
#include "am/storage/asset_zip/writer.hpp"
#include "am/storage/asset_zip/profile.hpp"
#include "zip_builder.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <random>
#include <stdexcept>
#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

using namespace am::asset_zip;
namespace {
using Bytes = std::vector<std::byte>;
am::Result<WrittenEntry> add(ZipWriter& writer, std::string_view name, std::string_view data, CompressionMethod method = CompressionMethod::deflate, bool sha = true) {
  EntryOptions options; options.method = method; options.uncompressed_size = data.size(); options.sha256 = sha;
  auto entry = writer.begin_entry(name, options); if (!entry) return entry.error();
  auto written = entry->write(std::as_bytes(std::span(data))); if (!written) return written.error();
  return entry->finish();
}
am::Result<ZipArchive> open(Bytes bytes) {
  auto source = memory_source(std::move(bytes)); if (!source) return source.error(); return ZipArchive::open(*source);
}
Bytes snapshot(const MemorySink& sink) { return {sink.bytes().begin(), sink.bytes().end()}; }
void verify(const MemorySink& sink, const WriteSummary& summary) {
  auto source = memory_source(snapshot(sink)); ASSERT_TRUE(source); ReadBudget budget(10000000);
  auto result = verify_written(*source, summary, budget); EXPECT_TRUE(result) << (result ? "" : result.error().ctx.detail);
}
Bytes raw(const ZipArchive& archive, std::size_t index) {
  Bytes result; std::array<std::byte, 103> buffer{};
  for (;;) { auto n = archive.read_raw(index, result.size(), buffer); if (!n) throw std::runtime_error(n.error().ctx.detail); if (!*n) break; result.insert(result.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*n)); }
  return result;
}
class FaultSink final : public RandomAccessSink {
 public:
  std::shared_ptr<RandomAccessSink> inner = std::make_shared<MemorySink>();
  std::size_t calls = 0, fail_call = 0, flushes = 0, closes = 0, discards = 0;
  bool fail_flush = false, throw_write = false, flushed_before_close = false;
  std::uint64_t max_end = 0;
  std::vector<std::pair<std::uint64_t, std::size_t>> history;
  am::Result<void> write_at(std::uint64_t p, std::span<const std::byte> bytes) override {
    ++calls;
    if (throw_write) throw std::runtime_error("Injected sink exception");
    if (calls == fail_call) return full();
    auto written = inner->write_at(p, bytes);
    if (written) { history.emplace_back(p, bytes.size()); max_end = std::max(max_end, p + bytes.size()); }
    return written;
  }
  am::Result<void> flush() override { ++flushes; if (fail_flush) return full(); return inner->flush(); }
  am::Result<void> close() override { ++closes; flushed_before_close = flushes != 0; return inner->close(); }
  am::Result<void> discard() override { ++discards; return inner->discard(); }
  std::uint64_t size() const noexcept override { return inner->size(); }
  static am::Error full() { return am::Error::make(am::Errc::insufficient_space, "Injected full volume", "Injected sink failure").id("am.zip.write.insufficient_space"); }
};
}
TEST(ZipWriter, RoundTripStoredDeflateEmptyUnicodeAndLargeInput) {
  RecordProperty("req", "ADR-0005|ADR-0035");
  auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer);
  ASSERT_TRUE(add(*writer, "manifest.json", "abc", CompressionMethod::stored));
  ASSERT_TRUE(add(*writer, "content/\u043c\u043e\u0434\u0435\u043b\u044c.stl", std::string(120000, 'x')));
  ASSERT_TRUE(add(*writer, "content/\u6a21\u578b", "", CompressionMethod::stored));
  ASSERT_TRUE(add(*writer, "generated/empty", ""));
  auto summary = writer->finish(); ASSERT_TRUE(summary); verify(*sink, *summary);
  auto archive = open(snapshot(*sink)); ASSERT_TRUE(archive); auto profile = check_normative_profile(*archive); ASSERT_TRUE(profile); EXPECT_TRUE(profile->empty());
  EXPECT_FALSE(summary->zip64); EXPECT_EQ(archive->summary().unaccounted_bytes, 0u);
  for (const auto& entry : summary->entries) { EXPECT_EQ(entry.flags, 0x800); EXPECT_TRUE(entry.sha256); }
  auto empty_sink = std::make_shared<MemorySink>(); auto empty = ZipWriter::create(empty_sink); ASSERT_TRUE(empty);
  auto nothing = empty->finish(); ASSERT_TRUE(nothing); EXPECT_EQ(empty_sink->size(), 22u); verify(*empty_sink, *nothing);
}
TEST(ZipWriter, DeterminismAndManualSmallStoredGolden) {
  RecordProperty("req", "ADR-0005|ADR-0035");
  Bytes first;
  for (int iteration = 0; iteration < 2; ++iteration) {
    auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer);
    auto entry = add(*writer, "manifest.json", "abc", CompressionMethod::stored); ASSERT_TRUE(entry);
    EXPECT_EQ(entry->local_offset, 0u); EXPECT_EQ(entry->data_offset, 43u); EXPECT_EQ(entry->crc32, 0x352441c2u);
    auto summary = writer->finish(); ASSERT_TRUE(summary); EXPECT_EQ(summary->directory_offset, 46u); EXPECT_EQ(summary->directory_size, 59u);
    if (iteration) EXPECT_EQ(snapshot(*sink), first); else first = snapshot(*sink);
  }
  const std::string golden =
      "504b030414000008000000002100c241243503000000030000000d0000006d616e69666573742e6a736f6e616263"
      "504b01022d0014000008000000002100c241243503000000030000000d0000000000000000000000000000000000"
      "6d616e69666573742e6a736f6e504b050600000000010001003b0000002e0000000000";
  Bytes expected;
  for (std::size_t i = 0; i < golden.size(); i += 2) expected.push_back(static_cast<std::byte>(std::stoul(golden.substr(i, 2), nullptr, 16)));
  EXPECT_EQ(first, expected);
}
TEST(ZipWriter, SizeMismatchIsStickyAndDiscardsStoredAndDeflate) {
  RecordProperty("req", "ADR-0006|ADR-0035");
  for (const auto method : {CompressionMethod::stored, CompressionMethod::deflate}) for (const bool excess : {false, true}) {
    auto sink = std::make_shared<FaultSink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer);
    EntryOptions options; options.method = method; options.uncompressed_size = excess ? 2 : 4;
    auto entry = writer->begin_entry("content/a", options); ASSERT_TRUE(entry);
    const std::string abc = "abc"; auto write = entry->write(std::as_bytes(std::span(abc)));
    if (excess) { ASSERT_FALSE(write); EXPECT_EQ(write.error().message_id, "am.zip.write.size_mismatch"); }
    else { ASSERT_TRUE(write); auto end = entry->finish(); ASSERT_FALSE(end); EXPECT_EQ(end.error().message_id, "am.zip.write.size_mismatch"); }
    auto again = writer->finish(); ASSERT_FALSE(again); EXPECT_EQ(again.error().code, am::Errc::conflict); EXPECT_EQ(again.error().message_id, "am.zip.write.size_mismatch");
    EXPECT_GT(sink->discards, 0u); EXPECT_EQ(sink->size(), 0u);
    auto child_again = entry->write({}); ASSERT_FALSE(child_again); EXPECT_EQ(child_again.error().message_id, again.error().message_id);
    auto cleanup = writer->abort(); ASSERT_FALSE(cleanup); EXPECT_EQ(cleanup.error().message_id, again.error().message_id);
  }
}
TEST(ZipWriter, InvalidNamesAndCollisionsDoNotPoisonWriter) {
  RecordProperty("req", "ADR-0012|ADR-0035");
  auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer);
  EntryOptions options; options.method = CompressionMethod::stored;
  const std::vector<std::string> names{"", "../x", "/x", "C:x", "a\\b", "a//b", "a/", "a/./b", "a/../b", "a?", "a*", "a:", "a<", "a>", "a|", "a\"", "a.", "a ", "CON", "lpt1.txt", "a\x01", "a\x7f", "\xff", "cafe\u0301", std::string(201, 'a')};
  for (const auto& name : names) { auto entry = writer->begin_entry(name, options); ASSERT_FALSE(entry) << name; EXPECT_EQ(entry.error().message_id, "am.zip.write.invalid_name"); EXPECT_EQ(sink->size(), 0u); }
  ASSERT_TRUE(add(*writer, "content/A.stl", ""));
  for (const auto name : {"content/A.stl", "content/a.stl"}) { auto entry = writer->begin_entry(name, options); ASSERT_FALSE(entry); EXPECT_EQ(entry.error().message_id, "am.zip.write.name_collision"); }
  ASSERT_TRUE(add(*writer, "content/caf\u00e9", ""));
  auto nfd = writer->begin_entry("content/cafe\u0301", options); ASSERT_FALSE(nfd); EXPECT_EQ(nfd.error().message_id, "am.zip.write.name_collision");
  ASSERT_TRUE(add(*writer, "manifest.json", "{}")); auto summary = writer->finish(); ASSERT_TRUE(summary); verify(*sink, *summary);
}
TEST(ZipWriter, StatesAbortAndUnfinishedDestruction) {
  RecordProperty("req", "ADR-0006");
  auto sink = std::make_shared<FaultSink>();
  {
    auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer); EntryOptions options; options.method = CompressionMethod::stored;
    auto child = writer->begin_entry("manifest.json", options); ASSERT_TRUE(child);
    EXPECT_FALSE(writer->begin_entry("content/b", options)); EXPECT_FALSE(writer->finish());
    auto a = open(zip_test::build().bytes); ASSERT_TRUE(a); ReadBudget budget(0); EXPECT_FALSE(writer->copy_raw(*a, 0, budget));
    ASSERT_TRUE(child->finish()); auto after = child->write({}); ASSERT_FALSE(after); EXPECT_EQ(after.error().message_id, "am.zip.write.invalid_state");
    auto end = writer->finish(); ASSERT_TRUE(end); EXPECT_EQ(sink->flushes, 1u); EXPECT_EQ(sink->closes, 1u); EXPECT_TRUE(sink->flushed_before_close);
    EXPECT_FALSE(writer->finish()); EXPECT_FALSE(writer->begin_entry("content/b", options)); EXPECT_FALSE(writer->abort());
  }
  EXPECT_EQ(sink->discards, 0u);
  auto unfinished = std::make_shared<FaultSink>();
  { auto writer = ZipWriter::create(unfinished); ASSERT_TRUE(writer); ASSERT_TRUE(add(*writer, "content/a", "abc")); }
  EXPECT_EQ(unfinished->size(), 0u); EXPECT_GT(unfinished->discards, 0u);
  auto abort_sink = std::make_shared<FaultSink>(); auto writer = ZipWriter::create(abort_sink); ASSERT_TRUE(writer);
  ASSERT_TRUE(writer->abort()); ASSERT_TRUE(writer->abort()); EXPECT_EQ(abort_sink->discards, 1u);
}
TEST(ZipWriter, RawCopiesAllPythonFixturesAndDescriptorVariants) {
  RecordProperty("req", "ADR-0006|ADR-0035");
  for (const auto& path : std::filesystem::directory_iterator(AM_ZIP_FIXTURES)) {
    if (path.path().extension() != ".zip") continue;
    SCOPED_TRACE(path.path().filename().string()); auto source = file_source(path.path()); ASSERT_TRUE(source); auto archive = ZipArchive::open(*source); ASSERT_TRUE(archive);
    auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer); ReadBudget budget(100000);
    for (std::size_t i = 0; i < archive->entries().size(); ++i) ASSERT_TRUE(writer->copy_raw(*archive, i, budget));
    auto summary = writer->finish(); ASSERT_TRUE(summary); verify(*sink, *summary); auto result = open(snapshot(*sink)); ASSERT_TRUE(result);
    for (std::size_t i = 0; i < result->entries().size(); ++i) {
      EXPECT_EQ(raw(*result, i), raw(*archive, i)); EXPECT_EQ(result->entries()[i].flags, 0x800);
      EXPECT_TRUE(result->entries()[i].central_extra.empty()); EXPECT_TRUE(result->entries()[i].local_extra.empty()); EXPECT_FALSE(result->entries()[i].has_zip64);
    }
  }
  zip_test::Entry e; e.method = 8; e.flags |= 8; e.descriptor_signature = false;
  auto archive = open(zip_test::build({e}).bytes); ASSERT_TRUE(archive); auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer); ReadBudget budget(3);
  ASSERT_TRUE(writer->copy_raw(*archive, 0, budget)); auto summary = writer->finish(); ASSERT_TRUE(summary); verify(*sink, *summary);
}
TEST(ZipWriter, RawExtraAndCommentPolicyIsExplicitAndRecoverable) {
  RecordProperty("req", "ADR-0006|ADR-0037");
  zip_test::Entry e; e.method = 8; e.zip64 = true; e.comment = std::string("c\0\xff", 3);
  e.local_extra = {std::byte{0x55}, std::byte{0x54}, std::byte{1}, std::byte{0}, std::byte{7}};
  e.central_extra = {std::byte{0x75}, std::byte{0x78}, std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0xff}};
  auto archive = open(zip_test::build({e}).bytes); ASSERT_TRUE(archive); auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer); ReadBudget budget(100);
  RawCopyOptions options;
  for (int pass = 0; pass < 3; ++pass) {
    auto rejected = writer->copy_raw(*archive, 0, budget, options); ASSERT_FALSE(rejected); EXPECT_EQ(rejected.error().message_id, "am.zip.write.extra_not_preserved"); EXPECT_EQ(sink->size(), 0u);
    if (pass == 0) options.carry_extra_ids.push_back(0x5455); else options.carry_extra_ids.push_back(0x7875);
  }
  options.carry_comment = true; auto copied = writer->copy_raw(*archive, 0, budget, options); ASSERT_TRUE(copied);
  ASSERT_TRUE(add(*writer, "manifest.json", "{}")); auto summary = writer->finish(); ASSERT_TRUE(summary); verify(*sink, *summary);
  auto result = open(snapshot(*sink)); ASSERT_TRUE(result); const auto& entry = result->entries()[0];
  EXPECT_EQ(entry.local_extra, e.local_extra); EXPECT_EQ(entry.central_extra, e.central_extra); EXPECT_EQ(entry.comment, archive->entries()[0].comment); EXPECT_FALSE(entry.has_zip64);
}
namespace {
class ChangingSource final : public RandomAccessSource {
 public:
  zip_test::Built data = zip_test::build({zip_test::Entry{}});
  bool change = false;
  mutable std::size_t raw_reads = 0;
  std::uint64_t size() const noexcept override { return data.bytes.size(); }
  am::Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) const override {
    if (offset >= size()) return std::size_t{0};
    const auto n = std::min<std::size_t>(output.size(), data.bytes.size() - static_cast<std::size_t>(offset));
    std::copy_n(data.bytes.begin() + static_cast<std::ptrdiff_t>(offset), n, output.begin());
    if (change && offset == data.data[0] && n) { output[0] ^= std::byte{1}; ++raw_reads; }
    return n;
  }
};
}
TEST(ZipWriter, ChangedRawSourceIsReadOnceAndNoUncheckedPayloadIsWritten) {
  RecordProperty("req", "ADR-0006|ADR-0035");
  auto source = std::make_shared<ChangingSource>(); auto archive = ZipArchive::open(source); ASSERT_TRUE(archive); source->change = true;
  auto sink = std::make_shared<FaultSink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer); ReadBudget budget(3);
  auto copied = writer->copy_raw(*archive, 0, budget); ASSERT_FALSE(copied); EXPECT_EQ(copied.error().message_id, "am.zip.entry_corrupt");
  EXPECT_EQ(source->raw_reads, 1u); EXPECT_EQ(sink->history.size(), 1u); // Only the local header reached the sink.
  EXPECT_EQ(sink->size(), 0u); auto sticky = writer->finish(); ASSERT_FALSE(sticky); EXPECT_EQ(sticky.error().message_id, copied.error().message_id);
}
TEST(ZipWriter, RawCorruptionUnsupportedMethodsAndBudget) {
  RecordProperty("req", "ADR-0006|ADR-0035");
  for (int mode = 0; mode < 6; ++mode) {
    zip_test::Entry e; e.method = 8; e.data = std::string(200000, 'x');
    if (mode == 0) e.declared_crc = 1;
    if (mode == 1 || mode == 2) { e.compressed = zip_test::raw_deflate(e.data); if (mode == 1) e.compressed->pop_back(); else e.compressed->push_back(std::byte{0}); }
    if (mode == 3) e.method = 12;
    if (mode == 4) e.flags |= 1;
    auto archive = open(zip_test::build({e}).bytes); ASSERT_TRUE(archive); auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer); ReadBudget budget(mode == 5 ? 2 : 1000000);
    auto result = writer->copy_raw(*archive, 0, budget); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().message_id, mode == 3 || mode == 4 ? "am.zip.unsupported_feature" : mode == 5 ? "am.zip.limit_exceeded" : "am.zip.entry_corrupt");
    EXPECT_EQ(sink->size(), 0u);
  }
}
namespace {
class CountedSource final : public RandomAccessSource {
 public:
  Bytes bytes;
  bool track = false;
  mutable std::uint64_t next = 0, total = 0;
  mutable bool ordered = true;
  std::uint64_t size() const noexcept override { return bytes.size(); }
  am::Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) const override {
    if (offset >= size()) return std::size_t{0};
    const auto n = std::min<std::size_t>({output.size(), bytes.size() - static_cast<std::size_t>(offset), 13});
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), n, output.begin());
    if (track) { ordered = ordered && offset == next; next = offset + n; total += n; }
    return n;
  }
};
}
TEST(ZipWriter, RawManyChunksUseOnePassWithShortSourceReads) {
  RecordProperty("req", "ADR-0006|ADR-0035");
  zip_test::Entry e; e.method = 8; e.data.resize(200000); std::mt19937 random(654321);
  for (auto& c : e.data) c = static_cast<char>(random() & 255);
  auto built = zip_test::build({e}); auto source = std::make_shared<CountedSource>(); source->bytes = built.bytes;
  auto archive = ZipArchive::open(source); ASSERT_TRUE(archive); source->next = archive->entries()[0].data_offset; source->track = true;
  auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer); ReadBudget budget(e.data.size());
  auto entry = writer->copy_raw(*archive, 0, budget); ASSERT_TRUE(entry) << entry.error().ctx.detail;
  EXPECT_TRUE(source->ordered); EXPECT_EQ(source->total, archive->entries()[0].compressed_size); EXPECT_EQ(budget.remaining(), 0u);
  source->track = false; auto summary = writer->finish(); ASSERT_TRUE(summary); verify(*sink, *summary);
  auto result = open(snapshot(*sink)); ASSERT_TRUE(result); EXPECT_EQ(raw(*result, 0), raw(*archive, 0));
}
TEST(ZipWriter, SinkNoSpaceInDataDirectoryAndFlushAndException) {
  RecordProperty("req", "K-117|ADR-0006");
  for (int phase = 0; phase < 4; ++phase) {
    auto sink = std::make_shared<FaultSink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer);
    if (phase == 0) sink->fail_call = 2; // Header succeeds, Stored data fails.
    if (phase == 3) sink->throw_write = true;
    auto entry = add(*writer, "manifest.json", "abc", CompressionMethod::stored);
    if (phase == 0 || phase == 3) ASSERT_FALSE(entry);
    else { ASSERT_TRUE(entry); if (phase == 1) sink->fail_call = sink->calls + 1; else sink->fail_flush = true; }
    auto result = writer->finish(); ASSERT_FALSE(result); EXPECT_EQ(result.error().message_id, phase == 3 ? "am.zip.io_error" : "am.zip.write.insufficient_space");
    EXPECT_EQ(result.error().code, phase == 3 ? am::Errc::io_error : am::Errc::insufficient_space); EXPECT_GT(sink->discards, 0u); EXPECT_EQ(sink->size(), 0u);
  }
}
TEST(ZipWriter, FileAndZipLimitsAreEnforcedBeforeExceededBytes) {
  RecordProperty("req", "ADR-0031|ADR-0033|ADR-0035");
  auto sink = std::make_shared<FaultSink>(); WriterOptions options; options.limits.max_file_size = 60;
  auto writer = ZipWriter::create(sink, options); ASSERT_TRUE(writer); ASSERT_TRUE(add(*writer, "manifest.json", "abc", CompressionMethod::stored));
  auto summary = writer->finish(); ASSERT_FALSE(summary); EXPECT_EQ(summary.error().message_id, "am.zip.write.file_too_large"); EXPECT_LE(sink->max_end, 60u);
  for (int mode = 0; mode < 4; ++mode) {
    options = {}; if (mode == 0) options.zip_limits.max_entries = 0; if (mode == 1) options.zip_limits.max_name_bytes = 1; if (mode == 2) options.zip_limits.max_entry_bytes = 2; if (mode == 3) options.zip_limits.max_directory_bytes = 1;
    auto limit_sink = std::make_shared<FaultSink>(); auto limited = ZipWriter::create(limit_sink, options); ASSERT_TRUE(limited);
    auto e = add(*limited, "manifest.json", "abc", CompressionMethod::stored);
    if (mode < 3) { ASSERT_FALSE(e); EXPECT_EQ(e.error().message_id, "am.zip.limit_exceeded"); EXPECT_TRUE(limit_sink->history.empty()); }
    else { ASSERT_TRUE(e); auto end = limited->finish(); ASSERT_FALSE(end); EXPECT_EQ(end.error().message_id, "am.zip.limit_exceeded"); EXPECT_EQ(limit_sink->history.size(), 3u); }
  }
}
TEST(ZipWriter, VerificationRejectsDataCrcNameGapAndSummaryTampering) {
  RecordProperty("req", "ADR-0006");
  auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer); ASSERT_TRUE(add(*writer, "manifest.json", "abc", CompressionMethod::stored)); auto summary = writer->finish(); ASSERT_TRUE(summary);
  for (int mode = 0; mode < 5; ++mode) {
    auto data = snapshot(*sink); auto expected = *summary;
    if (mode == 0) data[43] ^= std::byte{1};
    if (mode == 1) data[static_cast<std::size_t>(summary->directory_offset) + 16] ^= std::byte{1};
    if (mode == 2) data[30] ^= std::byte{1};
    if (mode == 3) { data.insert(data.end() - 22, std::byte{0}); ++expected.file_size; }
    if (mode == 4) (*expected.entries[0].sha256)[0] ^= std::byte{1};
    auto source = memory_source(std::move(data)); ASSERT_TRUE(source); ReadBudget budget(100);
    auto verified = verify_written(*source, expected, budget); ASSERT_FALSE(verified); EXPECT_EQ(verified.error().message_id, "am.zip.write.verification_failed");
    EXPECT_NE(verified.error().ctx.detail.find("entry="), std::string::npos);
    if (mode < 3) EXPECT_TRUE(verified.error().cause);
  }
}
TEST(ZipWriter, CompressionPolicyUsesExtensionsAndBoundedSample) {
  RecordProperty("req", "ADR-0005");
  for (const auto ext : {"JPG", "jpeg", "PNG", "webp", "gif", "mp4", "mkv", "webm", "ZIP", "7z", "rar", "gz", "bz2", "xz", "zst", "3MF"}) {
    auto choice = choose_method(std::string("content/a.") + ext, {}); ASSERT_TRUE(choice); EXPECT_EQ(*choice, CompressionMethod::stored);
  }
  std::array<std::byte, 65536> sample{}; std::mt19937 generator(123456);
  for (auto& byte : sample) byte = static_cast<std::byte>(generator() & 255);
  auto random = choose_method("a.stl", sample); ASSERT_TRUE(random); EXPECT_EQ(*random, CompressionMethod::stored);
  sample.fill(std::byte{'a'}); auto text = choose_method("a.txt", sample); ASSERT_TRUE(text); EXPECT_EQ(*text, CompressionMethod::deflate);
  auto empty = choose_method("a.txt", {}); ASSERT_TRUE(empty); EXPECT_EQ(*empty, CompressionMethod::deflate);
}
TEST(ZipWriter, SafeOpaqueNamesAreLeftForTheCallerProfileDecision) {
  RecordProperty("req", "ADR-0037");
  auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer);
  ASSERT_TRUE(add(*writer, "manifest.json", "{}")); ASSERT_TRUE(add(*writer, "other/opaque.bin", "abc"));
  auto summary = writer->finish(); ASSERT_TRUE(summary); verify(*sink, *summary);
  auto archive = open(snapshot(*sink)); ASSERT_TRUE(archive); auto profile = check_normative_profile(*archive); ASSERT_TRUE(profile);
  EXPECT_TRUE(std::any_of(profile->begin(), profile->end(), [](const auto& v) { return v.code == profile_code::space; }));
}
#if defined(_WIN32) && defined(_DEBUG)
namespace {
thread_local bool fail_writer_allocation = false;
int __cdecl writer_allocation_hook(int type, void*, std::size_t size, int, long, const unsigned char*, int) {
  // Skip MSVC Debug iterator proxies allocated by noexcept string constructors;
  // inject failure into the normalization buffer allocation instead.
  if (fail_writer_allocation && size > 2 * sizeof(void*) && (type == _HOOK_ALLOC || type == _HOOK_REALLOC)) { fail_writer_allocation = false; return 0; }
  return 1;
}
}
TEST(ZipWriter, UnicodeKeyAllocationFailureIsFatalOutOfMemory) {
  RecordProperty("req", "ADR-0032|ADR-0006");
  auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer);
  EntryOptions options;
  const auto previous = _CrtSetAllocHook(writer_allocation_hook); fail_writer_allocation = true;
  auto entry = writer->begin_entry("content/a", options);
  fail_writer_allocation = false; _CrtSetAllocHook(previous);
  ASSERT_FALSE(entry); EXPECT_EQ(entry.error().code, am::Errc::resource_exhausted); EXPECT_EQ(entry.error().message_id, "am.zip.out_of_memory");
  auto sticky = writer->finish(); ASSERT_FALSE(sticky); EXPECT_EQ(sticky.error().message_id, entry.error().message_id); EXPECT_EQ(sink->size(), 0u);
}
#endif
