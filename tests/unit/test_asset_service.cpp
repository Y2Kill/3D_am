#include <gtest/gtest.h>
#include "am/application/asset_service.hpp"
#include "am/storage/repository/repository.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include <fstream>
#include <set>

namespace {
namespace app = am::application;
namespace repo = am::repository;
using namespace am;
using Path = std::filesystem::path;
using Json = app::Json;
template <class T>
T value(Result<T> r) {
  if (!r) throw std::runtime_error(to_log_string(r.error()));
  return std::move(r).value();
}
void value(Result<void> r) {
  if (!r) throw std::runtime_error(to_log_string(r.error()));
}
Path fresh() {
  auto p = Path(AM_ASSET_SERVICE_TEST_ROOT) / Uuid::v4().to_string();
  std::filesystem::create_directories(p);
  return p;
}
void write(const Path& p, std::string_view s) {
  std::ofstream out(paths::native(p), std::ios::binary);
  out.write(s.data(), static_cast<std::streamsize>(s.size()));
  if (!out) throw std::runtime_error("Fixture write failed");
}
std::string bytes(const Path& p) {
  std::ifstream in(paths::native(p), std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
app::CreateAssetOptions inputs(const Path& root) {
  app::CreateAssetOptions o;
  o.output = root / "asset.zip";
  o.files = {root / "one.txt", root / "two.bin"};
  write(o.files[0], "alpha");
  write(o.files[1], "beta");
  o.title = "Title";
  return o;
}
void no_stage(const Path& root) {
  for (const auto& p : std::filesystem::directory_iterator(root))
    EXPECT_FALSE(p.path().filename().string().starts_with(".am-create-"));
}
void stream(asset_zip::ZipWriter& writer, std::string_view name, std::string_view data) {
  asset_zip::EntryOptions o;
  o.uncompressed_size = data.size();
  auto s = value(writer.begin_entry(name, o));
  value(s.write(std::as_bytes(std::span(data))));
  value(s.finish());
}
Path rewrite(const Path& source, const Path& output, const std::function<void(Json&)>& mutation) {
  repo::ContainerContents old;
  value(repo::verify_container(source, {}, &old));
  auto j = Json::parse(old.manifest_text);
  mutation(j);
  auto sink = std::make_shared<asset_zip::MemorySink>();
  auto writer = value(asset_zip::ZipWriter::create(sink));
  stream(writer, "manifest.json", j.dump());
  for (const auto& entry : old.archive->entries())
    if (entry.raw_name != "manifest.json") value(writer.copy_raw(*old.archive, entry.index, *old.budget));
  value(writer.finish());
  const auto& data = sink->bytes();
  std::ofstream out(paths::native(output), std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out) throw std::runtime_error("Fixture ZIP write failed");
  return output;
}
}  // namespace
TEST(AssetService, CreateInspectValidateVerifyExtractRoundTrip) {
  RecordProperty("req", "K-15|K-85|K-3|K-6");
#ifdef _WIN32
  auto root = fresh();
  auto o = inputs(root);
  auto created = value(app::create_asset(o));
  EXPECT_EQ(created.status, app::ExitCode::success);
  EXPECT_EQ(created.value["file_count"], 2);
  no_stage(root);
  auto info = value(app::inspect_asset(o.output));
  EXPECT_EQ(info.value["title"], "Title");
  ASSERT_EQ(info.value["files"].size(), 2u);
  EXPECT_EQ(info.value["files"][0]["size_bytes"], "5");
  EXPECT_EQ(info.value["files"][0]["origin_kind"], "user_added");
  EXPECT_EQ(value(app::validate_asset(o.output)).status, app::ExitCode::success);
  EXPECT_EQ(value(app::verify_asset(o.output)).status, app::ExitCode::success);
  app::ExtractAssetOptions extract;
  extract.directory = root / "all";
  extract.manifest = true;
  auto result = value(app::extract_asset(o.output, extract));
  EXPECT_EQ(result.status, app::ExitCode::success);
  EXPECT_EQ(result.value["file_count"], 2);
  for (std::size_t i = 0; i < 2; ++i)
    EXPECT_EQ(bytes(extract.directory / Path(info.value["files"][i]["path"].get<std::string>())), bytes(o.files[i]));
  repo::ContainerContents contents;
  value(repo::verify_container(o.output, {}, &contents));
  EXPECT_EQ(bytes(extract.directory / "manifest.json"), contents.manifest_text);
  extract.directory = root / "selected";
  extract.manifest = false;
  extract.file_ids = {info.value["files"][0]["id"]};
  EXPECT_EQ(value(app::extract_asset(o.output, extract)).value["file_count"], 1);
#else
  auto root = fresh();
  auto o = inputs(root);
  auto r = app::create_asset(o);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().message_id, "am.repo.storage_read_only");
  no_stage(root);
#endif
}
TEST(AssetService, CreateRefusalsAndPublishRaceNeverReplaceOutput) {
  RecordProperty("req", "K-15|K-85|ADR-0025");
#ifdef _WIN32
  auto root = fresh();
  auto o = inputs(root);
  write(o.output, "existing");
  auto bad = app::create_asset(o);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().message_id, "am.app.output_exists");
  EXPECT_EQ(bytes(o.output), "existing");
  no_stage(root);
  std::filesystem::remove(o.output);
  o.files = {root};
  bad = app::create_asset(o);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().message_id, "am.app.directory_not_supported");
  no_stage(root);
  o.files = {root / "missing"};
  EXPECT_FALSE(app::create_asset(o));
  no_stage(root);
  o = inputs(root);
  o.before_publish = [](const Path& target) { write(target, "racing owner"); };
  bad = app::create_asset(o);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().message_id, "am.app.output_exists");
  EXPECT_EQ(bytes(o.output), "racing owner");
  no_stage(root);
#else
  GTEST_SKIP() << "Create requires fixed NTFS";
#endif
}
TEST(AssetService, PostCreateFailureDeletesOnlyTheNewOutput) {
  RecordProperty("req", "K-15|K-85|K-6");
#ifdef _WIN32
  auto root = fresh();
  auto o = inputs(root);
  o.after_publish = [](const Path& path) { write(path, "bad ZIP"); };
  auto bad = app::create_asset(o);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().message_id, "am.app.post_create_verification");
  EXPECT_EQ(app::error_exit_code(bad.error()), app::ExitCode::check_failed);
  EXPECT_FALSE(std::filesystem::exists(o.output));
  no_stage(root);
#else
  GTEST_SKIP() << "Create requires fixed NTFS";
#endif
}
TEST(AssetService, ValidationAndExtractionUseSharedV3AndPreserveSuccessfulFiles) {
  RecordProperty("req", "K-15|K-85|K-3|K-6");
#ifdef _WIN32
  auto root = fresh();
  auto o = inputs(root);
  value(app::create_asset(o));
  auto invalid = rewrite(o.output, root / "invalid.zip",
                         [](Json& j) { j["asset"]["metadata"]["license"] = {{"spdx", "MIT OR OR BSD"}}; });
  auto r = value(app::validate_asset(invalid));
  EXPECT_EQ(r.status, app::ExitCode::check_failed);
  EXPECT_EQ(r.value["v3_findings"][0]["rule"], "license_spdx");
  app::ExtractAssetOptions extract;
  extract.directory = root / "invalid";
  EXPECT_EQ(value(app::extract_asset(invalid, extract)).status, app::ExitCode::check_failed);
  EXPECT_FALSE(std::filesystem::exists(extract.directory));
  auto partial = rewrite(o.output, root / "units.zip", [](Json& j) {
    const auto id = "user:" + Uuid::v4().to_string();
    j["field_definitions"] = Json::array({Json{{"id", id},
                                               {"schema_revision", 1},
                                               {"name", "Length"},
                                               {"type", "decimal"},
                                               {"scopes", Json::array({"asset"})},
                                               {"dimension", "length"},
                                               {"constraints", {{"maximum", {{"value", "1"}, {"unit", "cm"}}}}}}});
    j["asset"]["metadata"]["custom_values"] = Json::array({Json{{"field_id", id}, {"value", "2"}, {"unit", "mm"}}});
  });
  EXPECT_EQ(value(app::validate_asset(partial)).status, app::ExitCode::partial);
  EXPECT_EQ(value(app::verify_asset(partial)).status, app::ExitCode::partial);
  auto mismatch =
      rewrite(o.output, root / "mismatch.zip", [](Json& j) { j["files"][0]["sha256"] = std::string(64, '0'); });
  EXPECT_EQ(value(app::validate_asset(mismatch)).status, app::ExitCode::success);
  extract.directory = root / "mismatch";
  r = value(app::extract_asset(mismatch, extract));
  EXPECT_EQ(r.status, app::ExitCode::check_failed);
  ASSERT_EQ(r.value["failed"].size(), 1u);
  EXPECT_EQ(r.value["file_count"], 1);
  auto info = value(app::inspect_asset(mismatch));
  EXPECT_FALSE(std::filesystem::exists(extract.directory / Path(info.value["files"][0]["path"].get<std::string>())));
  EXPECT_TRUE(std::filesystem::exists(extract.directory / Path(info.value["files"][1]["path"].get<std::string>())));
  extract.directory = root / "unknown";
  extract.file_ids = {Uuid::v4().to_string()};
  auto unknown = app::extract_asset(o.output, extract);
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().message_id, "am.app.unknown_file_id");
  EXPECT_FALSE(std::filesystem::exists(extract.directory));
  app::VerifyAssetOptions selected;
  selected.level = app::VerificationLevel::selective;
  auto usage = app::verify_asset(o.output, selected);
  ASSERT_FALSE(usage);
  EXPECT_EQ(app::error_exit_code(usage.error()), app::ExitCode::usage);
#else
  GTEST_SKIP() << "Fixture creation requires fixed NTFS; process tests cover read-only fixtures";
#endif
}
TEST(AssetService, StreamScopesMinorAndCorruption) {
  RecordProperty("req", "K-15|K-85|K-3|K-6");
#ifdef _WIN32
  auto root = fresh();
  auto o = inputs(root);
  value(app::create_asset(o));
  auto info = value(app::inspect_asset(o.output));
  app::VerifyAssetOptions requested;
  requested.level = app::VerificationLevel::quick;
  EXPECT_EQ(value(app::verify_asset(o.output, requested)).status, app::ExitCode::partial);
  requested.level = app::VerificationLevel::selective;
  requested.file_ids = {info.value["files"][0]["id"]};
  EXPECT_EQ(value(app::verify_asset(o.output, requested)).status, app::ExitCode::partial);
  requested.file_ids.push_back(info.value["files"][1]["id"]);
  EXPECT_EQ(value(app::verify_asset(o.output, requested)).status, app::ExitCode::success);
  auto newer = rewrite(o.output, root / "newer.zip", [](Json& j) { j["format"]["version"]["minor"] = 1; });
  auto inspection = value(app::inspect_asset(newer));
  EXPECT_EQ(inspection.status, app::ExitCode::partial);
  EXPECT_EQ(inspection.value["v3"], "not_performed");
  std::uint64_t offset = 0, manifest_offset = 0;
  {
    repo::ContainerContents contents;
    value(repo::verify_container(o.output, {}, &contents));
    for (const auto& entry : contents.archive->entries())
      if (entry.raw_name == "manifest.json")
        manifest_offset = entry.data_offset;
      else if (entry.raw_name == info.value["files"][0]["path"].get<std::string>())
        offset = entry.data_offset;
  }
  auto corrupted = bytes(o.output);
  ASSERT_GT(offset, 0u);
  ASSERT_LT(offset, corrupted.size());
  corrupted[static_cast<std::size_t>(offset)] ^= 1;
  write(root / "corrupt.zip", corrupted);
  EXPECT_EQ(value(app::validate_asset(root / "corrupt.zip")).status, app::ExitCode::success);
  EXPECT_EQ(value(app::verify_asset(root / "corrupt.zip")).status, app::ExitCode::check_failed);
  auto bad_manifest = bytes(o.output);
  ASSERT_GT(manifest_offset, 0u);
  bad_manifest[static_cast<std::size_t>(manifest_offset)] ^= 1;
  write(root / "bad-manifest.zip", bad_manifest);
  auto rejected_manifest = value(app::inspect_asset(root / "bad-manifest.zip"));
  EXPECT_EQ(rejected_manifest.status, app::ExitCode::check_failed);
  EXPECT_EQ(rejected_manifest.value["v1"], "failed");
  EXPECT_TRUE(rejected_manifest.value["error"].contains("message_id"));
  app::ExtractAssetOptions extraction;
  extraction.directory = root / "occupied";
  std::filesystem::create_directory(extraction.directory);
  write(extraction.directory / "keep", "keep");
  auto rejected = app::extract_asset(o.output, extraction);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().message_id, "am.app.target_not_empty");
  EXPECT_EQ(bytes(extraction.directory / "keep"), "keep");
#else
  GTEST_SKIP() << "Read-only process fixtures cover non-Windows builds";
#endif
}
TEST(AssetService, MessageCatalogCoversAllCheckedAndDeferredRulesAndFallback) {
  RecordProperty("req", "K-85|ADR-0032|ADR-0034");
  std::ifstream in(Path(AM_MANIFEST_SCHEMA).parent_path() / "examples/manifest-1.0.example.json");
  std::string json{std::istreambuf_iterator<char>(in), {}};
  auto document = value(manifest::parse_manifest(json));
  auto report = value(manifest::validate_semantics(document));
  for (const auto& rule : report.checked_rules) {
    const auto id = "am.manifest." + rule;
    EXPECT_NE(app::message_text(id), id) << id;
  }
  for (const auto& deferred : report.deferred_rules) {
    const auto id = "am.manifest." + deferred.rule;
    EXPECT_NE(app::message_text(id), id) << id;
  }
  auto unknown = Error::make(Errc::internal, "English fallback").id("test.absent");
  EXPECT_EQ(app::message_text(unknown), "English fallback");
  unknown.message.clear();
  EXPECT_EQ(app::message_text(unknown), "test.absent");
}
