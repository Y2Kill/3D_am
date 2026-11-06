#include <gtest/gtest.h>

#include "am/storage/archive/volume_detect.hpp"

using namespace am::archive;

TEST(Volumes, RarPartWithGap) {
  auto r = group_volumes({"Man I Love Figures.part1.rar", "Man I Love Figures.part2.rar",
                          "Man I Love Figures.part4.rar", "preview.jpg"});
  ASSERT_EQ(r.volume_sets.size(), 1u);
  const auto& v = r.volume_sets[0];
  EXPECT_EQ(v.scheme, VolumeScheme::rar_part);
  EXPECT_EQ(v.members.size(), 3u);
  ASSERT_EQ(v.missing_indices.size(), 1u);
  EXPECT_EQ(v.missing_indices[0], 3);
  EXPECT_EQ(r.singles, std::vector<std::string>{"preview.jpg"});
}

// Real case from the inventory: volumes renamed by a download client (Telegram).
TEST(Volumes, CopySuffixVolumesNeedHeaderConfirmation) {
  auto r = group_volumes({"Sylvanas.rar", "Sylvanas (2).rar", "Sylvanas (3).rar", "Sylvanas (4).rar",
                          "Sylvanas (5).rar", "Sylvanas.jpg"});
  ASSERT_EQ(r.volume_sets.size(), 1u);
  const auto& v = r.volume_sets[0];
  EXPECT_EQ(v.scheme, VolumeScheme::copy_suffix);
  EXPECT_TRUE(v.needs_header_confirmation);
  ASSERT_EQ(v.members.size(), 5u);
  EXPECT_EQ(v.members.front(), "Sylvanas.rar");
  EXPECT_EQ(v.members.back(), "Sylvanas (5).rar");
  EXPECT_TRUE(v.missing_indices.empty());
}

TEST(Volumes, LoneCopySuffixDuplicateIsNotASet) {
  auto r = group_volumes({"wednesday-adult (1).zip"});
  EXPECT_TRUE(r.volume_sets.empty());
  EXPECT_EQ(r.singles.size(), 1u);
}

TEST(Volumes, NumericSplit) {
  auto r = group_volumes({"model.7z.001", "model.7z.002", "model.7z.003", "model.7z.004"});
  ASSERT_EQ(r.volume_sets.size(), 1u);
  EXPECT_EQ(r.volume_sets[0].scheme, VolumeScheme::split_numeric);
  EXPECT_EQ(r.volume_sets[0].members.size(), 4u);
}

TEST(Volumes, OldRarAndZipSplitHeads) {
  auto r = group_volumes({"a.rar", "a.r00", "a.r01", "b.z01", "b.z02", "b.zip"});
  ASSERT_EQ(r.volume_sets.size(), 2u);
  for (const auto& v : r.volume_sets) {
    if (v.scheme == VolumeScheme::rar_old) {
      EXPECT_EQ(v.members.front(), "a.rar");
      EXPECT_EQ(v.members.size(), 3u);
    } else {
      EXPECT_EQ(v.scheme, VolumeScheme::zip_split);
      EXPECT_EQ(v.members.back(), "b.zip");
    }
  }
  EXPECT_TRUE(r.singles.empty());
}

// ТЗ §13.2.2: source-specific naming is a profile decision, not a core rule.
TEST(Volumes, CopySuffixSchemeIsConfigurable) {
  const std::vector<std::string> names{"Sylvanas.rar", "Sylvanas (2).rar", "Sylvanas (3).rar"};
  VolumeNamingRules off;
  off.copy_suffix = false;
  auto r = group_volumes(names, off);
  EXPECT_TRUE(r.volume_sets.empty());
  EXPECT_EQ(r.singles.size(), 3u);

  VolumeNamingRules rar_only;
  rar_only.copy_suffix_extensions = {"7z"};
  EXPECT_TRUE(group_volumes(names, rar_only).volume_sets.empty());
  EXPECT_EQ(group_volumes(names).volume_sets.size(), 1u);
}
