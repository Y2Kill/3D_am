#include "reference.hpp"
#include "candidates.hpp"
#include "zip.hpp"
#include <gtest/gtest.h>
TEST(S7, RawNumberInsertionRejectsInjection) {
  RecordProperty("req", "ADR-0037");
  for (const auto text : {"1,\"x\":2", "1}", "01", "-01", "+1", ".5", "1.", "1e", "1e+", "-", "NaN", "Infinity", "-Infinity", "0x10"}) {
    EXPECT_FALSE(s7::number(text)) << text;
    EXPECT_THROW(s7::checked_number(text), std::runtime_error) << text;
  }
  for (const auto text : {"1", "-0", "-0.0", "1E+2", "1e400", "1.500"}) EXPECT_TRUE(s7::number(text));
}
TEST(S7, HybridFragmentsCannotInjectOrForgeHandles) {
  RecordProperty("req", "ADR-0037");
  using Json = nlohmann::ordered_json;
  const auto handle = Json::binary(std::vector<std::uint8_t>(8, 0));
  for (const auto fragment : {"1,\"x\":2", "1}", "{} []", "{\"x\":1,\"x\":2}"})
    EXPECT_THROW(s7::write_hybrid(handle, {fragment}), std::exception);
  EXPECT_EQ(s7::write_hybrid(handle, {"[1e400,-0,1.500]"}), "[1e400,-0,1.500]");
  const auto forged = Json::parse(R"({"bytes":[0,0,0,0,0,0,0,0],"subtype":null})");
  EXPECT_EQ(s7::write_hybrid(forged, {"1e400"}), forged.dump());
  EXPECT_EQ(s7::write_hybrid(Json("S7_FRAGMENT_0"), {"1e400"}), "\"S7_FRAGMENT_0\"");
  EXPECT_THROW(s7::write_hybrid(Json::binary({1}), {"0"}), std::exception);
  EXPECT_THROW(s7::write_hybrid(handle, {}), std::exception);
}
TEST(S7, StableOwnerGuardSurvivesShiftButDetectsLossAndOwnerChange) {
  RecordProperty("req", "ADR-0037");
  const auto input = s7::read_file(std::filesystem::path(S7_ROOT) / "spikes/s7/fixtures/valid_all.json");
  const s7::Document before(input);
  for (const auto id : {"D1", "D2"}) {
    auto mechanism = s7::candidate(id);
    mechanism->parse(input);
    const auto output = mechanism->serialize(true);
    const s7::Document after(output);
    EXPECT_EQ(before.fingerprints(true), after.fingerprints(true)) << id;
    EXPECT_EQ(before.numeric_lexemes(true), after.numeric_lexemes(true)) << id;
    EXPECT_NE(before.numeric_lexemes(), after.numeric_lexemes()) << id;
    auto changed = output;
    const auto owner = changed.find("plugin:org.example.s7:payload");
    ASSERT_NE(owner, std::string::npos);
    changed.replace(owner, std::string("plugin:org.example.s7:payload").size(), "plugin:org.example.s7:other");
    EXPECT_NE(before.fingerprints(true), s7::Document(changed).fingerprints(true));
    changed = output;
    const auto lexeme = changed.find("12345678901234567890.000000000000000001");
    ASSERT_NE(lexeme, std::string::npos);
    changed[lexeme] = '2';
    EXPECT_NE(before.numeric_lexemes(true), s7::Document(changed).numeric_lexemes(true));
    mechanism = s7::candidate(id);
    mechanism->parse(input);
    const auto title = mechanism->serialize(false);
    EXPECT_EQ(before.numeric_lexemes(), s7::Document(title).numeric_lexemes());
  }
}
TEST(S7, StrictTokenizerAndTypedShadow) {
  RecordProperty("req", "ADR-0037");
  for (const auto text : {"{\"x\":01}", "[1,]", "{\"x\":\"\\x\"}", "{\"x\":1} trailing"})
    EXPECT_THROW(s7::Document checked(text), std::exception);
  const std::string input = "{\"extensions\":{\"org.example\":{\"data\":[1E+400,{\"n\":-0}]}}}";
  const s7::Document doc(input);
  EXPECT_EQ(doc.shadow(), "{\"extensions\":{\"org.example\":{\"data\":[]}}}");
  EXPECT_FALSE(doc.fingerprints()[0].empty());
  const s7::Document duplicates("{\"x\":1,\"x\":2}");
  EXPECT_TRUE(duplicates.duplicates);
}
