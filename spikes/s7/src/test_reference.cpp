#include "reference.hpp"
#include <gtest/gtest.h>
TEST(S7, RawNumberInsertionRejectsInjection) {
  RecordProperty("req", "ADR-0037");
  for (const auto text : {"1,\"x\":2", "1}", "01", "-01", "+1", ".5", "1.", "1e", "1e+", "-", "NaN", "Infinity", "-Infinity", "0x10"}) {
    EXPECT_FALSE(s7::number(text)) << text;
    EXPECT_THROW(s7::checked_number(text), std::runtime_error) << text;
  }
  for (const auto text : {"1", "-0", "-0.0", "1E+2", "1e400", "1.500"}) EXPECT_TRUE(s7::number(text));
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
