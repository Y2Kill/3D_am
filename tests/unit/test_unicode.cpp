#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "am/foundation/unicode.hpp"

namespace {

using namespace am::unicode;

// Encode the scalar sequences supplied by the independent UCD test oracle.
std::string utf8(const std::u32string& scalars) {
  std::string out;
  for (auto cp : scalars) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

std::u32string scalars(const std::string& hex) {
  std::istringstream input(hex);
  std::uint32_t cp;
  std::u32string out;
  while (input >> std::hex >> cp) out += static_cast<char32_t>(cp);
  return out;
}

std::string data_line(std::string line) {
  const auto comment = line.find('#');
  if (comment != std::string::npos) line.erase(comment);
  return line;
}

TEST(Unicode, NormalizationConformance) {
  RecordProperty("req", "ADR-0035");
  std::ifstream input(std::filesystem::path(AM_UNICODE_DATA_DIR) / "NormalizationTest.txt");
  ASSERT_TRUE(input.is_open());
  std::vector<bool> part1(0x110000, false);
  std::string line;
  bool in_part1 = false;
  unsigned line_number = 0, rows = 0;
  for (; std::getline(input, line);) {
    ++line_number;
    line = data_line(line);
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    if (line.rfind("@Part", 0) == 0) {
      in_part1 = line.rfind("@Part1", 0) == 0;
      continue;
    }
    SCOPED_TRACE(line_number);
    std::istringstream columns(line);
    std::array<std::string, 5> values;
    for (std::size_t i = 0; i < values.size(); ++i) {
      std::string column;
      ASSERT_TRUE(static_cast<bool>(std::getline(columns, column, ';')));
      const auto points = scalars(column);
      ASSERT_FALSE(points.empty());
      if (in_part1 && i == 0) {
        ASSERT_EQ(points.size(), 1u);
        part1[points[0]] = true;
      }
      values[i] = utf8(points);
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto result = nfc(values[i]);
      ASSERT_TRUE(result);
      ASSERT_EQ(*result, values[i < 3 ? 1 : 3]) << "column " << i + 1;
    }
    ++rows;
  }
  ASSERT_GT(rows, 0u);
  RecordProperty("normalization_rows", static_cast<int>(rows));
  unsigned invariant_scalars = 0;
  for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
    if ((cp >= 0xD800 && cp <= 0xDFFF) || part1[cp]) continue;
    const auto original = utf8(std::u32string(1, cp));
    const auto result = nfc(original);
    ASSERT_TRUE(result) << static_cast<std::uint32_t>(cp);
    ASSERT_EQ(*result, original) << "scalar " << std::hex << static_cast<std::uint32_t>(cp);
    ++invariant_scalars;
  }
  RecordProperty("invariant_scalars", static_cast<int>(invariant_scalars));
}

TEST(Unicode, SimpleFoldingConformance) {
  RecordProperty("req", "ADR-0035");
  std::ifstream input(std::filesystem::path(AM_UNICODE_DATA_DIR) / "CaseFolding.txt");
  ASSERT_TRUE(input.is_open());
  unsigned rows = 0;
  for (std::string line; std::getline(input, line);) {
    std::istringstream columns(data_line(line));
    std::string source, status, target;
    if (!std::getline(columns, source, ';')) continue;
    if (!std::getline(columns, status, ';')) continue;
    if (status != " C" && status != " S") continue;
    ASSERT_TRUE(static_cast<bool>(std::getline(columns, target, ';')));
    const auto result = simple_casefold(utf8(scalars(source)));
    ASSERT_TRUE(result) << source;
    ASSERT_EQ(*result, utf8(scalars(target))) << source;
    ++rows;
  }
  ASSERT_GT(rows, 0u);
  RecordProperty("simple_fold_rows", static_cast<int>(rows));
}

TEST(Unicode, SimpleFoldingExcludesFullAndTurkicMappings) {
  RecordProperty("req", "ADR-0035");
  const std::array<std::pair<std::string, std::string>, 6> cases = {{
      {"ẞ", "ß"}, {"ß", "ß"}, {"K", "k"}, {"İ", "İ"}, {"Σ", "σ"}, {"ς", "σ"}}};
  for (const auto& [source, expected] : cases) {
    const auto result = simple_casefold(source);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, expected);
  }
}

TEST(Unicode, StrictUtf8RejectsInvalidSequences) {
  RecordProperty("req", "ADR-0035");
  for (const std::string invalid : {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF0",
                                    "\xE2\x82", "\x80", "\xC2x", "\xE2\x28\xA1", "\xFF"}) {
    EXPECT_FALSE(is_valid_utf8(invalid));
    const auto normalized = nfc(invalid);
    const auto folded = simple_casefold(invalid);
    ASSERT_FALSE(normalized);
    ASSERT_FALSE(folded);
    EXPECT_EQ(normalized.error().code, am::Errc::invalid_argument);
    EXPECT_EQ(folded.error().code, am::Errc::invalid_argument);
    EXPECT_EQ(normalized.error().message_id, "unicode.invalid_utf8");
  }
}

TEST(Unicode, Utf8AcceptsScalarBoundariesAndEmbeddedNul) {
  RecordProperty("req", "ADR-0035");
  EXPECT_EQ(kUnicodeVersion, "17.0.0");
  EXPECT_TRUE(is_valid_utf8(""));
  for (const auto cp : {0u, 0x7Fu, 0x80u, 0x7FFu, 0x800u, 0xD7FFu, 0xE000u, 0xFFFFu, 0x10000u, 0x10FFFFu})
    EXPECT_TRUE(is_valid_utf8(utf8(std::u32string(1, static_cast<char32_t>(cp)))));
  const auto normalized = nfc(std::string("A\0B", 3));
  const auto folded = simple_casefold(std::string("A\0B", 3));
  ASSERT_TRUE(normalized);
  ASSERT_TRUE(folded);
  EXPECT_EQ(*normalized, std::string("A\0B", 3));
  EXPECT_EQ(*folded, std::string("a\0b", 3));
  ASSERT_TRUE(nfc(""));
  EXPECT_TRUE(nfc("")->empty());
}

TEST(Unicode, HangulOrderingAndCompositionExclusions) {
  RecordProperty("req", "ADR-0035");
  const std::array<std::pair<std::string, std::string>, 5> cases = {{
      {"\u1100\u1161\u11A8", "각"}, {"가\u11A8", "각"}, {"\u0344", "\u0308\u0301"},
      {"a\u0301\u0300", "á\u0300"}, {"a\u0300\u0301", "à\u0301"}}};
  for (const auto& [source, expected] : cases) {
    const auto result = nfc(source);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, expected);
  }
}

TEST(Unicode, CollisionKeyNormalizesAgainAfterFolding) {
  RecordProperty("req", "ADR-0035");
  EXPECT_EQ(path_collision_key("J\u030C"), "ǰ");
  EXPECT_EQ(path_collision_key("CAFÉ"), path_collision_key("cafe\u0301"));
  EXPECT_NE(path_collision_key("STRASSE"), path_collision_key("straße"));
}

TEST(Unicode, InvalidUtf8CollisionKeyPreservesBytesAndFoldsAscii) {
  RecordProperty("req", "ADR-0035");
  EXPECT_EQ(path_collision_key("\xFF" "ABC\xC0\xAF"), "\xFF" "abc\xC0\xAF");
}

}  // namespace
