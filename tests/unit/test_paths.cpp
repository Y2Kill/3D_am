#include <gtest/gtest.h>

#include <utility>

#include "am/foundation/paths.hpp"

using namespace am::paths;

TEST(Paths, NormalizeSource) {
  EXPECT_EQ(normalize_source_path(R"(Dinamuuu3D\Winry\\AN_Hips.stl)"), "Dinamuuu3D/Winry/AN_Hips.stl");
  EXPECT_EQ(normalize_source_path("./a//b/"), "a/b");
}

TEST(Paths, UnicodeCollisionsPreserveStoredSpelling) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto& m = *created;
  EXPECT_EQ(m.map("Café.stl").reason, RenameReason::none);
  const auto cafe = m.map("cafe\u0301.stl");
  EXPECT_EQ(cafe.reason, RenameReason::collision);
  EXPECT_EQ(cafe.zip_path.rfind(m.root() + "cafe\u0301~", 0), 0u);
  EXPECT_EQ(m.map("ΣΊΣΥΦΟΣ.stl").reason, RenameReason::none);
  EXPECT_EQ(m.map("σίσυφος.stl").reason, RenameReason::collision);
  EXPECT_EQ(m.map("STRASSE.stl").reason, RenameReason::none);
  EXPECT_EQ(m.map("straße.stl").reason, RenameReason::none);
  const auto nfd = m.map("other/e\u0301.stl");
  EXPECT_EQ(nfd.reason, RenameReason::none);
  EXPECT_EQ(nfd.zip_path, m.root() + "other/e\u0301.stl");
}

TEST(Paths, RootCreationEnforcesLengthAfterTrailingSlash) {
  RecordProperty("req", "ADR-0035");
  static_assert(ZipPathMapper::kMaxRootBytes == 190);
  static_assert(ZipPathMapper::kCollisionTagBytes == 9);
  for (const auto& root : {std::string(), std::string(191, 'r'), std::string(190, 'r')}) {
    const auto result = ZipPathMapper::create(root);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, am::Errc::invalid_argument);
  }
  auto created = ZipPathMapper::create(std::string(189, 'r') + '/');
  ASSERT_TRUE(created);
  EXPECT_EQ(created->root().size(), 190u);
  EXPECT_EQ(created->map("a").reason, RenameReason::none);
  const auto collision = created->map("a");
  EXPECT_EQ(collision.reason, RenameReason::collision);
  EXPECT_EQ(collision.zip_path.size(), 200u);
  auto appended = ZipPathMapper::create(std::string(189, 'r'));
  ASSERT_TRUE(appended);
  EXPECT_EQ(appended->root().size(), 190u);
}

TEST(Paths, OversizedExtensionAndRepeatedLongPathStayWithinBudget) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  const auto source = "a." + std::string(169, 'x');  // extension including '.' is 170 bytes
  const auto first = created->map(source);
  const auto second = created->map(source);
  for (const auto& mapped : {first, second}) {
    EXPECT_LE(mapped.zip_path.size(), 200u);
    EXPECT_EQ(mapped.reason, RenameReason::path_too_long);
    EXPECT_EQ(mapped.zip_path.rfind(created->root(), 0), 0u);
  }
  EXPECT_NE(first.zip_path, second.zip_path);
}

TEST(Paths, ExtensionFallsBackToStemWhenRootUsesBudget) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create(std::string(189, 'r') + '/');
  ASSERT_TRUE(created);
  const auto first = created->map("alphabet.stl");
  const auto second = created->map("alphabet.stl");
  for (const auto& mapped : {first, second}) {
    EXPECT_EQ(mapped.zip_path.size(), 200u);
    EXPECT_EQ(mapped.zip_path.rfind(created->root() + "a~", 0), 0u);
    EXPECT_EQ(mapped.reason, RenameReason::path_too_long);
  }
  EXPECT_NE(first.zip_path, second.zip_path);
}

TEST(Paths, CollisionCanDropExtensionWithoutUnsignedUnderflow) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create(std::string(185, 'r') + '/');
  ASSERT_TRUE(created);
  const auto first = created->map("a.longext");
  EXPECT_EQ(first.reason, RenameReason::none);
  const auto second = created->map("a.longext");
  EXPECT_LE(second.zip_path.size(), 200u);
  EXPECT_EQ(second.reason, RenameReason::path_too_long);
  EXPECT_EQ(second.zip_path.rfind(created->root() + "a.lon~", 0), 0u);
}

TEST(Paths, MinimumStemBudgetNeverSplitsUtf8) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create(std::string(189, 'r') + '/');
  ASSERT_TRUE(created);
  EXPECT_EQ(created->map("я").reason, RenameReason::none);
  const auto collision = created->map("я");
  EXPECT_EQ(collision.zip_path.rfind(created->root() + "_~", 0), 0u);
  EXPECT_EQ(collision.zip_path.size(), 200u);
}

TEST(Paths, MirrorsCleanPaths) {
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
  auto r = m.map(R"(Raven\standard version\arm1(version1).stl)");
  EXPECT_EQ(r.zip_path, "content/debdf916-b47c-41db-b4fd-7aa9511396f3/Raven/standard version/arm1(version1).stl");
  EXPECT_EQ(r.reason, RenameReason::none);
}

TEST(Paths, SameNameDifferentFoldersStayDistinct) {  // ТЗ 4.3.1: scales must not overwrite each other
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
  auto a = m.map("28mm/body.stl");
  auto b = m.map("75mm/body.stl");
  EXPECT_NE(a.zip_path, b.zip_path);
  EXPECT_EQ(b.reason, RenameReason::none);
}

TEST(Paths, CaseInsensitiveCollision) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
  auto a = m.map("part.stl");
  auto b = m.map("Part.stl");
  auto c = m.map("Part.stl");
  auto d = m.map("Part.stl");
  EXPECT_EQ(a.reason, RenameReason::none);
  EXPECT_EQ(b.reason, RenameReason::collision);
  EXPECT_EQ(b.zip_path, m.root() + "Part~f1ba0561.stl");
  EXPECT_EQ(c.zip_path, m.root() + "Part~b4a5b811.stl");
  EXPECT_EQ(d.zip_path, m.root() + "Part~b4a5b2f8.stl");
  EXPECT_EQ(c.reason, RenameReason::collision);
  EXPECT_EQ(d.reason, RenameReason::collision);
}

TEST(Paths, CollisionHashUsesNormalizedSourcePath) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
  m.map("part.stl");
  EXPECT_EQ(m.map("./Part.stl/").zip_path, m.root() + "Part~f1ba0561.stl");
}

TEST(Paths, TrailingSpaceAndInvalidChars) {
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
  auto r = m.map("STL Files for 3D Printing /a:b?.stl");  // trailing space dir exists in the collection
  EXPECT_EQ(r.zip_path, "content/debdf916-b47c-41db-b4fd-7aa9511396f3/STL Files for 3D Printing/a_b_.stl");
  EXPECT_EQ(r.reason, RenameReason::invalid_chars);
}

TEST(Paths, ZipSlipIsNeutralised) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
  auto r = m.map("../../evil.dll");
  EXPECT_EQ(r.reason, RenameReason::invalid_chars);
  EXPECT_EQ(r.zip_path.rfind("content/debdf916-b47c-41db-b4fd-7aa9511396f3/", 0), 0u);
  EXPECT_EQ(r.zip_path.find(".."), std::string::npos);
  EXPECT_EQ(r.zip_path, m.root() + "__up__/__up__/evil.dll");
  auto drive = m.map("C:/x/a.stl");
  EXPECT_EQ(drive.reason, RenameReason::invalid_chars);
  EXPECT_EQ(drive.zip_path, m.root() + "x/a.stl");
}

TEST(Paths, RenameReasonStringsMatchFormatDictionary) {
  RecordProperty("req", "ADR-0035");
  EXPECT_EQ(to_string(RenameReason::none), "none");
  EXPECT_EQ(to_string(RenameReason::path_too_long), "path_too_long");
  EXPECT_EQ(to_string(RenameReason::invalid_chars), "invalid_chars");
  EXPECT_EQ(to_string(RenameReason::encoding), "encoding");
  EXPECT_EQ(to_string(RenameReason::collision), "collision");
  EXPECT_EQ(to_string(RenameReason::reserved_name), "reserved_name");
}

TEST(Paths, FirstReasonSurvivesTraversalAndCollision) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
  EXPECT_EQ(m.map("CON/../a.stl").reason, RenameReason::reserved_name);
  EXPECT_EQ(m.map("CON/../a.stl").reason, RenameReason::reserved_name);
  EXPECT_EQ(m.map("CON/C:/b.stl").reason, RenameReason::reserved_name);
  EXPECT_EQ(m.map("a?/CON.stl").reason, RenameReason::invalid_chars);
  EXPECT_EQ(m.map("CON/" + std::string(200, 'x') + ".stl").reason, RenameReason::path_too_long);
}

TEST(Paths, CollisionAtByteLimitPreservesRootExtensionAndUtf8) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
  std::string name;
  for (int i = 0; i < 75; ++i) name += "я";
  name += "a.stl";
  const auto first = m.map(name);
  ASSERT_EQ(first.zip_path.size(), ZipPathMapper::kMaxZipPathBytes);
  EXPECT_EQ(first.reason, RenameReason::none);
  std::string prefix = m.root();
  for (int i = 0; i < 71; ++i) prefix += "я";
  const auto second = m.map(name);
  const auto third = m.map(name);
  for (const auto& r : {second, third}) {
    EXPECT_LE(r.zip_path.size(), ZipPathMapper::kMaxZipPathBytes);
    ASSERT_EQ(r.zip_path.size(), prefix.size() + 9 + 4);
    EXPECT_EQ(r.zip_path.substr(0, prefix.size()), prefix);
    EXPECT_EQ(r.zip_path[prefix.size()], '~');
    EXPECT_EQ(r.zip_path.substr(r.zip_path.size() - 4), ".stl");
    EXPECT_EQ(r.reason, RenameReason::path_too_long);
  }
  EXPECT_NE(second.zip_path, third.zip_path);
}

TEST(Paths, RepeatedLongPathStaysWithinByteLimit) {
  RecordProperty("req", "ADR-0035");
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
  const auto source = std::string(200, 'x') + ".stl";
  const auto first = m.map(source);
  const auto second = m.map(source);
  const auto third = m.map(source);
  for (const auto& r : {first, second, third}) {
    EXPECT_LE(r.zip_path.size(), ZipPathMapper::kMaxZipPathBytes);
    EXPECT_EQ(r.zip_path.rfind(m.root(), 0), 0u);
    EXPECT_EQ(r.zip_path.substr(r.zip_path.size() - 4), ".stl");
    EXPECT_EQ(r.reason, RenameReason::path_too_long);
  }
  EXPECT_NE(first.zip_path, second.zip_path);
  EXPECT_NE(second.zip_path, third.zip_path);
}

TEST(Paths, ReservedNames) {
  EXPECT_TRUE(is_reserved_windows_name("CON"));
  EXPECT_TRUE(is_reserved_windows_name("com1.txt"));
  EXPECT_FALSE(is_reserved_windows_name("console.stl"));
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
  EXPECT_EQ(m.map("nul.stl").reason, RenameReason::reserved_name);
}

TEST(Paths, LongPathIsShortenedKeepingExtensionAndUniqueness) {
  auto created = ZipPathMapper::create("content/debdf916-b47c-41db-b4fd-7aa9511396f3/");
  ASSERT_TRUE(created);
  auto m = std::move(*created);
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
