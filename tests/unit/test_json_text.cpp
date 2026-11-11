#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "am/foundation/json_text.hpp"

namespace {
TEST(JsonText, RejectsMalformedGrammarAndEncoding) {
  RecordProperty("req", "ADR-0038");
  std::vector<std::string> invalid = {"", "01", "-01", "+1", ".5", "1.", "1e", "1e+", "-", "NaN", "Infinity", "-Infinity", "0x10",
      "[1,]", "{\"a\":1,}", "/*x*/0", "{'a':1}", "true false", "[1 2]", "{\"a\" 0}", "\"\\x\"", "\"\\uD800\"", "\"\\uDC00\"",
      "\"\\uD800\\u0041\"", "\"\\u00Q0\"", "\"unterminated", "{", "[", "\xef\xbb\xbf{}", "\"\xc0\xaf\"", "\"\xed\xa0\x80\"", "\"\xf4\x90\x80\x80\""};
  for (char c = 0; c < 32; ++c) invalid.push_back(std::string("\"") + c + '"');
  for (const auto& text : invalid) {
    auto parsed = am::json::parse_strict(text);
    ASSERT_FALSE(parsed) << text;
    EXPECT_EQ(parsed.error().code, am::Errc::corrupt_data);
    EXPECT_EQ(parsed.error().message_id, "am.json.invalid_json");
    EXPECT_FALSE(parsed.error().ctx.detail.empty());
  }
}
TEST(JsonText, DecodesStringsAndRejectsEscapedDuplicateKeys) {
  RecordProperty("req", "ADR-0038");
  for (const auto text : {R"({"a":1,"a":2})", R"({"a":1,"\u0061":2})", R"({"\uD83D\uDE00":0,"😀":1})"}) {
    auto result = am::json::parse_strict(text);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().message_id, "am.json.duplicate_key");
  }
  auto result = am::json::parse_strict(R"({"a\/b":"\uD83D\uDE00\n\t\b\f\r\\\"","b":[true,false,null]})");
  ASSERT_TRUE(result);
  ASSERT_EQ(result->nodes[0].children.size(), 2u);
  EXPECT_EQ(result->nodes[1].edge, "a/b");
  EXPECT_EQ(result->nodes[1].decoded, "😀\n\t\b\f\r\\\"");
  EXPECT_EQ(result->nodes[3].index, 0u);
  EXPECT_EQ(result->nodes[5].index, 2u);
}
TEST(JsonText, DeepInputUsesExplicitStackAndConfigurableLimits) {
  RecordProperty("req", "ADR-0038");
  const std::string text = std::string(100000, '[') + '0' + std::string(100000, ']');
  auto limited = am::json::parse_strict(text, {1024, text.size()});
  ASSERT_FALSE(limited);
  EXPECT_EQ(limited.error().code, am::Errc::resource_exhausted);
  EXPECT_EQ(limited.error().message_id, "am.json.limit_exceeded");
  auto accepted = am::json::parse_strict(text, {200000, text.size()});
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted->nodes.size(), 100001u);
  EXPECT_EQ(accepted->nodes.back().begin, 100000u);
  auto too_large = am::json::parse_strict(text, {200000, text.size() - 1});
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error().message_id, "am.json.limit_exceeded");
  EXPECT_TRUE(am::json::parse_strict("0", {0, 1}));
  EXPECT_FALSE(am::json::parse_strict("[]", {0, 2}));
}
TEST(JsonText, PreservesEveryNumberSpanWithoutConversion) {
  RecordProperty("req", "ADR-0038|R-09.8");
  const std::vector<std::string> numbers = {"-0", "-0.0", "1E+2", "1e400", "-1E-400", "1.500",
      "12345678901234567890.000000000000000001", "0." + std::string(10000, '7'), "1e" + std::string(1000, '9')};
  for (const auto& number : numbers) {
    EXPECT_TRUE(am::json::is_json_number(number));
    const auto text = "[ " + number + " ]";
    auto tree = am::json::parse_strict(text);
    ASSERT_TRUE(tree);
    const auto& node = tree->nodes[1];
    EXPECT_EQ(node.kind, am::json::Kind::number);
    EXPECT_EQ(text.substr(node.begin, node.end - node.begin), number);
  }
  for (const auto bad : {"", "01", "+1", "1.", "NaN", "1,\"x\":2", "1}", " 1", "1 "})
    EXPECT_FALSE(am::json::is_json_number(bad));
}
}  // namespace
