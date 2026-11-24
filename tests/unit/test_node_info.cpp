#include <gtest/gtest.h>

#include <fstream>

#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"

namespace {
namespace paths = am::paths;
namespace fs = std::filesystem;
struct Case {
  const char* name;
  std::uint32_t attributes, tag;
  paths::NodeKind kind;
  bool placeholder;
};
class NodeInfo : public testing::TestWithParam<Case> {};
TEST_P(NodeInfo, MapsRawMetadataWithoutReadingFileData) {
  RecordProperty("req", "SC-3|K-130");
  const auto c = GetParam();
  const auto info = paths::classify_windows_node({c.attributes, c.tag, 123, 456});
  EXPECT_EQ(info.kind, c.kind);
  EXPECT_EQ(info.cloud_placeholder, c.placeholder);
  EXPECT_EQ(info.size, 123u);
  EXPECT_EQ(info.mtime_ns, 456);
  EXPECT_EQ(info.reparse_tag.has_value(), (c.attributes & 0x400) != 0);
  if (info.reparse_tag) {
    EXPECT_EQ(*info.reparse_tag, c.tag);
  }
}
INSTANTIATE_TEST_SUITE_P(WindowsMetadata, NodeInfo,
                         testing::Values(Case{"NoTag", 0, 0, paths::NodeKind::file, false},
                                         Case{"Directory", 0x10, 0, paths::NodeKind::directory, false},
                                         Case{"Symlink", 0x400, 0xa000000c, paths::NodeKind::link, false},
                                         Case{"MountPoint", 0x410, 0xa0000003, paths::NodeKind::link, false},
                                         Case{"LxSymlink", 0x400, 0xa000001d, paths::NodeKind::link, false},
                                         Case{"Cloud", 0x400, 0x9000001a, paths::NodeKind::file, false},
                                         Case{"CloudOne", 0x400, 0x9000101a, paths::NodeKind::file, false},
                                         Case{"CloudF", 0x400, 0x9000f01a, paths::NodeKind::file, false},
                                         Case{"Dedup", 0x400, 0x80000013, paths::NodeKind::file, false},
                                         Case{"Wci", 0x400, 0x80000018, paths::NodeKind::file, false},
                                         Case{"WciOne", 0x410, 0x90001018, paths::NodeKind::directory, false},
                                         Case{"WciLink", 0x400, 0xa0000027, paths::NodeKind::file, false},
                                         Case{"UnknownTag", 0x400, 0x8000abcd, paths::NodeKind::file, false},
                                         Case{"UnknownDirectory", 0x410, 0x8000abcd, paths::NodeKind::directory, false},
                                         Case{"RecallData", 0x400000, 0, paths::NodeKind::file, true},
                                         Case{"RecallOpen", 0x40000, 0, paths::NodeKind::file, true},
                                         Case{"Offline", 0x1000, 0, paths::NodeKind::file, true},
                                         Case{"RecallCloud", 0x400400, 0x9000001a, paths::NodeKind::file, true},
                                         Case{"RecallDirectory", 0x400010, 0, paths::NodeKind::directory, false},
                                         Case{"OfflineDirectory", 0x1010, 0, paths::NodeKind::directory, false}),
                         [](const auto& case_info) { return case_info.param.name; });

TEST(NodeInfoNative, ReadsOnlyMetadataAndReportsMissingNodes) {
  RecordProperty("req", "SC-3|K-130");
  const auto root = fs::path(AM_NODE_INFO_ROOT) / am::Uuid::v4().to_string();
  fs::create_directories(am::paths::native(root));
  const auto file = root / "metadata.bin";
  {
    std::ofstream out(am::paths::native(file), std::ios::binary);
    out << "hello";
  }
  const auto info = paths::node_info(file);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->kind, paths::NodeKind::file);
  EXPECT_EQ(info->size, 5u);
  EXPECT_TRUE(info->mtime_ns.has_value());
  EXPECT_FALSE(info->cloud_placeholder);
  const auto directory = paths::node_info(root);
  ASSERT_TRUE(directory);
  EXPECT_EQ(directory->kind, paths::NodeKind::directory);
  EXPECT_FALSE(paths::node_info(root / "absent"));
#ifndef _WIN32
  const auto link = root / "link";
  fs::create_symlink(file, link);
  const auto linked = paths::node_info(link);
  ASSERT_TRUE(linked);
  EXPECT_EQ(linked->kind, paths::NodeKind::link);
#endif
}
}  // namespace
