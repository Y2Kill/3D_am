#include <gtest/gtest.h>
#include "am/storage/asset_zip/writer.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <map>

using namespace am::asset_zip;
namespace {
std::string hex(const Sha256Digest& bytes) {
  constexpr char digits[] = "0123456789abcdef"; std::string out;
  for (const auto byte : bytes) { auto c = std::to_integer<unsigned>(byte); out += digits[c >> 4]; out += digits[c & 15]; } return out;
}
std::string digest(std::string_view data) {
  auto hash = Sha256::create(); if (!hash) throw std::runtime_error(hash.error().ctx.detail);
  auto updated = hash->update(std::as_bytes(std::span(data))); if (!updated) throw std::runtime_error(updated.error().ctx.detail);
  auto value = hash->finish(); if (!value) throw std::runtime_error(value.error().ctx.detail); return hex(*value);
}
}
TEST(ZipWriterInterop, GenerateArchivesForIndependentPythonVerification) {
  RecordProperty("req", "ADR-0006|ADR-0035");
  const auto directory = std::filesystem::path(AM_ZIP_CACHE) / "written"; std::filesystem::create_directories(directory);
  nlohmann::json expected = nlohmann::json::object();
  for (const auto file : {"stored.zip", "deflate-unicode.zip", "empty.zip", "many-70000.zip", "raw-descriptor.zip"}) {
    const auto path = directory / file; std::filesystem::remove(path); auto sink = file_sink(path); ASSERT_TRUE(sink);
    auto writer = ZipWriter::create(*sink); ASSERT_TRUE(writer); auto& entries = expected[file]; entries = nlohmann::json::object();
    const std::string name(file);
    if (name == "many-70000.zip") {
      const auto empty_hash = digest(""); EntryOptions o; o.method = CompressionMethod::stored; o.sha256 = false;
      for (int i = 0; i < 70000; ++i) {
        const auto entry_name = "content/" + std::to_string(i); auto e = writer->begin_entry(entry_name, o); ASSERT_TRUE(e); ASSERT_TRUE(e->finish()); entries[entry_name] = empty_hash;
      }
    } else if (name == "raw-descriptor.zip") {
      auto source = file_source(std::filesystem::path(AM_ZIP_FIXTURES) / "python-descriptor.zip"); ASSERT_TRUE(source); auto archive = ZipArchive::open(*source); ASSERT_TRUE(archive);
      std::ifstream input(std::filesystem::path(AM_ZIP_FIXTURES) / "expected.json"); ASSERT_TRUE(input); const auto original = nlohmann::json::parse(input);
      ReadBudget budget(100000);
      for (const auto& entry : archive->entries()) { auto raw = writer->copy_raw(*archive, entry.index, budget); ASSERT_TRUE(raw); entries[entry.raw_name] = original.at("python-descriptor.zip").at("entries").at(entry.raw_name).at("sha256"); }
    } else if (name != "empty.zip") {
      const std::map<std::string, std::string> data{{"manifest.json", "{}"}, {"content/\u6a21\u578b-\u0442\u0435\u0441\u0442.txt", std::string(120000, 'a')}, {"generated/empty", ""}};
      for (const auto& [entry_name, value] : data) {
        EntryOptions o; o.method = name == "stored.zip" ? CompressionMethod::stored : CompressionMethod::deflate; o.uncompressed_size = value.size();
        auto e = writer->begin_entry(entry_name, o); ASSERT_TRUE(e); ASSERT_TRUE(e->write(std::as_bytes(std::span(value)))); ASSERT_TRUE(e->finish()); entries[entry_name] = digest(value);
      }
    }
    auto summary = writer->finish(); ASSERT_TRUE(summary); auto source = file_source(path); ASSERT_TRUE(source); ReadBudget budget(1000000); ASSERT_TRUE(verify_written(*source, *summary, budget));
  }
  std::ofstream output(directory / "expected.json", std::ios::binary); ASSERT_TRUE(output); output << expected.dump(2) << '\n'; ASSERT_TRUE(output);
}
