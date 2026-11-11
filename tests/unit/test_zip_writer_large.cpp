#include <gtest/gtest.h>
#include "am/storage/asset_zip/writer.hpp"
#include "am/foundation/paths.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace am::asset_zip;
namespace {
std::uint16_t u16(std::span<const std::byte> b, std::size_t p) { return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[p]) | (std::to_integer<unsigned>(b[p + 1]) << 8)); }
std::uint32_t u32(std::span<const std::byte> b, std::size_t p) { return u16(b, p) | (static_cast<std::uint32_t>(u16(b, p + 2)) << 16); }
std::uint64_t u64(std::span<const std::byte> b, std::size_t p) { return u32(b, p) | (static_cast<std::uint64_t>(u32(b, p + 4)) << 32); }
am::Result<WrittenEntry> add_empty(ZipWriter& writer, const std::string& name) {
  EntryOptions o; o.method = CompressionMethod::stored; o.sha256 = false;
  auto entry = writer.begin_entry(name, o); if (!entry) return entry.error(); return entry->finish();
}
class FileFaultSink final : public RandomAccessSink {
 public:
  std::shared_ptr<RandomAccessSink> inner;
  std::size_t writes = 0, fail_write = 0, flush_count = 0;
  bool fail_flush = false, close_after_flush = false;
  am::Result<void> write_at(std::uint64_t offset, std::span<const std::byte> b) override {
    if (++writes == fail_write) return full(); return inner->write_at(offset, b);
  }
  am::Result<void> flush() override { ++flush_count; if (fail_flush) return full(); return inner->flush(); }
  am::Result<void> close() override { close_after_flush = flush_count != 0; return inner->close(); }
  am::Result<void> discard() override { return inner->discard(); }
  std::uint64_t size() const noexcept override { return inner->size(); }
  static am::Error full() { return am::Error::make(am::Errc::insufficient_space, "Injected full volume").id("am.zip.write.insufficient_space"); }
};
}
TEST(ZipWriterLarge, CountBoundaryHasZip64OnlyWhenEocdSaturates) {
  RecordProperty("req", "ADR-0005|ADR-0035");
  for (const std::size_t count : {65534u, 70000u}) {
    auto sink = std::make_shared<MemorySink>(); auto writer = ZipWriter::create(sink); ASSERT_TRUE(writer);
    for (std::size_t i = 0; i < count; ++i) { auto entry = add_empty(*writer, "content/" + std::to_string(i)); ASSERT_TRUE(entry); }
    auto summary = writer->finish(); ASSERT_TRUE(summary); EXPECT_EQ(summary->zip64, count >= 65535);
    const auto bytes = sink->bytes(); const auto end = bytes.size() - 22;
    EXPECT_EQ(u16(bytes, end + 8), count >= 65535 ? 65535 : count); EXPECT_EQ(u16(bytes, end + 10), count >= 65535 ? 65535 : count);
    EXPECT_EQ(u32(bytes, end + 12), summary->directory_size); EXPECT_EQ(u32(bytes, end + 16), summary->directory_offset);
    if (count >= 65535) { EXPECT_EQ(u32(bytes, end - 20), 0x07064b50u); EXPECT_EQ(u32(bytes, end - 76), 0x06064b50u); }
    else EXPECT_EQ(summary->directory_offset + summary->directory_size, end);
    auto source = memory_source({bytes.begin(), bytes.end()}); ASSERT_TRUE(source); auto archive = ZipArchive::open(*source); ASSERT_TRUE(archive);
    EXPECT_EQ(archive->entries().size(), count); EXPECT_EQ(archive->summary().unaccounted_bytes, 0u);
    ReadBudget budget(0); ASSERT_TRUE(verify_written(*source, *summary, budget));
  }
}
TEST(ZipWriterFile, ExistingTargetLongPathSharedReadAndFlushBeforeClose) {
  RecordProperty("req", "ADR-0012|ADR-0006");
  auto directory = std::filesystem::path(AM_ZIP_CACHE);
  for (int i = 0; i < 5; ++i) directory /= std::string(48, 'w');
  directory = am::paths::native(directory); std::filesystem::create_directories(directory);
  const auto path = directory / "new-writer.zip"; std::filesystem::remove(path);
  auto sink = file_sink(path); ASSERT_TRUE(sink); auto observed = std::make_shared<FileFaultSink>(); observed->inner = *sink;
  auto writer = ZipWriter::create(observed); ASSERT_TRUE(writer); ASSERT_TRUE(add_empty(*writer, "manifest.json"));
#ifdef _WIN32
  const auto reader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(reader, INVALID_HANDLE_VALUE); EXPECT_TRUE(CloseHandle(reader));
#endif
  auto summary = writer->finish(); ASSERT_TRUE(summary); EXPECT_EQ(observed->flush_count, 1u); EXPECT_TRUE(observed->close_after_flush);
  auto duplicate = file_sink(path); ASSERT_FALSE(duplicate); EXPECT_EQ(duplicate.error().code, am::Errc::conflict); EXPECT_EQ(duplicate.error().message_id, "am.zip.write.target_exists");
  auto source = file_source(path); ASSERT_TRUE(source); EXPECT_EQ((*source)->size(), summary->file_size); ReadBudget budget(0); EXPECT_TRUE(verify_written(*source, *summary, budget));
  // Destructor of a writer without finish removes its newly created file.
  const auto abandoned = directory / "abandoned.zip"; std::filesystem::remove(abandoned);
  { auto temp = file_sink(abandoned); ASSERT_TRUE(temp); auto unfinished = ZipWriter::create(*temp); ASSERT_TRUE(unfinished); ASSERT_TRUE(add_empty(*unfinished, "content/a")); }
  EXPECT_FALSE(std::filesystem::exists(abandoned));
}
TEST(ZipWriterFile, NoSpaceAtDataDirectoryAndFlushDeletesFile) {
  RecordProperty("req", "K-117|ADR-0006");
  const auto directory = std::filesystem::path(AM_ZIP_CACHE); std::filesystem::create_directories(directory);
  for (int phase = 0; phase < 3; ++phase) {
    const auto path = directory / ("full-" + std::to_string(phase) + ".zip"); std::filesystem::remove(path);
    auto sink = file_sink(path); ASSERT_TRUE(sink); auto fault = std::make_shared<FileFaultSink>(); fault->inner = *sink;
    auto writer = ZipWriter::create(fault); ASSERT_TRUE(writer);
    EntryOptions o; o.method = CompressionMethod::stored; o.uncompressed_size = 3;
    auto entry = writer->begin_entry("manifest.json", o); ASSERT_TRUE(entry);
    const std::string abc = "abc";
    if (phase == 0) { fault->fail_write = fault->writes + 1; auto failed = entry->write(std::as_bytes(std::span(abc))); ASSERT_FALSE(failed); }
    else {
      ASSERT_TRUE(entry->write(std::as_bytes(std::span(abc)))); ASSERT_TRUE(entry->finish());
      if (phase == 1) fault->fail_write = fault->writes + 1; else fault->fail_flush = true;
    }
    auto end = writer->finish(); ASSERT_FALSE(end); EXPECT_EQ(end.error().message_id, "am.zip.write.insufficient_space"); EXPECT_FALSE(std::filesystem::exists(path));
  }
}
TEST(ZipWriterLarge, FourGiBPlusOneDeflateLevelZeroSizeOffsetShaAndMemory) {
  RecordProperty("req", "K-116|ADR-0006");
  const auto start = std::chrono::steady_clock::now();
  const auto directory = std::filesystem::path(AM_ZIP_CACHE); std::filesystem::create_directories(directory);
  const auto path = directory / "writer-four-gib.zip"; std::filesystem::remove(path);
  auto sink = file_sink(path); ASSERT_TRUE(sink); WriterOptions options; options.deflate_level = 0;
  auto writer = ZipWriter::create(*sink, options); ASSERT_TRUE(writer);
  constexpr std::uint64_t total = 4ULL * 1024 * 1024 * 1024 + 1;
  EntryOptions entry_options; entry_options.uncompressed_size = total;
  auto entry = writer->begin_entry("content/zeros", entry_options); ASSERT_TRUE(entry);
  auto expected = Sha256::create(); ASSERT_TRUE(expected); std::array<std::byte, 65536> zeros{};
  for (std::uint64_t offset = 0; offset < total;) {
    const auto bytes = std::span(zeros).first(static_cast<std::size_t>(std::min<std::uint64_t>(zeros.size(), total - offset)));
    ASSERT_TRUE(expected->update(bytes)); auto written = entry->write(bytes); ASSERT_TRUE(written) << written.error().ctx.detail; offset += bytes.size();
  }
  auto first = entry->finish(); ASSERT_TRUE(first); auto digest = expected->finish(); ASSERT_TRUE(digest); ASSERT_EQ(first->sha256, std::optional<Sha256Digest>(*digest));
  EntryOptions small; small.method = CompressionMethod::stored; small.uncompressed_size = 3;
  auto last = writer->begin_entry("content/last", small); ASSERT_TRUE(last); const std::string abc = "abc";
  ASSERT_TRUE(last->write(std::as_bytes(std::span(abc)))); auto second = last->finish(); ASSERT_TRUE(second); EXPECT_GE(second->local_offset, 4ULL * 1024 * 1024 * 1024);
  auto summary = writer->finish(); ASSERT_TRUE(summary); const auto written = std::chrono::steady_clock::now();
  auto source = file_source(path); ASSERT_TRUE(source); ReadBudget budget(total + 3); auto verified = verify_written(*source, *summary, budget); ASSERT_TRUE(verified) << verified.error().ctx.detail;
  EXPECT_EQ(budget.remaining(), 0u); auto archive = ZipArchive::open(*source); ASSERT_TRUE(archive); EXPECT_EQ(archive->summary().unaccounted_bytes, 0u);
  const auto& a = archive->entries()[0]; const auto& b = archive->entries()[1];
  EXPECT_EQ(a.local_extra.size(), 20u); EXPECT_EQ(u16(a.local_extra, 0), 1); EXPECT_EQ(u16(a.local_extra, 2), 16);
  EXPECT_EQ(u64(a.local_extra, 4), total); EXPECT_EQ(u64(a.local_extra, 12), a.compressed_size);
  EXPECT_EQ(a.central_extra.size(), 20u); EXPECT_EQ(u64(a.central_extra, 4), total); EXPECT_EQ(u64(a.central_extra, 12), a.compressed_size);
  EXPECT_TRUE(b.local_extra.empty()); EXPECT_EQ(b.central_extra.size(), 12u); EXPECT_EQ(u64(b.central_extra, 4), b.local_offset);
  std::array<std::byte, 30> header{}; ASSERT_TRUE((*source)->read_at(0, header)); EXPECT_EQ(u32(header, 18), 0xffffffffu); EXPECT_EQ(u32(header, 22), 0xffffffffu);
  ASSERT_TRUE((*source)->read_at(b.local_offset, header)); EXPECT_EQ(u16(header, 4), 45);
  const auto finish = std::chrono::steady_clock::now();
  std::cout << "ZIP writer 4GiB+1: level=0 file_bytes=" << summary->file_size << " write_seconds=" << std::chrono::duration<double>(written - start).count()
      << " verify_seconds=" << std::chrono::duration<double>(finish - written).count();
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
  ASSERT_TRUE(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)));
  std::cout << " peak_working_set_bytes=" << memory.PeakWorkingSetSize << " peak_commit_bytes=" << memory.PeakPagefileUsage;
#endif
  std::cout << '\n';
}
