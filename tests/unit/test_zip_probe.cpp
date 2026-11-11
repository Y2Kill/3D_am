#include <gtest/gtest.h>
#include "zip_builder.hpp"
#include "am/storage/asset_zip/probe.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace am::asset_zip;
using namespace zip_test;
namespace {
constexpr auto id = "urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128";
std::string manifest(std::string major = "1", std::string format_id = id) {
  return "{\"format\":{\"id\":\"" + format_id + "\",\"version\":{\"major\":" + major + ",\"minor\":0}}}";
}
am::Result<ProbeResult> probe(Bytes bytes, am::manifest::CodecLimits limits = {}) {
  auto source = memory_source(std::move(bytes)); if (!source) return source.error();
  return probe_container(*source, {}, limits);
}
std::string hex(std::span<const std::byte> bytes) {
  constexpr char digits[] = "0123456789abcdef"; std::string out;
  for (const auto byte : bytes) { const auto c = std::to_integer<unsigned>(byte); out += digits[c >> 4]; out += digits[c & 15]; }
  return out;
}
}
TEST(ZipProbe, AllSixOutcomesVersionAndCause) {
  RecordProperty("req", "ADR-0035");
  auto not_zip = probe({std::byte{1}, std::byte{2}}); ASSERT_TRUE(not_zip); EXPECT_EQ(not_zip->outcome, ProbeOutcome::not_zip);
  auto bad = build({Entry{}}); put(bad.bytes, bad.central[0], 0, 4);
  auto invalid = probe(bad.bytes); ASSERT_TRUE(invalid); EXPECT_EQ(invalid->outcome, ProbeOutcome::invalid_zip);
  ASSERT_TRUE(invalid->error); ASSERT_TRUE(invalid->error->cause);
  EXPECT_EQ(invalid->error->root_cause().message_id, "am.zip.invalid_structure");
  auto foreign = probe(build({Entry{}}).bytes); ASSERT_TRUE(foreign); EXPECT_EQ(foreign->outcome, ProbeOutcome::foreign_zip);
  Entry e; e.name = "manifest.json"; e.data = manifest(); e.method = 8;
  auto own = probe(build({e}).bytes); ASSERT_TRUE(own) << own.error().ctx.detail; EXPECT_EQ(own->outcome, ProbeOutcome::own);
  ASSERT_TRUE(own->version); EXPECT_EQ(own->version->major, "1"); EXPECT_EQ(own->version->minor, "0"); EXPECT_FALSE(own->error);
  for (const auto major : {"0", "2", "18446744073709551615", "-1"}) {
    e.data = manifest(major); auto other = probe(build({e}).bytes); ASSERT_TRUE(other);
    EXPECT_EQ(other->outcome, ProbeOutcome::own_unsupported_version); ASSERT_TRUE(other->version); EXPECT_EQ(other->version->major, major);
  }
  e.data = manifest("1", "urn:other"); auto alien = probe(build({e}).bytes); ASSERT_TRUE(alien); EXPECT_EQ(alien->outcome, ProbeOutcome::foreign_zip);
  e.data = "{\"format\":\"" + std::string(id) + "\",";
  auto corrupt = probe(build({e}).bytes); ASSERT_TRUE(corrupt); EXPECT_EQ(corrupt->outcome, ProbeOutcome::own_corrupt);
  ASSERT_TRUE(corrupt->error); EXPECT_TRUE(corrupt->error->cause); EXPECT_NE(corrupt->error->root_cause().message_id.find("json"), std::string::npos);
  e.data = "{\"format\":{\"id\":\"" + std::string(id) + "\"}}";
  auto unreadable = probe(build({e}).bytes); ASSERT_TRUE(unreadable); EXPECT_EQ(unreadable->outcome, ProbeOutcome::own_corrupt);
  auto duplicate = probe(build({e, e}).bytes); ASSERT_TRUE(duplicate); EXPECT_EQ(duplicate->outcome, ProbeOutcome::invalid_zip);
  ASSERT_TRUE(duplicate->error); EXPECT_EQ(duplicate->error->root_cause().message_id, "am.zip.duplicate_name");
}
TEST(ZipProbe, PhysicalFirstEntryMarkerBoundaryAndCodecLimit) {
  RecordProperty("req", "ADR-0035");
  Entry first; first.name = "content/first"; first.data = "abc";
  Entry e; e.name = "manifest.json"; e.data = std::string(65520, ' ') + "{\"id\":\"" + id + "\","; e.method = 8;
  auto physical = build({e, first});
  // Reverse directory order while physical manifest remains first.
  Bytes reversed(physical.bytes.begin() + static_cast<std::ptrdiff_t>(physical.central[1]), physical.bytes.begin() + static_cast<std::ptrdiff_t>(physical.eocd));
  reversed.insert(reversed.end(), physical.bytes.begin() + static_cast<std::ptrdiff_t>(physical.central[0]), physical.bytes.begin() + static_cast<std::ptrdiff_t>(physical.central[1]));
  std::copy(reversed.begin(), reversed.end(), physical.bytes.begin() + static_cast<std::ptrdiff_t>(physical.cd));
  auto found = probe(physical.bytes); ASSERT_TRUE(found); EXPECT_EQ(found->outcome, ProbeOutcome::own_corrupt);
  auto later = probe(build({first, e}).bytes); ASSERT_TRUE(later); EXPECT_EQ(later->outcome, ProbeOutcome::foreign_zip);
  e.data = manifest(); am::manifest::CodecLimits limits; limits.max_bytes = 4;
  auto capped = probe(build({e}).bytes, limits); ASSERT_TRUE(capped); ASSERT_TRUE(capped->error);
  EXPECT_EQ(capped->error->root_cause().code, am::Errc::resource_exhausted);
  EXPECT_EQ(capped->error->root_cause().message_id, "am.zip.limit_exceeded");
  Entry v1 = e; v1.data = "{\"format\":{\"id\":\"" + std::string(id) + "\",\"version\":{\"major\":1.5}}}";
  auto v1_bad = probe(build({v1}).bytes); ASSERT_TRUE(v1_bad); EXPECT_EQ(v1_bad->outcome, ProbeOutcome::own_corrupt);
}
TEST(ZipInterop, PythonZipfileFixturesBytesAndSha256) {
  RecordProperty("req", "ADR-0035");
  const auto root = std::filesystem::path(AM_ZIP_FIXTURES);
  std::ifstream expectations(root / "expected.json"); ASSERT_TRUE(expectations);
  const auto expected = nlohmann::ordered_json::parse(expectations);
  for (const auto& [name, description] : expected.items()) {
    SCOPED_TRACE(name);
    auto source = file_source(root / name); ASSERT_TRUE(source); auto a = ZipArchive::open(*source); ASSERT_TRUE(a) << a.error().ctx.detail;
    const auto& entries = description.at("entries"); ASSERT_EQ(a->entries().size(), entries.size());
    ReadBudget budget(16 * 1024 * 1024);
    for (const auto& entry : a->entries()) {
      const auto& target = entries.at(entry.raw_name);
      auto stream = a->open_entry(entry.index, budget, true); ASSERT_TRUE(stream);
      std::array<std::byte, 101> buffer{}; std::string actual;
      for (;;) { auto n = stream->read(buffer); ASSERT_TRUE(n) << n.error().ctx.detail; if (*n == 0) break; actual += hex(std::span(buffer).first(*n)); }
      EXPECT_EQ(actual, target.at("hex").get<std::string>());
      auto verified = stream->verification(); ASSERT_TRUE(verified); ASSERT_TRUE(verified->sha256);
      EXPECT_EQ(hex(*verified->sha256), target.at("sha256").get<std::string>());
      EXPECT_EQ(entry.data_descriptor, description.at("descriptor").get<bool>());
      EXPECT_EQ(entry.has_zip64, description.at("entry_zip64").get<bool>());
    }
  }
  std::size_t archives = 0;
  for (const auto& file : std::filesystem::directory_iterator(root)) if (file.path().extension() == ".zip") ++archives;
  EXPECT_EQ(archives, expected.size()); // Every committed ZIP has expectations.
}
