#include <gtest/gtest.h>

#include "am/services/import/exclusion_rules.hpp"

using namespace am::import;
using am::FileKind;

TEST(Exclusion, AppleDoubleWithSignatureIsCertain) {
  ExclusionEngine e;
  auto m = e.evaluate({"Android 18/__MACOSX/option_1/._10_left_leg.stl", 4096, FileKind::apple_double});
  EXPECT_EQ(m.rule_id, "macos.appledouble");
  EXPECT_EQ(m.confidence, Confidence::certain);
}

TEST(Exclusion, AppleDoubleDisguisedAs3mf) {  // "__MACOSX/anime+23/._anime+23.3mf" from errors.txt
  ExclusionEngine e;
  auto m = e.evaluate({"__MACOSX/anime+23/._anime+23.3mf", 4096, FileKind::apple_double});
  EXPECT_EQ(m.confidence, Confidence::certain);
}

TEST(Exclusion, NameOnlyIsJustAProposal) {
  ExclusionEngine e;
  auto m = e.evaluate({"__MACOSX/a/._b.stl", 4096, std::nullopt});
  EXPECT_EQ(m.confidence, Confidence::probable);
}

TEST(Exclusion, RealFileStartingWithDotUnderscoreWithoutSignatureIsKept) {
  ExclusionEngine e;
  auto m = e.evaluate({"models/._weird_but_real.stl", 120000, FileKind::stl_binary});
  EXPECT_EQ(m.confidence, Confidence::none);
}

TEST(Exclusion, DsStoreAndThumbs) {
  ExclusionEngine e;
  EXPECT_EQ(e.evaluate({"a/.DS_Store", 6148, FileKind::ds_store}).confidence, Confidence::certain);
  EXPECT_EQ(e.evaluate({"a/.DS_Store", 6148, FileKind::unknown}).confidence, Confidence::probable);
  EXPECT_EQ(e.evaluate({"a/Thumbs.db", 20000, FileKind::ole_compound}).rule_id, "windows.thumbs_db");
  EXPECT_EQ(e.evaluate({"a/desktop.ini", 120, std::nullopt}).confidence, Confidence::certain);
}

TEST(Exclusion, OrdinaryModelIsKept) {
  ExclusionEngine e;
  EXPECT_EQ(e.evaluate({"Raven/Pre Support/arm1.stl", 1999534, FileKind::stl_binary}).confidence, Confidence::none);
}
