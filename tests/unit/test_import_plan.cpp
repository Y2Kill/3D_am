#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "am/services/import/scan.hpp"

namespace {
namespace imp = am::import;
using J = imp::PlanJson;
J candidate(unsigned index) {
  const auto id = "n" + std::to_string(index);
  return {
      {"id", "c" + std::to_string(index)},
      {"kind", "new_asset"},
      {"root", 0},
      {"base_path", ""},
      {"title_hint", "Test"},
      {"context", J::object()},
      {"confidence", "high"},
      {"reasons", J::array({"root_file"})},
      {"inputs",
       J::array({{{"id", "i" + std::to_string(index)}, {"kind", "file"}, {"objects", J::array({{{"node", id}}})}}})},
      {"entries", J::array({{{"node", id}, {"decision", "include"}}})},
      {"directories", J::array()},
      {"volume_sets", J::array()},
      {"suggested_links", J::array()},
      {"matches", J::array()},
      {"warnings", J::array()},
      {"action", "import"}};
}
J plan() {
  J nodes = J::array();
  for (unsigned i = 1; i <= 2; ++i)
    nodes.push_back({{"id", "n" + std::to_string(i)},
                     {"root", 0},
                     {"path", "file" + std::to_string(i)},
                     {"name", "file" + std::to_string(i)},
                     {"kind", "file"},
                     {"size", "1"},
                     {"mtime_ns", "123"},
                     {"detected", "unknown"},
                     {"status", "available"}});
  return {
      {"format", "am-import-plan"},
      {"version", 1},
      {"created_at", "2026-11-23T00:00:00.000Z"},
      {"generator", {{"name", "am"}, {"version", "test"}}},
      {"profile", {{"id", "default"}, {"version", 1}, {"sha256", std::string(64, '0')}, {"not_applied", J::array()}}},
      {"mode", "strict"},
      {"roots", J::array({{{"path", "/synthetic"}}})},
      {"scan",
       {{"completeness", "complete"},
        {"limits_hit", J::array()},
        {"counts", {{"nodes", 2}, {"files", 2}, {"unavailable", 0}}}}},
      {"nodes", nodes},
      {"candidates", J::array({candidate(1), candidate(2)})}};
}
void invalid(const J& value, std::string_view pointer) {
  auto parsed = imp::parse_plan(value.dump());
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().message_id, "am.import.plan_invalid");
  EXPECT_EQ(parsed.error().ctx.detail, pointer);
}
}  // namespace
TEST(ImportPlan, RoundTripAndActualSchemaFixture) {
  RecordProperty("req", "SC-1|SC-2");
  auto parsed = imp::parse_plan(plan().dump());
  ASSERT_TRUE(parsed);
  const auto first = imp::serialize_plan(*parsed);
  auto second = imp::parse_plan(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(imp::serialize_plan(*second), first);
  std::filesystem::create_directories(AM_IMPORT_PLAN_CASES);
  std::ofstream(std::filesystem::path(AM_IMPORT_PLAN_CASES) / "contract.json", std::ios::binary) << first;
}
TEST(ImportPlan, MissingNodeReferenceReportsExactPointer) {
  RecordProperty("req", "SC-2");
  auto p = plan();
  p["candidates"][0]["entries"][0]["node"] = "missing";
  invalid(p, "/candidates/0/entries/0/node");
}
TEST(ImportPlan, MissingVolumeReferenceReportsExactPointer) {
  RecordProperty("req", "SC-6");
  auto p = plan();
  p["candidates"][0]["inputs"][0]["objects"][0] = {{"volume_set", "missing"}};
  invalid(p, "/candidates/0/inputs/0/objects/0/volume_set");
}
TEST(ImportPlan, FileCannotBelongToTwoCandidates) {
  RecordProperty("req", "SC-2");
  auto p = plan();
  p["candidates"][1]["entries"][0]["node"] = "n1";
  invalid(p, "/candidates/1/entries/0/node");
}
TEST(ImportPlan, VolumeSetCannotBeSplitBetweenCandidates) {
  RecordProperty("req", "SC-6");
  auto p = plan();
  p["candidates"][0]["volume_sets"] = J::array({{{"id", "v1"},
                                                 {"scheme", "rar_part"},
                                                 {"base", "set"},
                                                 {"members", J::array({"n1", "n2"})},
                                                 {"missing_indices", J::array()},
                                                 {"confirmed", false}}});
  p["candidates"][0]["inputs"][0]["objects"] = J::array({{{"volume_set", "v1"}}});
  invalid(p, "/candidates/1/entries/0/node");
}
TEST(ImportPlan, ReadyAssetRequiresExactlyOneZipInput) {
  RecordProperty("req", "SC-14");
  auto p = plan();
  p["candidates"][0]["kind"] = "ready_asset";
  invalid(p, "/candidates/0/inputs");
  p["nodes"][0]["detected"] = "archive.zip";
  ASSERT_TRUE(imp::parse_plan(p.dump()));
  p["candidates"][0]["inputs"][0]["objects"].push_back({{"node", "n2"}});
  invalid(p, "/candidates/0/inputs");
}
TEST(ImportPlan, SkipRequiresExactInputMatchForEveryObject) {
  RecordProperty("req", "SC-15");
  auto p = plan();
  auto& c = p["candidates"][0];
  c["action"] = "skip";
  c["matches"] = J::array({{{"kind", "sha256_file"}, {"node", "n1"}, {"asset_id", "known"}}});
  invalid(p, "/candidates/0/action");
  c["matches"][0]["kind"] = "name_size";
  invalid(p, "/candidates/0/action");
  c["matches"][0]["kind"] = "sha256_input";
  EXPECT_TRUE(imp::parse_plan(p.dump()));
}
TEST(ImportPlan, UnknownVersionAndSchemaViolationsAreRejected) {
  RecordProperty("req", "SC-1");
  auto p = plan();
  for(const std::uint64_t version:{3ULL,0ULL,18446744073709551615ULL}) {
    p["version"]=version;
    invalid(p,"/version");
  }
  p=plan(); p["version"]=2;
  invalid(p,"/profile");
  p = plan();
  p["extra"] = true;
  EXPECT_FALSE(imp::parse_plan(p.dump()));
  EXPECT_FALSE(imp::parse_plan("{\"format\":1,\"format\":2}"));
}
TEST(ImportPlan, DuplicateIdsAndOutOfRangeSizeAreRejected) {
  RecordProperty("req", "SC-1");
  auto p = plan();
  p["nodes"][1]["id"] = "n1";
  invalid(p, "/nodes/1/id");
  p = plan();
  p["nodes"][0]["size"] = "18446744073709551616";
  invalid(p, "/nodes/0/size");
}
TEST(ImportPlan, UserCanMoveFilesChangeDecisionsDeleteCandidatesAndCombine) {
  RecordProperty("req", "SC-2|SC-8");
  auto p = plan();
  auto second = p["candidates"][1];
  p["candidates"].erase(1);
  p["candidates"][0]["entries"].push_back(second["entries"][0]);
  p["candidates"][0]["inputs"].push_back(second["inputs"][0]);
  p["candidates"][0]["entries"][1]["decision"] = "exclude";
  EXPECT_TRUE(imp::parse_plan(p.dump()));
  p["candidates"] = J::array();
  EXPECT_TRUE(imp::parse_plan(p.dump()));
}
TEST(ImportPlan, UserCanStoreRecognizedReadyZipAsAnOrdinaryFile) {
  RecordProperty("req", "SC-14|SC-2");
  auto p = plan();
  p["nodes"][0]["detected"] = "archive.zip";
  auto& c = p["candidates"][0];
  c["kind"] = "ready_asset";
  ASSERT_TRUE(imp::parse_plan(p.dump()));
  c["kind"] = "new_asset";
  c["inputs"][0]["kind"] = "file";
  c["entries"].push_back(p["candidates"][1]["entries"][0]);
  c["inputs"].push_back(p["candidates"][1]["inputs"][0]);
  p["candidates"].erase(1);
  EXPECT_TRUE(imp::parse_plan(p.dump()));
}
