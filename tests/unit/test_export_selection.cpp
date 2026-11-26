#include <gtest/gtest.h>

#include <array>

#include "am/services/export/selection.hpp"

namespace {
namespace ex = am::exporting;
// This policy fixture deliberately needs no ZIP: V1/V2/V3 validation belongs
// to the repository; selection reads only the retained manifest's known DOM.
constexpr std::string_view kManifest = R"({"format":{"id":"am-asset","major":1,"minor":0},
  "files":[
    {"id":"model","storage_path":"content/a/model.stl"},
    {"id":"image","storage_path":"content/a/picture.png","type":{"id":"image.png"}},
    {"id":"preview","storage_path":"generated/a/preview.bin"},
    {"id":"slicer","storage_path":"content/a/output.ctb","type":{"id":"slice.ctb"}},
    {"id":"other","storage_path":"content/a/other.bin"}],
  "historical_files":[{"id":"removed","storage_path":"content/a/old.stl"}],
  "previews":{"items":[{"id":"preview-id","file_id":"preview"}]}})";

TEST(ExportSelection, CurrentFilesAndPreviewsFollowUnionInManifestOrder) {
  RecordProperty("req", "K-10|K-46");
  auto document = am::manifest::parse_manifest(kManifest);
  ASSERT_TRUE(document) << am::to_log_string(document.error());
  const std::array preview{ex::Preset::model_preview};
  const std::array ids{std::string("other"), std::string("model"), std::string("other")};
  auto result = ex::select_files(*document, preview, ids);
  ASSERT_TRUE(result) << am::to_log_string(result.error());
  EXPECT_EQ(*result, (std::vector<std::size_t>{0, 1, 2, 4}));
  const std::array slicer{ex::Preset::slicer};
  result = ex::select_files(*document, slicer, {});
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, (std::vector<std::size_t>{0, 3}));
  const std::array all{ex::Preset::all};
  result = ex::select_files(*document, all, {});
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, (std::vector<std::size_t>{0, 1, 2, 3, 4}));
}

TEST(ExportSelection, UnknownAndHistoricalIdsFailEvenAlongsideAllPreset) {
  RecordProperty("req", "K-10|K-46");
  auto document = am::manifest::parse_manifest(kManifest);
  ASSERT_TRUE(document);
  const std::array all{ex::Preset::all};
  for (const std::string id : {"unknown", "removed"}) {
    const std::array requested{id};
    auto result = ex::select_files(*document, all, requested);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().message_id, "am.export.file_unknown");
    EXPECT_EQ(result.error().ctx.detail, id);
  }
  auto empty = ex::select_files(*document, {}, {});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().message_id, "am.export.selection_empty");
  auto no_files = am::manifest::parse_manifest(R"({"files":[]})");
  ASSERT_TRUE(no_files);
  empty = ex::select_files(*no_files, all, {});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().message_id, "am.export.selection_empty");
}
}  // namespace
