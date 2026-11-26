#include <gtest/gtest.h>

#include <array>

#include "am/foundation/unicode.hpp"
#include "am/services/export/categories.hpp"
#include "am/services/export/names.hpp"

namespace {
namespace ex = am::exporting;
constexpr std::string_view kAsset = "74fbcc58-dc05-47d5-a397-c71643404376";

TEST(ExportPolicy, PortableNamesAndFallbackPreserveUnicodeBytes) {
  RecordProperty("req", "K-10|ADR-0005");
  const std::array<std::pair<std::string, std::string>, 11> cases{{
      {"Model", "Model.zip"}, {"A<>:\"/\\|?*B", "A_________B.zip"},
      {"A\x01 B.  ", "A_ B.zip"}, {"CON", "_CON.zip"}, {"pRn.txt", "_pRn.txt.zip"},
      {"com\xc2\xb9.zip", "_com\xc2\xb9.zip.zip"}, {"LPT\xc2\xb2", "_LPT\xc2\xb2.zip"},
      {"lpt\xc2\xb3", "_lpt\xc2\xb3.zip"}, {"... ", std::string(kAsset) + ".zip"},
      {"", std::string(kAsset) + ".zip"}, {"e\xcc\x81", "e\xcc\x81.zip"}}};
  for (const auto& [title, expected] : cases) {
    SCOPED_TRACE(title);
    auto result = ex::export_name(title, kAsset);
    ASSERT_TRUE(result) << am::to_log_string(result.error());
    EXPECT_EQ(result->filename, expected);
    EXPECT_EQ(result->sanitized, expected != title + ".zip");
  }
}

TEST(ExportPolicy, CompleteFilenameFitsAtCodepointBoundaryIncludingSuffixes) {
  RecordProperty("req", "K-10|NFR-14.5.6");
  const std::array presets{ex::Preset::all, ex::Preset::model, ex::Preset::model_preview, ex::Preset::slicer};
  for (std::string_view codepoint : {"a", "\xc3\xa9", "\xe6\xa8\xa1", "\xf0\x9f\x98\x80"}) {
    std::string title;
    for (unsigned i = 0; i < 350; ++i) title += codepoint;
    auto result = ex::export_name(title, kAsset, presets, false, {}, 99);
    ASSERT_TRUE(result);
    EXPECT_LE(result->filename.size(), ex::kMaxNameUtf8Bytes);
    EXPECT_TRUE(am::unicode::is_valid_utf8(result->filename));
    EXPECT_TRUE(result->filename.ends_with(" (3d+3d_preview+slicer+all) (99).zip"));
    EXPECT_TRUE(result->sanitized);
    const auto stem_size = result->filename.find(" (3d");
    EXPECT_EQ(stem_size % codepoint.size(), 0u);
  }
}

TEST(ExportPolicy, ExplicitNameIsValidatedWithoutRepair) {
  RecordProperty("req", "K-10|NFR-14.5.6");
  for (std::string name : {"", "CON", "com\xc2\xb9.zip", "A/B", "A\\B", "..", ".zip", "A.", "A .zip",
                           "A\nB", "A:", "A?B"}) {
    SCOPED_TRACE(name);
    auto result = ex::export_name("Ignored", kAsset, {}, false, name);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().message_id, "am.export.name_invalid");
  }
  for (auto length : {252u, 256u}) {
    auto result = ex::export_name("Ignored", kAsset, {}, false, std::string(length, 'x'));
    EXPECT_FALSE(result);
  }
  auto plain = ex::export_name("Ignored", kAsset, {}, false, "Name");
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain->filename, "Name.zip");
  EXPECT_FALSE(plain->sanitized);
  auto uppercase = ex::export_name("Ignored", kAsset, {}, false, "e\xcc\x81.ZIP");
  ASSERT_TRUE(uppercase);
  EXPECT_EQ(uppercase->filename, "e\xcc\x81.ZIP");
  EXPECT_FALSE(uppercase->sanitized);
  EXPECT_FALSE(ex::export_name(std::string("\xff"), kAsset));
}

TEST(ExportPolicy, PresetsHaveStableOrderAndCollisionSuffix) {
  RecordProperty("req", "K-10|K-116");
  const std::array presets{ex::Preset::all, ex::Preset::slicer, ex::Preset::model, ex::Preset::all};
  EXPECT_EQ(ex::preset_suffix(presets), "3d+slicer+all");
  auto result = ex::export_name("A", kAsset, presets, false, {}, 2);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->filename, "A (3d+slicer+all) (2).zip");
  auto files = ex::export_name("A", kAsset, {}, true);
  ASSERT_TRUE(files);
  EXPECT_EQ(files->filename, "A (files).zip");
  auto explicit_name = ex::export_name("A", kAsset, presets, true, "Result.zip", 2);
  ASSERT_TRUE(explicit_name);
  EXPECT_EQ(explicit_name->filename, "Result (2).zip");
  EXPECT_FALSE(ex::export_name("A", kAsset, {}, false, {}, 100));
  EXPECT_FALSE(ex::export_name("A", kAsset, {}, false, {}, 0));
  for (auto preset : {ex::Preset::model, ex::Preset::model_preview, ex::Preset::slicer, ex::Preset::all}) {
    const auto parsed = ex::parse_preset(ex::preset_name(preset));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, preset);
  }
  EXPECT_FALSE(ex::parse_preset("MODEL"));
}

TEST(ExportPolicy, CategoryVersionOneUsesTypeBeforeFilename) {
  RecordProperty("req", "K-10|K-46");
  EXPECT_EQ(ex::kCategoriesVersion, 1u);
  const auto model = ex::categories("mesh.stl.binary", "content/id/texture.png");
  EXPECT_TRUE(model.model);
  EXPECT_TRUE(model.slicer);
  EXPECT_FALSE(model.image);
  const auto image = ex::categories("image.png", "content/id/model.stl");
  EXPECT_TRUE(image.image);
  EXPECT_FALSE(image.model);
  EXPECT_FALSE(image.slicer);
  const auto known_other = ex::categories("other.format", "content/id/model.stl");
  EXPECT_FALSE(known_other.model);
  EXPECT_FALSE(known_other.slicer);
  EXPECT_FALSE(ex::categories("", "a.stl").model);
  EXPECT_TRUE(ex::categories("slice.ctb", "a.bin").slicer);
  EXPECT_TRUE(ex::categories("gcode", "a.bin").slicer);
  EXPECT_TRUE(ex::categories("mesh.obj", "a.bin").slicer);
  EXPECT_FALSE(ex::categories("mesh.stl", "a.stl").slicer);
  for (std::string_view extension : {"STL", "obj", "MTL", "3mf", "ply", "amf", "fbx", "gltf", "glb", "dae",
                                     "off", "3ds", "step", "stp", "iges", "igs", "blend"}) {
    const auto value = ex::categories({}, "content/id/file." + std::string(extension));
    EXPECT_TRUE(value.model) << extension;
  }
  for (std::string_view extension : {"PNG", "jpg", "jpeg", "webp", "gif", "bmp", "tif", "tiff"})
    EXPECT_TRUE(ex::categories({}, "a." + std::string(extension)).image) << extension;
  for (std::string_view extension : {"stl", "3mf", "obj", "lys", "chitubox", "ctb", "GCODE"})
    EXPECT_TRUE(ex::categories({}, "a." + std::string(extension)).slicer) << extension;
  EXPECT_FALSE(ex::categories({}, "directory.stl/no_extension").model);
  EXPECT_FALSE(ex::categories({}, "file.").model);
}

TEST(ExportPolicy, PreviewListAndUnionSelectUncategorizedFiles) {
  RecordProperty("req", "K-10|K-46");
  const std::array models{ex::Preset::model};
  const std::array previews{ex::Preset::model_preview};
  const std::array all{ex::Preset::all};
  const std::array combined{ex::Preset::model, ex::Preset::slicer};
  EXPECT_TRUE(ex::selected({}, true, previews));
  EXPECT_FALSE(ex::selected({}, true, models));
  EXPECT_TRUE(ex::selected({}, false, all));
  EXPECT_TRUE(ex::selected({true, false, false}, false, combined));
  EXPECT_TRUE(ex::selected({false, false, true}, false, combined));
  EXPECT_FALSE(ex::selected({false, true, false}, false, combined));
  EXPECT_FALSE(ex::selected({true, true, true}, true, {}));
}
TEST(ExportPolicy, DelAndC1AreSanitizedAsCodepointsAndRejectedInExplicitName) {
  RecordProperty("req", "K-10|NFR-14.5.6");
  for (const std::string control : {"\x7f", "\xc2\x80", "\xc2\x9f"}) {
    const auto title = "A" + control + "B";
    auto result = ex::export_name(title,kAsset);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->filename,"A_B.zip");
    EXPECT_TRUE(result->sanitized);
    result = ex::export_name({},kAsset,{},false,title);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().message_id,"am.export.name_invalid");
  }
}
}  // namespace
