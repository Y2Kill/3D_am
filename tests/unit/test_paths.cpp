#include <gtest/gtest.h>

#include "am/foundation/paths.hpp"

using namespace am::paths;

TEST(Paths, NormalizeSource) {
  EXPECT_EQ(normalize_source_path(R"(Dinamuuu3D\Winry\\AN_Hips.stl)"), "Dinamuuu3D/Winry/AN_Hips.stl");
  EXPECT_EQ(normalize_source_path("./a//b/"), "a/b");
}

TEST(Paths, MirrorsCleanPaths) {
  ZipPathMapper m("content/i001");
  auto r = m.map(R"(Raven\standard version\arm1(version1).stl)");
  EXPECT_EQ(r.zip_path, "content/i001/Raven/standard version/arm1(version1).stl");
  EXPECT_EQ(r.reason, RenameReason::none);
}

TEST(Paths, SameNameDifferentFoldersStayDistinct) {  // ТЗ 4.3.1: scales must not overwrite each other
  ZipPathMapper m("content/i001/");
  auto a = m.map("28mm/body.stl");
  auto b = m.map("75mm/body.stl");
  EXPECT_NE(a.zip_path, b.zip_path);
  EXPECT_EQ(b.reason, RenameReason::none);
}

TEST(Paths, CaseInsensitiveCollision) {
  ZipPathMapper m("content/i001/");
  auto a = m.map("Part.STL");
  auto b = m.map("part.stl");
  EXPECT_EQ(a.reason, RenameReason::none);
  EXPECT_EQ(b.reason, RenameReason::collision);
  EXPECT_EQ(b.zip_path, "content/i001/part~2.stl");
}

TEST(Paths, TrailingSpaceAndInvalidChars) {
  ZipPathMapper m("content/i001/");
  auto r = m.map("STL Files for 3D Printing /a:b?.stl");  // trailing space dir exists in the collection
  EXPECT_EQ(r.zip_path, "content/i001/STL Files for 3D Printing/a_b_.stl");
  EXPECT_EQ(r.reason, RenameReason::invalid_chars);
}

TEST(Paths, ZipSlipIsNeutralised) {
  ZipPathMapper m("content/i001/");
  auto r = m.map("../../evil.dll");
  EXPECT_EQ(r.reason, RenameReason::unsafe_traversal);
  EXPECT_EQ(r.zip_path.rfind("content/i001/", 0), 0u);
  EXPECT_EQ(r.zip_path.find(".."), std::string::npos);
}

TEST(Paths, ReservedNames) {
  EXPECT_TRUE(is_reserved_windows_name("CON"));
  EXPECT_TRUE(is_reserved_windows_name("com1.txt"));
  EXPECT_FALSE(is_reserved_windows_name("console.stl"));
  ZipPathMapper m("content/i001/");
  EXPECT_EQ(m.map("nul.stl").reason, RenameReason::reserved_name);
}

TEST(Paths, LongPathIsShortenedKeepingExtensionAndUniqueness) {
  ZipPathMapper m("content/i001/");
  std::string dir(150, 'd');
  auto a = m.map(dir + "/" + std::string(120, 'x') + "_A.stl");
  auto b = m.map(dir + "/" + std::string(120, 'x') + "_B.stl");
  EXPECT_EQ(a.reason, RenameReason::path_too_long);
  EXPECT_LE(a.zip_path.size(), ZipPathMapper::kMaxZipPathBytes);
  EXPECT_EQ(a.zip_path.substr(a.zip_path.size() - 4), ".stl");
  EXPECT_NE(a.zip_path, b.zip_path);
}

TEST(Paths, TruncateUtf8NeverSplitsCodePoints) {
  const std::string s = "абв";  // 6 bytes
  EXPECT_EQ(truncate_utf8(s, 3), "а");
  EXPECT_EQ(truncate_utf8(s, 4), "аб");
}

#ifdef _WIN32
TEST(Paths, NativeLongPathPrefix) {
  EXPECT_EQ(native(std::filesystem::path(L"D:/x/y")).wstring(), L"\\\\?\\D:\\x\\y");
  EXPECT_EQ(native(std::filesystem::path(L"\\\\srv\\share\\f")).wstring(), L"\\\\?\\UNC\\srv\\share\\f");
  EXPECT_EQ(native(std::filesystem::path(L"\\\\?\\D:\\x")).wstring(), L"\\\\?\\D:\\x");
}
#endif
