#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <set>

#include "am/application/library_service.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/catalog/indexer.hpp"

namespace {
namespace fs = std::filesystem;
namespace cat = am::catalog;
namespace lib = am::library;
namespace repo = am::repository;
template <class T>
T value(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(r).value();
}
void value(am::Result<void> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
}
std::string bytes(const fs::path& p) {
  std::ifstream f(am::paths::native(p), std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
void write(const fs::path& p, const std::string& s) {
  std::ofstream f(am::paths::native(p), std::ios::binary);
  f << s;
  if (!f) throw std::runtime_error("Test write failed");
}
std::string hash(const fs::path& p) {
  auto h = value(am::asset_zip::Sha256::create());
  auto s = bytes(p);
  value(h.update(std::as_bytes(std::span(s))));
  auto digest = value(h.finish());
  return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}
lib::OpenOptions options(lib::Mode mode = lib::Mode::read_write) {
  lib::OpenOptions o;
  o.mode = mode;
  o.capabilities.emplace();
  o.capabilities->mode = repo::StorageMode::full;
  return o;
}
fs::path fresh() {
  auto parent = fs::path(AM_LIBRARY_TEST_ROOT);
  fs::create_directories(parent);
  auto root = parent / am::Uuid::v7().to_string();
  value(lib::Library::init(root));
  return root;
}
fs::path asset_path(const fs::path& root, std::string_view id) {
  return root / "assets" / std::string(id.substr(34)) / (std::string(id) + ".zip");
}
repo::CreateRequest request(const std::string& title) {
  repo::CreateRequest r;
  r.metadata = repo::Json{{"title", title}};
  return r;
}
std::string scalar(lib::Library& l, const std::string& sql) {
  auto c = am::db::Connection::borrow(l.database(lib::DatabaseKind::index));
  auto s = value(c.prepare(sql));
  value(s.step());
  return value(s.text(0));
}
std::string fixture(const std::string& name) {
  repo::ContainerContents contents;
  repo::ContainerVerifyOptions o;
  o.streams = false;
  value(repo::verify_container(fs::path(AM_GOLDEN_FIXTURES) / name / "asset.zip", o, &contents));
  return contents.manifest_text;
}
std::map<fs::path, std::string> snapshot(const fs::path& root, bool zips_only = false) {
  std::map<fs::path, std::string> out;
  for (const auto& e : fs::recursive_directory_iterator(root))
    if (e.is_regular_file()) {
      const auto name = e.path().filename().string();
      if (name.ends_with(".sqlite-wal") || name.ends_with(".sqlite-shm")) continue;
      if (!zips_only || e.path().extension() == ".zip") out.emplace(e.path().lexically_relative(root), hash(e.path()));
    }
  return out;
}
}  // namespace
TEST(Indexer, GoldenFullMapsEverySupportedTableAndNames) {
  RecordProperty("req", "K-3|K-7|ADR-0037");
  const auto root = fresh();
  const auto raw = fixture("pos-full");
  auto doc = value(am::manifest::parse_manifest(raw));
  const auto id = doc.known()["asset"]["id"].get<std::string>();
  const auto target = asset_path(root, id);
  fs::create_directories(target.parent_path());
  fs::copy_file(fs::path(AM_GOLDEN_FIXTURES) / "pos-full/asset.zip", target);
  auto library = value(lib::Library::open(root, options()));
  cat::Indexer index(root, library);
  auto rebuilt = value(index.rebuild());
  EXPECT_EQ(rebuilt.indexed, 1u);
  EXPECT_TRUE(rebuilt.skipped.empty());
  for (const auto& [table, count] : std::map<std::string, std::string>{{"assets", "1"},
                                                                       {"files", "3"},
                                                                       {"input_entries", "3"},
                                                                       {"input_objects", "1"},
                                                                       {"sources", "1"},
                                                                       {"asset_tags", "2"},
                                                                       {"asset_categories", "1"},
                                                                       {"asset_fields", "3"},
                                                                       {"fts_assets", "1"},
                                                                       {"fts_names", "5"}})
    EXPECT_EQ(scalar(library, "SELECT count(*) FROM " + table), count) << table;
  for (const auto* table : {"image_hashes", "mesh_features", "asset_minhash", "fts_docs", "suggestions"})
    EXPECT_EQ(scalar(library, "SELECT count(*) FROM " + std::string(table)), "0");
  EXPECT_EQ(scalar(library, "SELECT manifest_json FROM assets"), raw);
  EXPECT_EQ(scalar(library, "SELECT title FROM assets"), "Winry Rockbell + ALT");
  EXPECT_EQ(scalar(library, "SELECT primary_preview FROM assets"), "4a5ad0ca-eb13-45b3-893b-907c1ba8377f");
  EXPECT_EQ(scalar(library, "SELECT path FROM asset_categories"), "[\"Figures\",\"Anime\"]");
  EXPECT_EQ(scalar(library, "SELECT tag FROM asset_tags WHERE tag='FMA'"), "FMA");
  EXPECT_EQ(scalar(library, "SELECT count(*) FROM fts_assets WHERE fts_assets MATCH 'Dinamuuu3D'"), "1");
  EXPECT_EQ(scalar(library, "SELECT count(*) FROM fts_names WHERE fts_names MATCH 'Hips'"), "2");
  EXPECT_FALSE(value(index.rebuild_required()));
}
TEST(Indexer, ExactPluginFragmentsAndNumericLexemesArePreserved) {
  RecordProperty("req", "K-3|K-7|ADR-0037");
  const auto raw = fixture("pos-exact-numbers");
  auto doc = value(am::manifest::parse_manifest(raw));
  repo::VerificationReport report;
  report.v1 = report.v2 = report.v3 = repo::CheckStatus::passed;
  auto mapped = value(cat::Indexer::map(doc, raw, {"assets/test.zip", "18446744073709551615", 0, false}, report));
  EXPECT_EQ(mapped.zip_size, "18446744073709551615");
  std::size_t plugins = 0;
  for (const auto& f : mapped.fields)
    if (f.type_id && f.type_id->starts_with("plugin:")) {
      ++plugins;
      ASSERT_TRUE(f.txt);
      EXPECT_FALSE(f.num);
      EXPECT_NE(f.txt->find("12345678901234567890.000000000000000001"), std::string::npos);
      EXPECT_NE(f.txt->find("1e400"), std::string::npos);
      EXPECT_NE(raw.find(*f.txt), std::string::npos);
    }
  EXPECT_EQ(plugins, 3u);
  auto json = doc.known();
  json.erase("extensions");
  json["groups"] = repo::Json::array();
  json["files"] = repo::Json::array();
  json["asset"]["metadata"]["custom_values"] =
      repo::Json::array({{{"field_id", "user:integer"}, {"value", "-9223372036854775808"}},
                         {{"field_id", "user:decimal"}, {"value", "12345678901234567890.000000000000000001"}}});
  json["field_definitions"] =
      repo::Json::array({{{"id", "user:integer"}, {"type", "integer"}}, {{"id", "user:decimal"}, {"type", "decimal"}}});
  const auto numeric = json.dump();
  auto numbers = value(am::manifest::parse_manifest(numeric));
  mapped = value(cat::Indexer::map(numbers, numeric, {"test.zip", "1", 0, false}, report));
  ASSERT_EQ(mapped.fields.size(), 2u);
  EXPECT_EQ(mapped.fields[0].txt, "-9223372036854775808");
  EXPECT_TRUE(mapped.fields[0].num);
  EXPECT_EQ(mapped.fields[1].txt, "12345678901234567890.000000000000000001");
  EXPECT_TRUE(mapped.fields[1].num);
  const auto root = fresh();
  auto library = value(lib::Library::open(root, options()));
  am::db::IndexStore store(library.database(lib::DatabaseKind::index));
  value(store.replace(mapped));
  EXPECT_EQ(scalar(library, "SELECT txt FROM asset_fields WHERE field_id='user:decimal'"), *mapped.fields[1].txt);
  mapped.summary.asset_id = am::Uuid::v4().to_string();
  mapped.fields.clear();
  mapped.rel_path = "plugin.zip";
  auto original = value(cat::Indexer::map(doc, raw, {"exact.zip", "1", 0, false}, report));
  value(store.replace(original));
  EXPECT_EQ(scalar(library, "SELECT count(*) FROM asset_fields WHERE type_id LIKE 'plugin:%' AND num IS NULL"), "3");
}
TEST(Indexer, Uint64OverflowAndUnrepresentableNumericFilter) {
  RecordProperty("req", "K-3|ADR-0037");
  auto json = repo::Json::parse(fixture("pos-full"));
  json["files"][0]["size_bytes"] = "18446744073709551615";
  json["files"][1]["size_bytes"] = "18446744073709551615";
  json["inputs"][0]["entries"][0]["size_bytes"] = "18446744073709551615";
  json["inputs"][0]["objects"][0]["size_bytes"] = "18446744073709551615";
  const auto raw = json.dump();
  auto doc = value(am::manifest::parse_manifest(raw));
  repo::VerificationReport report;
  report.v1 = report.v2 = report.v3 = repo::CheckStatus::passed;
  auto record = value(cat::Indexer::map(doc, raw, {"test.zip", "1", 0, false}, report));
  EXPECT_FALSE(record.summary.content_bytes);
  EXPECT_EQ(record.summary.health, "warn");
  ASSERT_TRUE(record.health_detail);
  EXPECT_NE(record.health_detail->find("content_bytes_overflow"), std::string::npos);
  EXPECT_EQ(record.files[0].size, "18446744073709551615");
  EXPECT_EQ(record.files[1].size, "18446744073709551615");
  const auto root = fresh();
  auto library = value(lib::Library::open(root, options()));
  am::db::IndexStore store(library.database(lib::DatabaseKind::index));
  record.zip_size = "18446744073709551615";
  value(store.replace(record));
  EXPECT_EQ(scalar(library, "SELECT typeof(zip_size)||':'||zip_size FROM assets"), "text:18446744073709551615");
  EXPECT_EQ(scalar(library, "SELECT size FROM files ORDER BY length(size) DESC,size DESC LIMIT 1"),
            "18446744073709551615");
  EXPECT_EQ(scalar(library, "SELECT size FROM input_entries ORDER BY length(size) DESC,size DESC LIMIT 1"),
            "18446744073709551615");
  EXPECT_EQ(scalar(library, "SELECT size FROM input_objects"), "18446744073709551615");
  EXPECT_EQ(scalar(library, "SELECT content_bytes IS NULL FROM assets"), "1");
  json["asset"]["metadata"]["custom_values"] =
      repo::Json::array({{{"field_id", "wide"}, {"value", std::string(400, '9')}}});
  json["field_definitions"] = repo::Json::array({{{"id", "wide"}, {"type", "decimal"}}});
  json["files"] = repo::Json::array();
  json["groups"] = repo::Json::array();
  const auto wide = json.dump();
  auto wide_doc = value(am::manifest::parse_manifest(wide));
  auto wide_record = value(cat::Indexer::map(wide_doc, wide, {"test.zip", "1", 0, false}, report));
  EXPECT_FALSE(wide_record.fields[0].num);
  EXPECT_EQ(wide_record.fields[0].txt, std::string(400, '9'));
}
TEST(Indexer, VolumesCategorySegmentsUnitsAndHealth) {
  RecordProperty("req", "K-3|K-7|ADR-0037");
  auto j = repo::Json::parse(fixture("pos-full"));
  j["inputs"][0]["completeness"] = "unknown";
  auto& object = j["inputs"][0]["objects"][0];
  object["kind"] = "multivolume_archive";
  object["volumes"] =
      repo::Json::array({{{"index", 0}, {"name", "folder/archive.part1.rar"}, {"size_bytes", "18446744073709551615"}},
                         {{"index", 1}, {"name", "archive.part2.rar"}, {"missing", true}}});
  j["asset"]["metadata"]["categories"] = repo::Json::array({{{"path", repo::Json::array({"A/B", "C"})}}});
  j["field_definitions"][0]["type"] = "decimal";
  j["asset"]["metadata"]["custom_values"][0]["value"] = "1.500";
  j["asset"]["metadata"]["custom_values"][0]["unit"] = "mm";
  const auto raw = j.dump();
  auto doc = value(am::manifest::parse_manifest(raw));
  repo::VerificationReport report;
  report.v1 = report.v2 = report.v3 = repo::CheckStatus::passed;
  auto record = value(cat::Indexer::map(doc, raw, {"x.zip", "1", 0, false}, report));
  EXPECT_TRUE(record.incomplete);
  ASSERT_EQ(record.objects.size(), 2u);
  EXPECT_EQ(record.objects[0].volume_index, 0);
  EXPECT_EQ(record.objects[0].name, "archive.part1.rar");
  EXPECT_EQ(record.objects[0].size, "18446744073709551615");
  EXPECT_EQ(record.objects[1].volume_index, 1);
  EXPECT_FALSE(record.objects[1].size);
  EXPECT_EQ(record.categories, (std::vector<std::string>{"[\"A/B\",\"C\"]"}));
  EXPECT_EQ(record.fields[0].txt, "1.500");
  EXPECT_EQ(record.fields[0].unit, "mm");
  EXPECT_EQ(record.fields[0].num, 1.5);
  report.v3_findings.push_back(
      {"sample", am::manifest::FindingSeverity::warning, "/asset", "am.catalog.error", "test warning"});
  record = value(cat::Indexer::map(doc, raw, {"x.zip", "1", 0, false}, report));
  EXPECT_EQ(record.summary.health, "warn");
  EXPECT_EQ(record.health_detail, "am.catalog.error");
  report.v3 = repo::CheckStatus::failed;
  record = value(cat::Indexer::map(doc, raw, {"x.zip", "1", 0, false}, report));
  EXPECT_EQ(record.summary.health, "corrupt");
  report.error = am::Error::make(am::Errc::unsupported, "test unsupported").id("am.catalog.error");
  record = value(cat::Indexer::map(doc, raw, {"x.zip", "1", 0, false}, report));
  EXPECT_EQ(record.summary.health, "unsupported");
  report.error.reset();
  report.v3 = repo::CheckStatus::not_performed;
  report.format_support = repo::FormatSupport::newer_minor;
  record = value(cat::Indexer::map(doc, raw, {"x.zip", "1", 0, false}, report));
  EXPECT_EQ(record.summary.health, "warn");
}
TEST(Indexer, RebuildReportsConflictsAndPreservesUserdataAndZips) {
  RecordProperty("req", "K-7|ADR-0007");
  const auto root = fresh();
  std::vector<repo::AssetRef> refs;
  {
    auto library = value(lib::Library::open(root, options()));
    for (int i = 0; i < 4; ++i)
      refs.push_back(value(library.assets().create(request("Asset " + std::to_string(i)))).ref);
    auto userdata = am::db::Connection::borrow(library.database(lib::DatabaseKind::userdata));
    value(userdata.exec("CREATE TABLE sentinel(n); INSERT INTO sentinel VALUES('preserved')"));
  }
  fs::copy_file(asset_path(root, refs[0].asset_id), root / "trash" / (refs[0].asset_id + ".zip"));
  fs::rename(asset_path(root, refs[3].asset_id), root / "trash" / (refs[3].asset_id + ".zip"));
  const auto bad = am::Uuid::v4().to_string();
  fs::create_directories(asset_path(root, bad).parent_path());
  write(asset_path(root, bad), "invalid ZIP");
  const auto mismatch = am::Uuid::v4().to_string();
  fs::create_directories(asset_path(root, mismatch).parent_path());
  fs::copy_file(asset_path(root, refs[0].asset_id), asset_path(root, mismatch));
  write(root / "assets/foreign.txt", "keep");
  write(root / "trash/ignored.txt", "keep");
  const auto userdata_before = hash(root / ".am/userdata.sqlite");
  const auto zip_before = snapshot(root, true);
  {
    auto library = value(lib::Library::open(root, options()));
    cat::Indexer index(root, library);
    const auto result = value(index.rebuild());
    EXPECT_EQ(result.indexed, 4u);
    ASSERT_EQ(result.skipped.size(), 3u);
    EXPECT_EQ(result.ignored.size(), 2u);
    std::set<std::string> reasons;
    for (const auto& s : result.skipped) reasons.insert(s.reason);
    EXPECT_EQ(reasons, (std::set<std::string>{"unreadable", "id_mismatch", "duplicate_id"}));
    EXPECT_EQ(value(index.list()).size(), 3u);
    EXPECT_EQ(value(index.list(true)).size(), 4u);
    EXPECT_FALSE(value(index.rebuild_required()));
    EXPECT_EQ(scalar(library, "SELECT count(*) FROM assets WHERE in_trash=1"), "1");
  }
  EXPECT_EQ(hash(root / ".am/userdata.sqlite"), userdata_before);
  EXPECT_EQ(snapshot(root, true), zip_before);
}
TEST(Indexer, DeletedIndexRebuildRestoresSameRows) {
  RecordProperty("req", "K-7|ADR-0007");
  const auto root = fresh();
  std::vector<am::db::IndexSummary> rows;
  {
    auto library = value(lib::Library::open(root, options()));
    for (int i = 0; i < 3; ++i) value(library.assets().create(request(std::to_string(i))));
    cat::Indexer index(root, library);
    value(index.rebuild());
    rows = value(index.list(true));
  }
  fs::remove(root / ".am/index.sqlite");
  {
    auto library = value(lib::Library::open(root, options()));
    cat::Indexer index(root, library);
    EXPECT_TRUE(value(index.rebuild_required()));
    value(index.rebuild());
    EXPECT_EQ(value(index.list(true)), rows);
  }
}
TEST(Indexer, RebuildRollbackPreservesRowsFlagAndReaderView) {
  RecordProperty("req", "ADR-0007");
  const auto root = fresh();
  auto library = value(lib::Library::open(root, options()));
  for (int i = 0; i < 2; ++i) value(library.assets().create(request(std::to_string(i))));
  cat::Indexer index(root, library);
  value(index.rebuild());
  const auto rows = value(index.list(true));
  value(library.assets().create(request("third")));
  auto reader = value(lib::Library::open(root, options(lib::Mode::read_only)));
  cat::Indexer read_index(root, reader);
  cat::IndexerHooks hooks;
  hooks.after_rebuild_insert = [&](std::size_t n) -> am::Result<void> {
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(value(read_index.list(true)), rows);
    return am::Error::make(am::Errc::io_error, "Injected rebuild failure");
  };
  cat::Indexer failed(root, library, hooks);
  EXPECT_FALSE(failed.rebuild());
  EXPECT_EQ(value(index.list(true)), rows);
  EXPECT_FALSE(value(index.rebuild_required()));
  value(index.rebuild());
  EXPECT_EQ(value(read_index.list(true)).size(), 3u);
}
TEST(Indexer, CommitObserverUpdatesFtsAndFailureKeepsCommittedZip) {
  RecordProperty("req", "ADR-0025|K-7");
  const auto root = fresh();
  bool fail = false;
  cat::IndexerHooks hooks;
  hooks.before_update = [&](std::string_view) -> am::Result<void> {
    if (fail) return am::Error::make(am::Errc::io_error, "Injected index failure");
    return {};
  };
  auto catalog = value(cat::Catalog::open(root, options(), hooks));
  auto created = value(catalog.create(request("BeforeTitle")));
  ASSERT_TRUE(created.warnings.empty());
  ASSERT_EQ(value(catalog.indexer().list()).size(), 1u);
  repo::ChangeRequest change;
  change.ref = created.change.ref;
  change.expected_rev = 1;
  repo::SetMetadata metadata;
  metadata.field = "title";
  metadata.value.emplace("AfterTitle");
  change.ops.push_back(std::move(metadata));
  auto moved = std::move(catalog);
  auto changed = value(moved.change(change));
  EXPECT_EQ(changed.change.revision, 2u);
  EXPECT_TRUE(changed.warnings.empty());
  EXPECT_EQ(scalar(moved.library(), "SELECT title FROM assets"), "AfterTitle");
  EXPECT_EQ(scalar(moved.library(), "SELECT count(*) FROM fts_assets WHERE fts_assets MATCH 'AfterTitle'"), "1");
  EXPECT_EQ(scalar(moved.library(), "SELECT count(*) FROM fts_assets WHERE fts_assets MATCH 'BeforeTitle'"), "0");
  value(moved.indexer().rebuild());
  fail = true;
  auto committed = value(moved.create(request("CommittedWithoutIndex")));
  ASSERT_EQ(committed.warnings.size(), 1u);
  EXPECT_EQ(committed.warnings[0].message_id, "am.catalog.index_update_failed");
  EXPECT_TRUE(value(moved.indexer().rebuild_required()));
  EXPECT_TRUE(fs::exists(asset_path(root, committed.change.ref.asset_id)));
  EXPECT_EQ(value(moved.library().assets().snapshot(committed.change.ref)).revision(), 1u);
  EXPECT_EQ(value(moved.indexer().list()).size(), 1u);
  value(moved.indexer().remove(created.change.ref.asset_id));
  EXPECT_TRUE(value(moved.indexer().list()).empty());
  EXPECT_EQ(scalar(moved.library(), "SELECT count(*) FROM fts_assets"), "0");
}
TEST(Indexer, ReadOnlyListWithWriterPreservesData) {
  RecordProperty("req", "ADR-0031");
  const auto root = fresh();
  auto writer = value(cat::Catalog::open(root, options()));
  value(writer.create(request("Visible")));
  value(writer.indexer().rebuild());
  const auto before = snapshot(root);
  {
    auto reader = value(cat::Catalog::open(root, options(lib::Mode::read_only)));
    EXPECT_TRUE(reader.library().owner().locked);
    EXPECT_EQ(value(reader.indexer().list()).size(), 1u);
    auto listed = value(am::application::list_library(root));
    EXPECT_EQ(listed.value["assets"].size(), 1u);
    EXPECT_FALSE(listed.value["rebuild_required"].get<bool>());
    auto failed = reader.indexer().rebuild();
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, am::Errc::read_only);
  }
  EXPECT_EQ(snapshot(root), before);
}
