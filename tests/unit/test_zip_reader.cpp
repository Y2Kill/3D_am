#include <gtest/gtest.h>
#include "zip_builder.hpp"
#include "am/storage/asset_zip/reader.hpp"
#include "am/storage/asset_zip/profile.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>

using namespace am::asset_zip;
using namespace zip_test;
namespace {
am::Result<ZipArchive> open(Bytes bytes, ZipLimits limits = {}) {
  auto source = memory_source(std::move(bytes));
  if (!source) return source.error();
  return ZipArchive::open(*source, limits);
}
void rejected(const Built& b, const char* id = "am.zip.invalid_structure") {
  const auto archive = open(b.bytes);
  ASSERT_FALSE(archive);
  EXPECT_EQ(archive.error().message_id, id) << archive.error().ctx.detail;
  EXPECT_FALSE(archive.error().ctx.detail.empty());
}
bool has(const std::vector<ProfileViolation>& violations, std::string_view code) {
  return std::any_of(violations.begin(), violations.end(), [&](const auto& v) { return v.code == code; });
}
}
TEST(ZipReader, CorrectStoredDeflateEmptyCommentsAndZip64) {
  RecordProperty("req", "ADR-0035");
  for (const bool global64 : {false, true}) for (const bool entry64 : {false, true}) {
    Entry a; a.name = "manifest.json"; a.zip64 = entry64;
    Entry b; b.name = "content/b"; b.method = 8; b.data = std::string(120000, 'x'); b.zip64 = entry64;
    Entry empty; empty.name = "generated/empty"; empty.data.clear(); empty.method = 8; empty.zip64 = entry64;
    auto built = build({a, b, empty}, global64);
    auto archive = open(built.bytes); ASSERT_TRUE(archive) << archive.error().ctx.detail;
    EXPECT_EQ(archive->summary().entry_count, 3u); EXPECT_EQ(archive->summary().unaccounted_bytes, 0u);
    EXPECT_EQ(archive->summary().zip64, global64 || entry64);
    auto profile = check_normative_profile(*archive); ASSERT_TRUE(profile); EXPECT_TRUE(profile->empty());
    ReadBudget budget(a.data.size() + b.data.size());
    for (std::size_t i = 0; i < 3; ++i) {
      auto verified = archive->verify_entry(i, budget, true); ASSERT_TRUE(verified) << verified.error().ctx.detail;
      EXPECT_TRUE(verified->sha256); EXPECT_EQ(verified->crc32, archive->entries()[i].crc32);
      auto raw = archive->raw_range(i); ASSERT_TRUE(raw); EXPECT_EQ(raw->offset, built.data[i]);
      EXPECT_EQ(raw->length, archive->entries()[i].compressed_size);
      EXPECT_EQ(archive->entries()[i].has_zip64, entry64);
    }
    EXPECT_EQ(budget.remaining(), 0u);
  }
  for (const std::string comment : {std::string{}, std::string("comment"), std::string(65535, 'x')}) {
    auto archive = open(build({}, false, comment).bytes); ASSERT_TRUE(archive);
    EXPECT_TRUE(archive->entries().empty()); EXPECT_EQ(archive->summary().comment_length, comment.size());
  }
}
TEST(ZipReader, EocdBoundaryLastCandidateAndZip64Pointer) {
  RecordProperty("req", "ADR-0035");
  auto missing = build(); missing.bytes.clear(); rejected(missing);
  auto truncated = build({Entry{}}); truncated.bytes.pop_back(); rejected(truncated);
  auto comment = build({}, false, "abc"); put(comment.bytes, comment.eocd + 20, 2, 2); rejected(comment);
  // A signature inside the comment with a wrong end boundary is ignored.
  std::string fake(30, 'x'); fake.replace(2, 4, "PK\x05\x06", 4);
  ASSERT_TRUE(open(build({}, false, fake).bytes));
  // The last exact-boundary candidate wins, even if an earlier one is valid.
  auto second = build(); const auto nested = build();
  put(second.bytes, second.eocd + 20, nested.bytes.size(), 2);
  second.bytes.insert(second.bytes.end(), nested.bytes.begin(), nested.bytes.end());
  auto last = open(second.bytes); ASSERT_TRUE(last); EXPECT_EQ(last->summary().unaccounted_bytes, 22u);
  for (const auto patch : {0u, 8u, 16u}) {
    auto b = build({Entry{}}, true); put(b.bytes, b.locator + patch, patch == 8 ? std::numeric_limits<std::uint64_t>::max() : 0, patch == 8 ? 8 : 4);
    rejected(b, patch == 16 ? "am.zip.unsupported_feature" : "am.zip.invalid_structure");
  }
  auto no_locator = build({}, true); put(no_locator.bytes, no_locator.locator, 0, 4); rejected(no_locator);
  auto short64 = build({}, true); put(short64.bytes, short64.end64 + 4, 43, 8); rejected(short64);
  auto wrong_record = build({Entry{}}, true); put(wrong_record.bytes, wrong_record.locator + 8, 0, 8); rejected(wrong_record);
  auto mismatch = build({Entry{}}, true); put(mismatch.bytes, mismatch.eocd + 10, 2, 2); rejected(mismatch);
}
TEST(ZipReader, CentralDirectoryCountsRangesSignaturesAndExtras) {
  RecordProperty("req", "ADR-0035");
  const std::vector<std::function<void(Built&)>> patches{
    [](Built& b) { put(b.bytes, b.eocd + 8, 2, 2); put(b.bytes, b.eocd + 10, 2, 2); },
    [](Built& b) { put(b.bytes, b.eocd + 12, 1, 4); },
    [](Built& b) { put(b.bytes, b.eocd + 16, b.bytes.size() + 1, 4); },
    [](Built& b) { put(b.bytes, b.eocd + 12, b.bytes.size(), 4); },
    [](Built& b) { put(b.bytes, b.central[0], 0, 4); },
    [](Built& b) { put(b.bytes, b.central[0] + 28, 4095, 2); },
    [](Built& b) { put(b.bytes, b.central[0] + 24, 0xffffffffULL, 4); }
  };
  for (const auto& patch : patches) { auto b = build({Entry{}}); patch(b); rejected(b, "am.zip.invalid_structure"); }
  Entry e; e.zip64 = true;
  const auto base = build({e});
  const auto extra = base.central[0] + 46 + e.name.size();
  for (const auto& [offset, value] : std::vector<std::pair<std::size_t, std::uint64_t>>{
      {extra, 0x1234}, {extra + 2, 23}, {extra + 2, 25}, {base.central[0] + 30, 3}}) {
    auto b = base; put(b.bytes, offset, value, 2); rejected(b);
  }
  auto reordered = base; put(reordered.bytes, extra + 4, 999, 8); rejected(reordered); // size values no longer match local
  // ZIP64 can supply just one saturated field, not a fixed tuple.
  auto one = base;
  put(one.bytes, one.central[0] + 20, 3, 4); put(one.bytes, one.central[0] + 42, 0, 4);
  one.bytes.erase(one.bytes.begin() + static_cast<std::ptrdiff_t>(extra + 12), one.bytes.begin() + static_cast<std::ptrdiff_t>(extra + 28));
  put(one.bytes, extra + 2, 8, 2); put(one.bytes, one.central[0] + 30, 12, 2); one.eocd -= 16;
  put(one.bytes, one.eocd + 12, one.eocd - one.cd, 4);
  ASSERT_TRUE(open(one.bytes));
}
TEST(ZipReader, LocalHeaderConsistency) {
  RecordProperty("req", "ADR-0035");
  const std::vector<std::pair<std::size_t, std::uint64_t>> patches{{0, 0}, {6, 0x801}, {8, 8}, {14, 123}, {18, 2}, {22, 2}, {30, 'X'}};
  for (const auto& [offset, value] : patches) {
    auto b = build({Entry{}}); put(b.bytes, offset, value, offset == 30 ? 1 : offset == 6 || offset == 8 ? 2 : 4); rejected(b);
  }
  auto bad_offset = build({Entry{}}); put(bad_offset.bytes, bad_offset.central[0] + 42, bad_offset.bytes.size() + 1, 4); rejected(bad_offset);
  Entry e; e.zip64 = true; auto local_extra = build({e});
  put(local_extra.bytes, local_extra.local[0] + 30 + e.name.size() + 2, 15, 2); rejected(local_extra);
  // Only the encryption flags are compared, as required by the mechanism.
  auto non_encryption = build({Entry{}}); put(non_encryption.bytes, 6, 0, 2); EXPECT_TRUE(open(non_encryption.bytes));
}
TEST(ZipReader, OverlappingHeadersDataAndDirectory) {
  RecordProperty("req", "ADR-0035");
  Entry a, b; b.name = "content/b";
  auto shared = build({a, b}); put(shared.bytes, shared.central[1] + 42, shared.local[0], 4); rejected(shared, "am.zip.overlapping_entries");
  auto neighbor = build({a, b});
  put(neighbor.bytes, neighbor.central[0] + 20, neighbor.local[1] - neighbor.data[0] + 1, 4); rejected(neighbor, "am.zip.overlapping_entries");
  auto directory = build({a}); put(directory.bytes, directory.central[0] + 20, 4, 4); rejected(directory, "am.zip.overlapping_entries");
  auto header_in_cd = build({a}); put(header_in_cd.bytes, header_in_cd.central[0] + 42, header_in_cd.cd, 4); rejected(header_in_cd, "am.zip.overlapping_entries");
}
TEST(ZipReader, DuplicateNamesAreStructuralButCaseAndNfcAreProfile) {
  RecordProperty("req", "ADR-0035");
  rejected(build({Entry{}, Entry{}}), "am.zip.duplicate_name");
  std::vector<Entry> entries(5); entries[0].name = "manifest.json"; entries[1].name = "content/A"; entries[2].name = "content/a";
  entries[3].name = "content/caf\u00e9"; entries[4].name = "content/cafe\u0301";
  auto archive = open(build(entries).bytes); ASSERT_TRUE(archive);
  auto profile = check_normative_profile(*archive); ASSERT_TRUE(profile);
  EXPECT_EQ(std::count_if(profile->begin(), profile->end(), [](const auto& v) { return v.code == profile_code::collision; }), 2);
  EXPECT_TRUE(has(*profile, profile_code::path));
  Entry bad1, bad2; bad1.name = "content/A\xff"; bad2.name = "content/a\xff";
  auto invalid_names = open(build({bad1, bad2}).bytes); ASSERT_TRUE(invalid_names);
  auto invalid_profile = check_normative_profile(*invalid_names); ASSERT_TRUE(invalid_profile);
  EXPECT_TRUE(has(*invalid_profile, profile_code::invalid_utf8)); EXPECT_TRUE(has(*invalid_profile, profile_code::collision));
}
TEST(ZipReader, ForeignFeaturesKindsAndAllProfileCodes) {
  RecordProperty("req", "ADR-0035");
  std::vector<Entry> entries(8);
  entries[0].name = "content/dir/"; entries[0].data.clear();
  entries[1].name = "content/link"; entries[1].made_by = 0x314; entries[1].attributes = 0xa1ff0000;
  entries[2].name = "content/fifo"; entries[2].made_by = 0x314; entries[2].attributes = 0x11a40000;
  entries[3].name = "other"; entries[3].method = 12;
  entries[4].name = "content/aes"; entries[4].method = 99; entries[4].flags |= 1;
  entries[5].name = "content/\xff"; entries[5].flags = 0;
  entries[6].name = "content/dd"; entries[6].method = 8; entries[6].flags |= 8;
  entries[7].name = "../slip";
  auto archive = open(build(entries, false, "comment", "SFX").bytes); ASSERT_TRUE(archive);
  EXPECT_EQ(archive->entries()[0].kind, EntryKind::directory); EXPECT_EQ(archive->entries()[1].kind, EntryKind::symlink);
  EXPECT_EQ(archive->entries()[2].kind, EntryKind::special); EXPECT_TRUE(archive->entries()[4].encrypted);
  EXPECT_TRUE(archive->entries()[5].utf8_name.empty()); EXPECT_TRUE(archive->entries()[6].data_descriptor);
  EXPECT_EQ(archive->summary().unaccounted_bytes, 3u);
  auto profile = check_normative_profile(*archive); ASSERT_TRUE(profile);
  for (const auto code : {profile_code::encrypted, profile_code::method, profile_code::descriptor, profile_code::utf8_flag,
      profile_code::invalid_utf8, profile_code::kind, profile_code::path, profile_code::manifest, profile_code::space, profile_code::gaps, profile_code::comment})
    EXPECT_TRUE(has(*profile, code)) << code;
  ReadBudget budget(100);
  for (std::size_t i : {0u, 1u, 2u, 5u, 6u, 7u}) EXPECT_TRUE(archive->verify_entry(i, budget));
  for (std::size_t i : {3u, 4u}) {
    auto stream = archive->open_entry(i, budget); ASSERT_FALSE(stream); EXPECT_EQ(stream.error().code, am::Errc::unsupported);
    EXPECT_EQ(stream.error().message_id, "am.zip.unsupported_feature");
  }
  Entry no_flag; no_flag.flags = 0; auto legacy = open(build({no_flag}).bytes); ASSERT_TRUE(legacy);
  EXPECT_TRUE(legacy->entries()[0].utf8_name.empty()); EXPECT_FALSE(legacy->entries()[0].utf8_flag);
  // Reader profile does not enforce the writer's manifest-first rule.
  Entry manifest; manifest.name = "manifest.json";
  auto reverse = open(build({Entry{}, manifest}).bytes); ASSERT_TRUE(reverse);
  auto clean = check_normative_profile(*reverse); ASSERT_TRUE(clean); EXPECT_TRUE(clean->empty());
  Entry encrypted; encrypted.flags |= 1;
  auto flagged = open(build({encrypted}).bytes); ASSERT_TRUE(flagged); EXPECT_TRUE(flagged->entries()[0].encrypted);
  auto denied = flagged->open_entry(0, budget); ASSERT_FALSE(denied); EXPECT_EQ(denied.error().message_id, "am.zip.unsupported_feature");
}
TEST(ZipReader, MultidiskIsUnsupported) {
  RecordProperty("req", "ADR-0035");
  for (const auto offset : {4u, 6u}) { auto b = build({Entry{}}); put(b.bytes, b.eocd + offset, 1, 2); rejected(b, "am.zip.unsupported_feature"); }
  auto central = build({Entry{}}); put(central.bytes, central.central[0] + 34, 1, 2); rejected(central, "am.zip.unsupported_feature");
  auto global = build({Entry{}}, true); put(global.bytes, global.end64 + 16, 1, 4); rejected(global, "am.zip.unsupported_feature");
}
TEST(ZipReader, LimitsBeforeAllocationAndHugeCountInTinyFile) {
  RecordProperty("req", "ADR-0035");
  const auto normal = build({Entry{}});
  std::vector<ZipLimits> limits(4);
  limits[0].max_entries = 0; limits[1].max_directory_bytes = 1; limits[2].max_name_bytes = 1; limits[3].max_entry_bytes = 2;
  for (const auto& l : limits) { auto a = open(normal.bytes, l); ASSERT_FALSE(a); EXPECT_EQ(a.error().message_id, "am.zip.limit_exceeded"); }
  auto huge = build({}, true); put(huge.bytes, huge.end64 + 24, 0xffffffffULL, 8); put(huge.bytes, huge.end64 + 32, 0xffffffffULL, 8);
  auto a = open(huge.bytes); ASSERT_FALSE(a); EXPECT_EQ(a.error().code, am::Errc::resource_exhausted);
  EXPECT_EQ(a.error().message_id, "am.zip.limit_exceeded"); EXPECT_LT(huge.bytes.size(), 128u);
}
TEST(ZipReader, DataDescriptorsWithAndWithoutSignatureAndZip64) {
  RecordProperty("req", "ADR-0035");
  for (const bool signature : {true, false}) for (const bool zip64 : {true, false}) {
    Entry e; e.flags |= 8; e.method = 8; e.zip64 = zip64; e.descriptor_signature = signature;
    auto b = build({e}); auto a = open(b.bytes); ASSERT_TRUE(a);
    ReadBudget budget(3); ASSERT_TRUE(a->verify_entry(0, budget));
    auto bad = b; put(bad.bytes, bad.descriptor[0] + (signature ? 4 : 0), 123, 4); rejected(bad);
    auto bad_size = b; put(bad_size.bytes, bad_size.descriptor[0] + (signature ? 8 : 4), 999, zip64 ? 8 : 4); rejected(bad_size);
  }
  Entry e; e.flags |= 8; e.zip64 = true;
  auto central_only = build({e});
  const auto extra = central_only.local[0] + 30 + e.name.size();
  central_only.bytes.erase(central_only.bytes.begin() + static_cast<std::ptrdiff_t>(extra), central_only.bytes.begin() + static_cast<std::ptrdiff_t>(extra + 20));
  central_only.cd -= 20; central_only.eocd -= 20;
  put(central_only.bytes, 18, 0, 4); put(central_only.bytes, 22, 0, 4); put(central_only.bytes, 28, 0, 2);
  put(central_only.bytes, central_only.eocd + 16, central_only.cd, 4);
  auto wide = open(central_only.bytes); ASSERT_TRUE(wide); ReadBudget budget(3); EXPECT_TRUE(wide->verify_entry(0, budget));
  e.zip64 = false; auto truncated = build({e});
  truncated.bytes.erase(truncated.bytes.begin() + static_cast<std::ptrdiff_t>(truncated.cd - 6), truncated.bytes.begin() + static_cast<std::ptrdiff_t>(truncated.cd));
  truncated.cd -= 6; truncated.eocd -= 6; put(truncated.bytes, truncated.eocd + 16, truncated.cd, 4); rejected(truncated);
}
namespace {
class ShortSource final : public RandomAccessSource {
 public:
  explicit ShortSource(Bytes bytes) : bytes_(std::move(bytes)) {}
  std::uint64_t size() const noexcept override { return bytes_.size(); }
  am::Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> out) const override {
    if (offset >= bytes_.size()) return std::size_t{0};
    const auto n = std::min({out.size(), bytes_.size() - static_cast<std::size_t>(offset), std::size_t{3}});
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), n, out.begin()); return n;
  }
 private: Bytes bytes_;
};
}
TEST(ZipReader, ShortReadsAndMemoryOwnership) {
  RecordProperty("req", "ADR-0035");
  auto source = std::make_shared<ShortSource>(build({Entry{}}).bytes);
  auto a = ZipArchive::open(source); ASSERT_TRUE(a); ReadBudget budget(3); EXPECT_TRUE(a->verify_entry(0, budget));
  auto bytes = build().bytes; auto memory = memory_source(bytes); ASSERT_TRUE(memory); bytes.clear();
  EXPECT_EQ((*memory)->size(), 22u); std::array<std::byte, 32> buffer{};
  auto short_read = (*memory)->read_at(20, buffer); ASSERT_TRUE(short_read); EXPECT_EQ(*short_read, 2u);
  auto eof = (*memory)->read_at(std::numeric_limits<std::uint64_t>::max(), buffer); ASSERT_TRUE(eof); EXPECT_EQ(*eof, 0u);
}
namespace {
class ThrowingSource final : public RandomAccessSource {
 public:
  Bytes bytes = build({Entry{}}).bytes;
  enum class Failure { none, allocation, io };
  Failure failure = Failure::none;
  std::uint64_t size() const noexcept override { return bytes.size(); }
  am::Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> out) const override {
    if (failure == Failure::allocation) throw std::bad_alloc();
    if (failure == Failure::io) throw std::runtime_error("Injected source read error");
    if (offset >= bytes.size()) return std::size_t{0};
    const auto n = std::min(out.size(), bytes.size() - static_cast<std::size_t>(offset));
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), n, out.begin()); return n;
  }
};
}
TEST(ZipReader, SourceExceptionsBecomeResultsAndStreamErrorsStaySticky) {
  RecordProperty("req", "ADR-0032|ADR-0035");
  auto source = std::make_shared<ThrowingSource>();
  source->failure = ThrowingSource::Failure::allocation;
  auto allocation = ZipArchive::open(source); ASSERT_FALSE(allocation);
  EXPECT_EQ(allocation.error().message_id, "am.zip.out_of_memory"); EXPECT_EQ(allocation.error().code, am::Errc::resource_exhausted);
  source->failure = ThrowingSource::Failure::io;
  auto io = ZipArchive::open(source); ASSERT_FALSE(io); EXPECT_EQ(io.error().message_id, "am.zip.io_error");
  source->failure = ThrowingSource::Failure::none;
  auto a = ZipArchive::open(source); ASSERT_TRUE(a); ReadBudget budget(3); auto stream = a->open_entry(0, budget); ASSERT_TRUE(stream);
  source->failure = ThrowingSource::Failure::io; std::array<std::byte, 3> buffer{};
  auto failure = stream->read(buffer); ASSERT_FALSE(failure); EXPECT_EQ(failure.error().code, am::Errc::io_error);
  source->failure = ThrowingSource::Failure::none;
  auto retry = stream->read(buffer); ASSERT_FALSE(retry); EXPECT_EQ(retry.error().ctx.detail, failure.error().ctx.detail);
  EXPECT_FALSE(stream->verification());
}
