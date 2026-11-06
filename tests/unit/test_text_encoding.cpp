#include <gtest/gtest.h>

#include <vector>

#include "am/foundation/text_encoding.hpp"

using namespace am::text;

namespace {
std::vector<std::uint8_t> bytes(std::initializer_list<int> v) {
  std::vector<std::uint8_t> r;
  for (int x : v) r.push_back(static_cast<std::uint8_t>(x));
  return r;
}
std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }
}  // namespace

TEST(TextEncoding, Utf8Validation) {
  EXPECT_TRUE(is_valid_utf8(std::string_view("plain ascii")));
  EXPECT_TRUE(is_valid_utf8(std::string_view("Azerama – Digimon ✧ 武僧")));
  EXPECT_FALSE(is_valid_utf8(bytes({0xC0, 0xAF})));              // overlong '/'
  EXPECT_FALSE(is_valid_utf8(bytes({0xED, 0xA0, 0x80})));        // surrogate
  EXPECT_FALSE(is_valid_utf8(bytes({0xE2, 0x80})));              // truncated
  EXPECT_FALSE(is_valid_utf8(bytes({0xAF, 0xE0, 0xA8})));        // CP866 bytes
}

// Exactly the mojibake observed in the reference inventory.
TEST(TextEncoding, RepairCp437Mojibake) {
  EXPECT_EQ(repair_cp437_mojibake("Azerama ΓÇô Digimon ΓÇô Lady Devimon"), "Azerama – Digimon – Lady Devimon");
  EXPECT_EQ(repair_cp437_mojibake("plain"), "plain");
  EXPECT_EQ(repair_cp437_mojibake("Café"), "Café");  // 'é' is CP437 0x82: not valid UTF-8 alone -> unchanged
}

TEST(TextEncoding, GuessPrefersValidUtf8) {
  auto g = guess_name_encoding(bytes("Модель – 1_6.stl"));
  EXPECT_TRUE(g.exact);
  EXPECT_EQ(g.codepage, Codepage::utf8);
  EXPECT_EQ(g.utf8, "Модель – 1_6.stl");
}

TEST(TextEncoding, GuessCp866) {
  // "привет.stl" in CP866
  auto g = guess_name_encoding(bytes({0xAF, 0xE0, 0xA8, 0xA2, 0xA5, 0xE2, '.', 's', 't', 'l'}));
  EXPECT_FALSE(g.exact);
  EXPECT_EQ(g.codepage, Codepage::cp866);
  EXPECT_EQ(g.utf8, "привет.stl");
}

TEST(TextEncoding, GuessCp1251) {
  // "привет.stl" in CP1251
  auto g = guess_name_encoding(bytes({0xEF, 0xF0, 0xE8, 0xE2, 0xE5, 0xF2, '.', 's', 't', 'l'}));
  EXPECT_EQ(g.codepage, Codepage::cp1251);
  EXPECT_EQ(g.utf8, "привет.stl");
}

TEST(TextEncoding, AmbiguousShortNameUsesVowels) {
  // "мир" in CP1251 = EC E8 F0 — every byte is also a CP866 letter ("ьшЁ")
  auto g = guess_name_encoding(bytes({0xEC, 0xE8, 0xF0}));
  EXPECT_EQ(g.codepage, Codepage::cp1251);
  EXPECT_EQ(g.utf8, "мир");
  EXPECT_LT(g.confidence, 0.8);
}

TEST(TextEncoding, EncodeDecodeRoundTrip) {
  std::string out;
  ASSERT_TRUE(encode("Привет", Codepage::cp866, out));
  auto span = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(out.data()), out.size());
  EXPECT_EQ(decode(span, Codepage::cp866), "Привет");
  EXPECT_FALSE(encode("武", Codepage::cp1251, out));
}

TEST(TextEncoding, Base64) {
  EXPECT_EQ(to_base64(bytes("")), "");
  EXPECT_EQ(to_base64(bytes("f")), "Zg==");
  EXPECT_EQ(to_base64(bytes("fo")), "Zm8=");
  EXPECT_EQ(to_base64(bytes("foo")), "Zm9v");
  EXPECT_EQ(to_base64(bytes("foobar")), "Zm9vYmFy");
}
