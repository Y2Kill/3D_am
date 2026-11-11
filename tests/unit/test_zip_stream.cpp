#include <gtest/gtest.h>
#include "zip_builder.hpp"
#include "am/storage/asset_zip/reader.hpp"
#include "am/foundation/paths.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
using namespace am::asset_zip;
using namespace zip_test;
namespace {
std::string hex(const Sha256Digest& digest) {
  std::string out; constexpr char digits[] = "0123456789abcdef";
  for (const auto byte : digest) { auto c = std::to_integer<unsigned>(byte); out += digits[c >> 4]; out += digits[c & 15]; }
  return out;
}
am::Result<ZipArchive> archive(const std::vector<Entry>& entries) {
  auto source = memory_source(build(entries).bytes); if (!source) return source.error();
  return ZipArchive::open(*source);
}
void stream_corrupt(const Entry& e) {
  auto a = archive({e}); ASSERT_TRUE(a) << a.error().ctx.detail;
  ReadBudget budget(1024 * 1024); auto stream = a->open_entry(0, budget); ASSERT_TRUE(stream);
  std::array<std::byte, 7> buffer{};
  std::optional<am::Error> failure;
  for (;;) {
    auto n = stream->read(buffer);
    if (!n) { failure = n.error(); break; }
    if (*n == 0) break;
  }
  ASSERT_TRUE(failure); EXPECT_EQ(failure->message_id, "am.zip.entry_corrupt");
  EXPECT_EQ(failure->code, am::Errc::corrupt_data); EXPECT_FALSE(stream->verification());
  auto again = stream->read(buffer); ASSERT_FALSE(again); EXPECT_EQ(again.error().message_id, failure->message_id);
  EXPECT_EQ(again.error().ctx.detail, failure->ctx.detail);
}
}
TEST(ZipStream, Sha256KnownVectorsAndFinalization) {
  RecordProperty("req", "ADR-0035");
  auto hash = Sha256::create(); ASSERT_TRUE(hash);
  auto empty = hash->finish(); ASSERT_TRUE(empty);
  EXPECT_EQ(hex(*empty), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  ASSERT_TRUE(hash->finish()); EXPECT_EQ(*hash->finish(), *empty);
  const std::string abc = "abc";
  auto another = Sha256::create(); ASSERT_TRUE(another);
  ASSERT_TRUE(another->update(std::as_bytes(std::span(abc))));
  auto digest = another->finish(); ASSERT_TRUE(digest);
  EXPECT_EQ(hex(*digest), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_FALSE(another->update({}));
  for (const std::uint16_t method : {std::uint16_t{0}, std::uint16_t{8}}) {
    Entry e; e.method = method; auto a = archive({e}); ASSERT_TRUE(a); ReadBudget budget(3);
    auto verified = a->verify_entry(0, budget, true); ASSERT_TRUE(verified); ASSERT_TRUE(verified->sha256);
    EXPECT_EQ(hex(*verified->sha256), hex(*digest)); EXPECT_EQ(verified->crc32, 0x352441c2u);
  }
}
TEST(ZipStream, SizeCrcTruncationTailAndInvalidInflate) {
  RecordProperty("req", "ADR-0035");
  Entry e; e.method = 8; e.data = std::string(1000, 'x');
  auto larger = e; larger.declared_size = 999; stream_corrupt(larger);
  auto smaller = e; smaller.declared_size = 1001; stream_corrupt(smaller);
  auto crc = e; crc.declared_crc = 123; stream_corrupt(crc);
  auto tail = e; tail.compressed = raw_deflate(e.data); tail.compressed->push_back(std::byte{0}); stream_corrupt(tail);
  auto truncated = e; truncated.compressed = raw_deflate(e.data); truncated.compressed->pop_back(); stream_corrupt(truncated);
  auto invalid = e; invalid.compressed = Bytes{std::byte{0xff}, std::byte{0xff}}; stream_corrupt(invalid);
  Entry stored; stored.declared_size = 2; stream_corrupt(stored);
  stored.declared_size = 4; stream_corrupt(stored);
  stored.declared_size.reset(); stored.declared_crc = 1; stream_corrupt(stored);
}
TEST(ZipStream, ExcessStopsAtFirstByteAndBudgetIsSharedAndSticky) {
  RecordProperty("req", "ADR-0035");
  Entry bomb; bomb.method = 8; bomb.data = std::string(200000, 'x'); bomb.declared_size = 3;
  auto a = archive({bomb}); ASSERT_TRUE(a); ReadBudget three(3);
  auto stream = a->open_entry(0, three); ASSERT_TRUE(stream); std::array<std::byte, 65536> buffer{};
  auto first = stream->read(buffer); ASSERT_TRUE(first); EXPECT_EQ(*first, 3u); EXPECT_EQ(three.remaining(), 0u);
  buffer.fill(std::byte{0x55}); auto excess = stream->read(buffer); ASSERT_FALSE(excess);
  EXPECT_EQ(excess.error().message_id, "am.zip.entry_corrupt"); EXPECT_EQ(buffer[0], std::byte{0x55});
  EXPECT_EQ(three.remaining(), 0u); // No excess byte is delivered or accumulated.
  Entry second; second.name = "content/b";
  auto two = archive({Entry{}, second}); ASSERT_TRUE(two); ReadBudget four(4);
  ASSERT_TRUE(two->verify_entry(0, four)); EXPECT_EQ(four.remaining(), 1u);
  auto partial = two->open_entry(1, four); ASSERT_TRUE(partial);
  auto one = partial->read(buffer); ASSERT_TRUE(one); EXPECT_EQ(*one, 1u); EXPECT_EQ(four.remaining(), 0u);
  auto limit = partial->read(buffer); ASSERT_FALSE(limit); EXPECT_EQ(limit.error().message_id, "am.zip.limit_exceeded");
  auto sticky = partial->read(buffer); ASSERT_FALSE(sticky); EXPECT_EQ(sticky.error().ctx.detail, limit.error().ctx.detail);
  ReadBudget zero(0); auto none = two->verify_entry(0, zero); ASSERT_FALSE(none);
  EXPECT_EQ(none.error().code, am::Errc::resource_exhausted);
  auto no_allocation = two->open_entry(0, zero); ASSERT_FALSE(no_allocation);
  EXPECT_EQ(no_allocation.error().message_id, "am.zip.limit_exceeded");
  EXPECT_FALSE(four.consume(1)); EXPECT_EQ(four.remaining(), 0u);
}
TEST(ZipStream, FileSourceSharedReadLongPathAndErrors) {
  RecordProperty("req", "ADR-0012|ADR-0035");
  auto directory = std::filesystem::path(AM_ZIP_CACHE);
  for (int i = 0; i < 5; ++i) directory /= std::string(48, 'p');
  const auto native_dir = am::paths::native(directory); std::filesystem::create_directories(native_dir);
  const auto path = am::paths::native(directory / "archive.zip"); const auto b = build({Entry{}});
  { std::ofstream file(path, std::ios::binary); ASSERT_TRUE(file); file.write(reinterpret_cast<const char*>(b.bytes.data()), static_cast<std::streamsize>(b.bytes.size())); ASSERT_TRUE(file); }
  auto first = file_source(path); ASSERT_TRUE(first); auto second = file_source(path); ASSERT_TRUE(second);
  auto a = ZipArchive::open(*first); ASSERT_TRUE(a); ReadBudget budget(3); EXPECT_TRUE(a->verify_entry(0, budget));
  std::array<std::byte, 3> data{}; auto eof = (*second)->read_at(std::numeric_limits<std::uint64_t>::max(), data);
  ASSERT_TRUE(eof); EXPECT_EQ(*eof, 0u);
  auto missing = file_source(directory / "absent.zip"); ASSERT_FALSE(missing); EXPECT_EQ(missing.error().message_id, "am.zip.io_error");
  EXPECT_EQ(missing.error().code, am::Errc::io_error);
}
TEST(ZipStream, FourGiBZip64ZerosFullSha256AndPeakMemory) {
  RecordProperty("req", "K-116|ADR-0035");
  constexpr std::uint64_t size = 4ULL * 1024 * 1024 * 1024;
  const auto start = std::chrono::steady_clock::now();
  // Generate without a 4 GiB allocation. Only compressed bytes are retained.
  std::array<std::byte, 65536> zero{}, compressed{};
  Bytes payload;
  z_stream state{}; ASSERT_EQ(deflateInit2(&state, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY), Z_OK);
  struct End { z_stream* state; ~End() { deflateEnd(state); } } end{&state};
  std::uint32_t crc = 0; auto expected_hash = Sha256::create(); ASSERT_TRUE(expected_hash);
  for (std::uint64_t done = 0; done < size; done += zero.size()) {
    state.next_in = reinterpret_cast<Bytef*>(zero.data()); state.avail_in = static_cast<uInt>(zero.size());
    crc = static_cast<std::uint32_t>(::crc32(crc, state.next_in, state.avail_in));
    ASSERT_TRUE(expected_hash->update(zero));
    do {
      state.next_out = reinterpret_cast<Bytef*>(compressed.data()); state.avail_out = static_cast<uInt>(compressed.size());
      ASSERT_EQ(deflate(&state, Z_NO_FLUSH), Z_OK);
      payload.insert(payload.end(), compressed.begin(), compressed.end() - state.avail_out);
    } while (state.avail_in != 0 || state.avail_out == 0);
  }
  int status = Z_OK;
  do {
    state.next_out = reinterpret_cast<Bytef*>(compressed.data()); state.avail_out = static_cast<uInt>(compressed.size());
    status = deflate(&state, Z_FINISH); ASSERT_TRUE(status == Z_OK || status == Z_STREAM_END);
    payload.insert(payload.end(), compressed.begin(), compressed.end() - state.avail_out);
  } while (status != Z_STREAM_END);
  auto expected = expected_hash->finish(); ASSERT_TRUE(expected);
  Entry e; e.method = 8; e.zip64 = true; e.compressed = std::move(payload); e.declared_size = size; e.declared_crc = crc;
  const auto directory = std::filesystem::path(AM_ZIP_CACHE); std::filesystem::create_directories(directory);
  const auto path = directory / "four-gib-zeros.zip";
  std::uint64_t compressed_bytes = 0;
  {
    auto b = build({e}, true); compressed_bytes = b.bytes.size();
    std::ofstream file(path, std::ios::binary); ASSERT_TRUE(file);
    file.write(reinterpret_cast<const char*>(b.bytes.data()), static_cast<std::streamsize>(b.bytes.size())); ASSERT_TRUE(file);
  }
  e.compressed.reset();
  const auto generated = std::chrono::steady_clock::now();
  auto source = file_source(path); ASSERT_TRUE(source); auto a = ZipArchive::open(*source); ASSERT_TRUE(a);
  ASSERT_TRUE(a->entries()[0].has_zip64); ASSERT_EQ(a->entries()[0].uncompressed_size, size);
  ReadBudget budget(size); auto verified = a->verify_entry(0, budget, true); ASSERT_TRUE(verified) << verified.error().ctx.detail;
  EXPECT_EQ(verified->size, size); EXPECT_EQ(verified->crc32, crc); ASSERT_TRUE(verified->sha256);
  EXPECT_EQ(*verified->sha256, *expected); EXPECT_EQ(budget.remaining(), 0u);
  const auto finish = std::chrono::steady_clock::now();
  std::cout << "ZIP64 4GiB: compressed_file_bytes=" << compressed_bytes
      << " generate_seconds=" << std::chrono::duration<double>(generated - start).count()
      << " read_seconds=" << std::chrono::duration<double>(finish - generated).count()
      << " sha256=" << hex(*verified->sha256);
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
  ASSERT_TRUE(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)));
  std::cout << " peak_working_set_bytes=" << memory.PeakWorkingSetSize << " peak_commit_bytes=" << memory.PeakPagefileUsage;
#endif
  std::cout << '\n';
}
