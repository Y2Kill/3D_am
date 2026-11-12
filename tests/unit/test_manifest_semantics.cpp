#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>

#include "am/domain/manifest/semantics.hpp"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using namespace am::manifest;
using J = nlohmann::ordered_json;
constexpr const char* unknown = "00000000-0000-4000-8000-000000000001";
J example() {
  std::ifstream in(std::filesystem::path(AM_MANIFEST_SCHEMA).parent_path() / "examples/manifest-1.0.example.json");
  return J::parse(in);
}
template <class T>
T take(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(r).value();
}
SemanticReport validate(const J& j, const SemanticOptions& options = {}) {
  auto doc = take(parse_manifest(j.dump()));
  return take(validate_semantics(doc, options));
}
void passes(const J& j) {
  auto r = validate(j);
  EXPECT_TRUE(r.complete);
  for (const auto& f : r.findings) ADD_FAILURE() << f.rule << " " << f.pointer;
}
J& field(J& j, std::string type, J v) {
  auto id = j["field_definitions"][0]["id"];
  j["field_definitions"][0] = {
      {"id", id}, {"schema_revision", 1}, {"name", "Test"}, {"type", type}, {"scopes", J::array({"asset"})}};
  j["asset"]["metadata"]["custom_values"][0] = {{"field_id", id}, {"value", std::move(v)}};
  return j["field_definitions"][0];
}
J& cv(J& j) { return j["asset"]["metadata"]["custom_values"][0]; }
J& claim(J& j) { return j["asset"]["metadata"]["claims"][0]; }
void custom_claim(J& j, J v) {
  claim(j)["target"] = {{"kind", "custom"}, {"field_id", j["field_definitions"][0]["id"]}};
  claim(j)["relation"] = "alternative";
  claim(j)["value"] = std::move(v);
}
void compact(J& j) {
  j["asset"]["revision"] = 5;
  j["history"]["entries"][1]["revision"] = 5;
  j["history"]["compacted_ranges"] =
      J::array({J{{"from_revision", 2},
                  {"to_revision", 4},
                  {"summary", {{"revision_count", 3}, {"operation_counts", {{"metadata_changed", 3}}}}}}});
}
struct RuleCase {
  std::string rule, pointer;
  std::function<void(J&)> mutate;
  FindingSeverity severity = FindingSeverity::error;
};
void PrintTo(const RuleCase& c, std::ostream* out) { *out << c.rule; }
const std::vector<RuleCase> cases{
    {"unique_ids", "/files/1/id",
     [](J& j) {
       j["files"][1]["id"] = j["files"][0]["id"];
       j["inputs"][0]["entries"][2]["file_id"] = j["files"][0]["id"];
       j["groups"][0]["file_refs"].erase(1);
     }},
    {"storage_paths", "/files/0/storage_path",
     [](J& j) { j["files"][0]["storage_path"] = "content/" + std::string(unknown) + "/a"; }},
    {"history_operation_id_unique", "/history/entries/1/operation_id",
     [](J& j) { j["history"]["entries"][1]["operation_id"] = j["history"]["entries"][0]["operation_id"]; }},
    {"byte_sizes_uint64", "/files/0/size_bytes", [](J& j) { j["files"][0]["size_bytes"] = "18446744073709551616"; }},
    {"historical_files", "/historical_files/0/removed_at_revision",
     [](J& j) { j["historical_files"] = J::array({J{{"id", unknown}, {"removed_at_revision", 3}}}); }},
    {"derived_from", "/files/2/derived_from/0/file_id",
     [](J& j) { j["files"][2]["derived_from"][0]["file_id"] = unknown; }},
    {"input_source_refs", "/inputs/0/source_refs/0", [](J& j) { j["inputs"][0]["source_refs"][0] = unknown; }},
    {"input_entry_object", "/inputs/0/entries/0/object_id",
     [](J& j) { j["inputs"][0]["entries"][0]["object_id"] = unknown; }},
    {"input_entry_file", "/inputs/0/entries/0/file_id", [](J& j) { j["inputs"][0]["entries"][0].erase("file_id"); }},
    {"input_nesting", "/inputs/0/objects/0/parent_object_id",
     [](J& j) { j["inputs"][0]["objects"][0]["parent_object_id"] = unknown; }},
    {"group_refs", "/groups/0/parent_id", [](J& j) { j["groups"][0]["parent_id"] = j["groups"][0]["id"]; }},
    {"preview_refs", "/previews/primary_id", [](J& j) { j["previews"]["primary_id"] = unknown; }},
    {"field_options", "/field_definitions/0/options/1/id",
     [](J& j) {
       auto& f = field(j, "enum", "a");
       f["options"] = J::array({J{{"id", "a"}, {"label", "A"}}, J{{"id", "a"}, {"label", "B"}}});
     }},
    {"field_constraints", "/field_definitions/0/constraints/minimum/value",
     [](J& j) { field(j, "integer", "0")["constraints"] = {{"minimum", {{"value", "9223372036854775808"}}}}; }},
    {"field_default", "/field_definitions/0/default",
     [](J& j) { j["field_definitions"][0]["default"] = "not a boolean"; }},
    {"custom_value_field", "/asset/metadata/custom_values/0/field_id",
     [](J& j) { cv(j)["field_id"] = "user:" + std::string(unknown); }},
    {"value_representation", "/asset/metadata/custom_values/0/value", [](J& j) { cv(j)["value"] = "not a boolean"; }},
    {"value_options", "/asset/metadata/custom_values/0/value",
     [](J& j) { field(j, "enum", "b")["options"] = J::array({J{{"id", "a"}, {"label", "A"}}}); }},
    {"value_file_ref", "/asset/metadata/custom_values/0/value", [](J& j) { field(j, "file_ref", unknown); }},
    {"value_constraints", "/asset/metadata/custom_values/0/value",
     [](J& j) { field(j, "decimal", "1.5000001")["constraints"] = {{"maximum", {{"value", "1.5"}}}}; }},
    {"value_unit_required", "/asset/metadata/custom_values/0/unit",
     [](J& j) { field(j, "decimal", "1")["dimension"] = "length"; }},
    {"value_dependencies_scope", "/asset/metadata/custom_values/0/dependencies",
     [](J& j) { cv(j)["dependencies"] = {{"state", "none"}}; }},
    {"x02_unit_conversion_open", "/asset/metadata/custom_values/0/value",
     [](J& j) {
       auto& f = field(j, "decimal", "2");
       f["dimension"] = "length";
       f["constraints"] = {{"maximum", {{"value", "1"}, {"unit", "cm"}}}};
       cv(j)["unit"] = "mm";
     },
     FindingSeverity::unverifiable},
    {"x02_physical_default_open", "/field_definitions/0/default",
     [](J& j) {
       auto& f = field(j, "integer", "1");
       f["dimension"] = "length";
       f["default"] = "2";
       cv(j)["unit"] = "mm";
     },
     FindingSeverity::unverifiable},
    {"claim_target_standard", "/asset/metadata/claims/0/target/field",
     [](J& j) { claim(j)["target"] = {{"kind", "standard"}, {"field", "description"}}; }},
    {"claim_target_item", "/asset/metadata/claims/0/target/item_id",
     [](J& j) { claim(j)["target"]["item_id"] = unknown; }},
    {"claim_target_custom", "/asset/metadata/claims/0/target/field_id",
     [](J& j) { claim(j)["target"] = {{"kind", "custom"}, {"field_id", "user:" + std::string(unknown)}}; }},
    {"claim_value_type", "/asset/metadata/claims/0/value", [](J& j) { custom_claim(j, "not a boolean"); }},
    {"claim_value_unit_unexpressible", "/asset/metadata/claims/0/value",
     [](J& j) {
       field(j, "decimal", "1")["dimension"] = "length";
       cv(j)["unit"] = "mm";
       custom_claim(j, "2");
     },
     FindingSeverity::unverifiable},
    {"claim_evidence", "/asset/metadata/claims/0/evidence/0/source_entry_id",
     [](J& j) { claim(j)["evidence"][0]["source_entry_id"] = unknown; }},
    {"license_file_ref", "/asset/metadata/license/file_ref",
     [](J& j) { j["asset"]["metadata"]["license"] = {{"file_ref", unknown}}; }},
    {"license_spdx", "/asset/metadata/license/spdx",
     [](J& j) { j["asset"]["metadata"]["license"] = {{"spdx", "MIT OR OR Apache-2.0"}}; }},
    {"urls", "/origin/sources/0/url", [](J& j) { j["origin"]["sources"][0]["url"] = "https://host/%ZZ"; }},
    {"dependency_refs", "/extensions/org.example.stlstats/dependencies/refs/0",
     [](J& j) { j["extensions"]["org.example.stlstats"]["dependencies"]["refs"][0]["file_id"] = unknown; }},
    {"timestamps", "/asset/metadata/dates/published",
     [](J& j) { j["asset"]["metadata"]["dates"] = {{"published", "2100-02-29"}}; }},
    {"asset_times", "/asset/created_at", [](J& j) { j["asset"]["created_at"] = "2026-11-05T00:00:00Z"; }},
    {"history_coverage", "/history",
     [](J& j) {
       j["asset"]["revision"] = 3;
       j["history"]["entries"][1]["revision"] = 3;
     }},
    {"history_compacted_summary", "/history/compacted_ranges/0/summary", [](J& j) {
       compact(j);
       j["history"]["compacted_ranges"][0]["summary"]["revision_count"] = 2;
     }}};
class ManifestSemanticRule : public ::testing::TestWithParam<RuleCase> {};
TEST_P(ManifestSemanticRule, MinimalMutationProducesOnlyExpectedFinding) {
  RecordProperty("req", "ADR-0035|R-05|O-01|O-02|O-12|O-14|O-18|X-01|X-02");
  auto j = example();
  const auto& c = GetParam();
  passes(j);
  c.mutate(j);
  auto r = validate(j);
  ASSERT_TRUE(r.complete);
  ASSERT_EQ(r.findings.size(), 1u) << [&] {
    std::string s;
    for (auto& f : r.findings) s += f.rule + " " + f.pointer + "\n";
    return s;
  }();
  const auto& f = r.findings[0];
  EXPECT_EQ(f.rule, c.rule);
  EXPECT_EQ(f.pointer, c.pointer);
  EXPECT_EQ(f.severity, c.severity);
  EXPECT_EQ(f.message_id, "am.manifest." + c.rule);
  EXPECT_FALSE(f.detail.empty());
}
INSTANTIATE_TEST_SUITE_P(Catalog, ManifestSemanticRule, ::testing::ValuesIn(cases),
                         [](const auto& info) { return info.param.rule; });
TEST(ManifestSemantics, ExamplePassesCompleteCatalog) {
  RecordProperty("req", "ADR-0035|ADR-0037|ADR-0038");
  std::ifstream in(std::filesystem::path(AM_MANIFEST_SCHEMA).parent_path() / "examples/manifest-1.0.example.json");
  std::string bytes{std::istreambuf_iterator<char>(in), {}};
  auto doc = am::manifest::parse_manifest(bytes);
  ASSERT_TRUE(doc.ok());
  auto result = am::manifest::validate_semantics(doc.value());
  ASSERT_TRUE(result.ok());
  for (const auto& f : result.value().findings) ADD_FAILURE() << f.rule << " " << f.pointer << " " << f.detail;
  EXPECT_TRUE(result.value().complete);
  EXPECT_EQ(result.value().checked_rules.size(), 38);
  EXPECT_TRUE(result.value().has_plugin_data);
  ASSERT_EQ(result.value().deferred_rules.size(), 1u);
  EXPECT_EQ(result.value().deferred_rules[0].rule, "tags_search_key");
  EXPECT_EQ(result.value().deferred_rules[0].reason, "O-08");
}
TEST(ManifestSemantics, M01SelectedFieldMustExist) {
  RecordProperty("req", "M-01|R-05");
  auto j = example();
  claim(j)["target"] = {{"kind", "standard"}, {"field", "description"}};
  EXPECT_EQ(validate(j).findings[0].rule, "claim_target_standard");
}
TEST(ManifestSemantics, M02SelectedClaimCannotCarryValueV2) {
  RecordProperty("req", "M-02|R-05");
  auto j = example();
  claim(j)["target"] = {{"kind", "standard"}, {"field", "title"}};
  claim(j)["relation"] = "alternative";
  claim(j)["value"] = "extra";
  std::ifstream in(AM_MANIFEST_SCHEMA);
  auto schema = take(SchemaValidator::create(std::string{std::istreambuf_iterator<char>(in), {}}));
  auto allowed = take(parse_manifest(j.dump()));
  ASSERT_TRUE(schema.validate(allowed));
  claim(j)["relation"] = "supports_selected";
  auto doc = take(parse_manifest(j.dump()));
  EXPECT_FALSE(schema.validate(doc));
}
TEST(ManifestSemantics, M03AlternativeAndRejectedRequireValueV2) {
  RecordProperty("req", "M-03|R-05");
  std::ifstream in(AM_MANIFEST_SCHEMA);
  auto schema = take(SchemaValidator::create(std::string{std::istreambuf_iterator<char>(in), {}}));
  for (auto relation : {"alternative", "rejected"}) {
    auto j = example();
    claim(j)["target"] = {{"kind", "standard"}, {"field", "title"}};
    claim(j)["relation"] = relation;
    if (std::string_view(relation) == "rejected") claim(j)["review"] = {{"at", "2026-11-12T00:00:00Z"}};
    claim(j)["value"] = "Valid title";
    auto allowed = take(parse_manifest(j.dump()));
    ASSERT_TRUE(schema.validate(allowed));
    claim(j).erase("value");
    auto doc = take(parse_manifest(j.dump()));
    EXPECT_FALSE(schema.validate(doc));
  }
}
TEST(ManifestSemantics, M04CustomClaimRequiresKnownField) {
  RecordProperty("req", "M-04|R-05");
  auto j = example();
  claim(j)["target"] = {{"kind", "custom"}, {"field_id", "user:" + std::string(unknown)}};
  EXPECT_EQ(validate(j).findings[0].rule, "claim_target_custom");
}
TEST(ManifestSemantics, M05ItemClaimRequiresOwnerAuthor) {
  RecordProperty("req", "M-05|R-05");
  auto j = example();
  claim(j)["target"]["item_id"] = unknown;
  EXPECT_EQ(validate(j).findings[0].rule, "claim_target_item");
}
TEST(ManifestSemantics, M06ReorderedAuthorsKeepTargets) {
  RecordProperty("req", "M-06|R-05");
  auto j = example();
  auto& a = j["asset"]["metadata"]["authors"];
  a.push_back({{"name", "No ID"}});
  a.push_back({{"name", "Another without ID"}});
  std::reverse(a.begin(), a.end());
  passes(j);
}
TEST(ManifestSemantics, M07KnownFieldTypeMustMatch) {
  RecordProperty("req", "M-07|R-05");
  auto j = example();
  cv(j)["value"] = 4;
  EXPECT_EQ(validate(j).findings[0].rule, "value_representation");
}
TEST(ManifestSemantics, M08PluginFragmentsRemainOpaqueAndDocumentUnchanged) {
  RecordProperty("req", "M-08|ADR-0038");
  auto j = example();
  auto& f = field(j, "plugin:org.test.kind:data", J{{"value", 1}});
  f["constraints"] = {{"plugin", true}};
  f["default"] = false;
  custom_claim(j, J::array({1, 2, 3}));
  auto bytes = j.dump();
  auto token = bytes.find("[1,2,3]");
  ASSERT_NE(token, bytes.npos);
  bytes.replace(token, 7, "[9007199254740993,1e9999,-0]");
  auto doc = take(parse_manifest(bytes));
  ASSERT_TRUE(cv(doc.known())["value"].is_binary());
  const auto before = take(write_manifest(doc));
  auto r = take(validate_semantics(doc));
  EXPECT_TRUE(r.complete);
  EXPECT_TRUE(r.findings.empty());
  EXPECT_TRUE(r.has_plugin_data);
  EXPECT_EQ(take(write_manifest(doc)), before);
  auto work = [&] { return take(validate_semantics(doc)).findings.empty(); };
  auto one = std::async(std::launch::async, work), two = std::async(std::launch::async, work);
  EXPECT_TRUE(one.get());
  EXPECT_TRUE(two.get());
}
TEST(ManifestSemantics, ExactNumericRepresentationsAndBounds) {
  RecordProperty("req", "X-02|ADR-0038");
  for (auto s : {"-9223372036854775808", "9223372036854775807", "-0", "0"}) {
    auto j = example();
    field(j, "integer", s);
    passes(j);
  }
  for (auto s : {"-9223372036854775809", "9223372036854775808", "1e1", "01", "+1", "1.0"}) {
    auto j = example();
    field(j, "integer", s);
    EXPECT_EQ(validate(j).findings[0].rule, "value_representation");
  }
  for (auto s : {"1.500", "-0", "-1000000000000000000000000000000000000.0000001"}) {
    auto j = example();
    field(j, "decimal", s)["constraints"] = {{"maximum", {{"value", "1.5"}}}};
    passes(j);
  }
  auto j = example();
  field(j, "decimal", "-0")["constraints"] = {{"minimum", {{"value", "0.000"}}}, {"maximum", {{"value", "0"}}}};
  passes(j);
  cv(j)["value"] = "-0.00000000000000000001";
  EXPECT_EQ(validate(j).findings.size(), 1u);
  field(j, "integer", "2")["constraints"] = {{"minimum", {{"value", "3"}}}, {"maximum", {{"value", "1"}}}};
  cv(j)["value"] = "2";
  EXPECT_EQ(validate(j).findings.size(), 2u);
}
TEST(ManifestSemantics, PhysicalUnitsDefaultsAndClaimConstraints) {
  RecordProperty("req", "X-02|R-05");
  auto j = example();
  auto& f = field(j, "decimal", "1.500");
  f["dimension"] = "length";
  f["constraints"] = {{"maximum", {{"value", "1.5"}, {"unit", "arbitrary unit"}}}};
  cv(j)["unit"] = "arbitrary unit";
  passes(j);
  f["constraints"]["maximum"].erase("unit");
  EXPECT_EQ(validate(j).findings[0].rule, "field_constraints");
  f["dimension"] = "dimensionless";
  cv(j).erase("unit");
  passes(j);
  custom_claim(j, "100");
  passes(j);  // Alternatives do not obey current bounds.
  f["default"] = "100";
  EXPECT_EQ(validate(j).findings[0].rule, "field_default");
}
TEST(ManifestSemantics, CalendarAndFractionalDatetimeBounds) {
  RecordProperty("req", "O-14|X-02");
  for (auto s : {"2000-02-29", "2024-02-29", "2026-04-30"}) {
    auto j = example();
    field(j, "date", s);
    passes(j);
  }
  for (auto s : {"2100-02-29", "2025-02-29", "2026-04-31", "2026-00-01"}) {
    auto j = example();
    field(j, "date", s);
    EXPECT_EQ(validate(j).findings[0].rule, "timestamps");
  }
  for (auto s : {"2026-11-12T23:59:60Z", "2026-11-12T23:59:59.123456789Z"}) {
    auto j = example();
    field(j, "datetime", s);
    passes(j);
  }
  for (auto s : {"2026-11-12T24:00:00Z", "2026-11-12T00:60:00Z", "2026-11-12T00:00:61Z"}) {
    auto j = example();
    field(j, "datetime", s);
    EXPECT_EQ(validate(j).findings[0].rule, "timestamps");
  }
  auto j = example();
  field(j, "datetime", "2026-11-12T00:00:00.1200Z")["constraints"] = {
      {"minimum", {{"value", "2026-11-12T00:00:00.12Z"}}}, {"maximum", {{"value", "2026-11-12T00:00:00.120Z"}}}};
  passes(j);
  cv(j)["value"] = "2026-11-12T00:00:00.1201Z";
  EXPECT_EQ(validate(j).findings[0].rule, "value_constraints");
}
TEST(ManifestSemantics, UnicodeCodepointsEnumsAndDeprecatedOptions) {
  RecordProperty("req", "X-02");
  for (const auto& [s, count] :
       std::vector<std::pair<std::string, int>>{{"e\xCC\x81", 2}, {"\xC3\xA9", 1}, {"\xF0\x9F\x98\x80", 1}}) {
    auto j = example();
    field(j, "text", s)["constraints"] = {{"min_length", count}, {"max_length", count}};
    passes(j);
    cv(j)["value"] = s + "a";
    EXPECT_EQ(validate(j).findings[0].rule, "value_constraints");
  }
  auto j = example();
  auto& f = field(j, "multi_enum", J::array({"a", "b"}));
  f["options"] = J::array({J{{"id", "a"}, {"label", "A"}, {"deprecated", true}}, J{{"id", "b"}, {"label", "B"}}});
  f["constraints"] = {{"min_items", 2}, {"max_items", 2}};
  passes(j);
  cv(j)["value"] = J::array({"a", "a"});
  EXPECT_EQ(validate(j).findings[0].rule, "value_representation");
  cv(j)["value"] = J::array({"a"});
  EXPECT_EQ(validate(j).findings[0].rule, "value_constraints");
}
TEST(ManifestSemantics, SpdxGrammar) {
  RecordProperty("req", "O-14");
  for (auto s :
       {"MIT", "GPL-2.0-or-later", "Apache-2.0 WITH LLVM-exception", "(MIT OR Apache-2.0) AND BSD-3-Clause",
        "LicenseRef-my.custom-1", "DocumentRef-x:LicenseRef-y", "GPL-2.0+", "MIT OR Apache-2.0 AND GPL-2.0+"}) {
    auto j = example();
    j["asset"]["metadata"]["license"] = {{"spdx", s}};
    passes(j);
  }
  for (auto s : {"", "mit or apache-2.0", "(MIT", "MIT WITH", "MIT AND OR BSD", "LicenseRef-", "MIT!",
                 "(MIT)WITH LLVM-exception", "MIT OR(Apache-2.0)", "MIT WITH E WITH F", "DocumentRef-:LicenseRef-y"}) {
    auto j = example();
    j["asset"]["metadata"]["license"] = {{"spdx", s}};
    EXPECT_EQ(validate(j).findings[0].rule, "license_spdx") << s;
  }
}
TEST(ManifestSemantics, UriGrammarAndAllTypedLocations) {
  RecordProperty("req", "O-14");
  for (auto s : {"https://user:pass@[2001:db8::1]:443/a%20b?q=a/b?c#fragment", "urn:example:test",
                 "mailto:user@example.org", "https://[::ffff:192.0.2.1]/", "https://[v1.a:b]/",
                 "https://example.org/\xD0\xBF\xD1\x83\xD1\x82\xD1\x8C", "https://host:65536/", "file:///a"}) {
    auto j = example();
    field(j, "url", s);
    passes(j);
  }
  for (auto s : {"relative/path", "http://[1:2:3]/", "http://[1::2::3]/", "http://host:bad/", "https://host/%0g",
                 "http://host/a b", "http://host/#a#b", "http://[:::]/"}) {
    auto j = example();
    field(j, "url", s);
    EXPECT_EQ(validate(j).findings[0].rule, "urls") << s;
  }
  auto j = example();
  field(j, "url", "https://valid/")["default"] = "https://bad/%X";
  EXPECT_EQ(validate(j).findings[0].pointer, "/field_definitions/0/default");
  j = example();
  claim(j)["target"] = {{"kind", "standard"}, {"field", "license"}};
  claim(j)["relation"] = "alternative";
  claim(j)["value"] = {{"url", "https://bad/%X"}};
  EXPECT_EQ(validate(j).findings[0].pointer, "/asset/metadata/claims/0/value/url");
}
TEST(ManifestSemantics, HistoryIntervalsAndSummariesWithoutExpansion) {
  RecordProperty("req", "O-18");
  auto base = example();
  compact(base);
  passes(base);
  for (int fault = 0; fault < 7; ++fault) {
    auto j = base;
    auto& range = j["history"]["compacted_ranges"][0];
    if (fault == 0) range["from_revision"] = 1;
    if (fault == 1) range["to_revision"] = 5;
    if (fault == 2) j["history"]["compacted_ranges"].push_back(range);
    if (fault == 3) range["summary"]["operation_counts"]["metadata_changed"] = 2;
    if (fault == 4) range["summary"]["operation_counts"] = {{"asset_created", 3}};
    if (fault == 5) range["summary"]["operation_counts"] = {{"metadata_changed", -1}};
    if (fault == 6) range["from_revision"] = 3;
    EXPECT_FALSE(validate(j).findings.empty());
  }
  auto j = base;
  constexpr std::uint64_t last = 9007199254740992ULL;
  j["asset"]["revision"] = last;
  j["history"]["entries"][1]["revision"] = last;
  auto& range = j["history"]["compacted_ranges"][0];
  range["to_revision"] = last - 1;
  range["summary"] = {{"revision_count", last - 2}};
  passes(j);
}
TEST(ManifestSemantics, HistoricalReferencesAreValidAndHashesAreNotCompared) {
  RecordProperty("req", "O-12|R-05");
  auto j = example();
  j["historical_files"] =
      J::array({J{{"id", unknown}, {"removed_at_revision", 2}, {"last_known_sha256", std::string(64, '0')}}});
  j["files"][2]["derived_from"][0]["file_id"] = unknown;
  j["extensions"]["org.example.stlstats"]["dependencies"]["refs"][0]["file_id"] = unknown;
  claim(j)["evidence"].push_back({{"file_id", unknown}});
  passes(j);
}
TEST(ManifestSemantics, FindingLimitIsReportedAndCompletenessIsAccurate) {
  RecordProperty("req", "ADR-0035|O-17");
  auto j = example();
  for (auto& f : j["files"]) f["storage_path"] = "../bad";
  for (bool stop : {false, true}) {
    SemanticOptions o;
    o.max_findings = 1;
    o.stop_after_limit = stop;
    auto r = validate(j, o);
    EXPECT_FALSE(r.complete);
    ASSERT_EQ(r.findings.size(), 2u);
    EXPECT_EQ(r.findings.back().rule, "limit_exceeded");
    EXPECT_EQ(r.checked_rules.size() == 38, !stop);
  }
}
TEST(ManifestSemantics, Uint64BoundsInAllSixLocations) {
  RecordProperty("req", "ADR-0035|ADR-0038");
  auto j = example();
  j["historical_files"] = J::array({J{{"id", unknown}, {"removed_at_revision", 2}, {"last_known_size_bytes", "0"}}});
  j["inputs"][0]["objects"][0]["volumes"] = J::array({J{{"index", 0}, {"name", "part"}, {"size_bytes", "0"}}});
  j["extensions"]["org.example.stlstats"]["attachments"] = J::array({J{{"id", unknown},
                                                                       {"path", "extensions/org.example.stlstats/a"},
                                                                       {"size_bytes", "0"},
                                                                       {"sha256", std::string(64, '0')}}});
  for (auto pointer : {"/files/0/size_bytes", "/historical_files/0/last_known_size_bytes",
                       "/inputs/0/objects/0/size_bytes", "/inputs/0/objects/0/volumes/0/size_bytes",
                       "/inputs/0/entries/0/size_bytes", "/extensions/org.example.stlstats/attachments/0/size_bytes"}) {
    for (auto s : {"0", "18446744073709551615", "18446744073709551616", "99999999999999999999"}) {
      auto copy = j;
      copy[J::json_pointer(pointer)] = s;
      auto r = validate(copy);
      if (std::string_view(s) == "0" || std::string_view(s) == "18446744073709551615")
        EXPECT_TRUE(r.findings.empty());
      else {
        ASSERT_EQ(r.findings.size(), 1u);
        EXPECT_EQ(r.findings[0].rule, "byte_sizes_uint64");
        EXPECT_EQ(r.findings[0].pointer, pointer);
      }
    }
  }
}
TEST(ManifestSemantics, EveryTimestampLocationUsesCalendarValidation) {
  RecordProperty("req", "O-14|R-05");
  auto j = example();
  j["origin"]["sources"][0]["published_at"] = "2026-11-12T00:00:00Z";
  j["origin"]["sources"][0]["retrieved_at"] = "2026-11-12T00:00:00Z";
  claim(j)["review"] = {{"at", "2026-11-12T00:00:00Z"}};
  auto& f = field(j, "datetime", "2026-11-12T00:00:00Z");
  f["default"] = "2026-11-12T00:00:00Z";
  f["constraints"] = {{"minimum", {{"value", "2026-11-12T00:00:00Z"}}}};
  for (auto pointer :
       {"/origin/sources/0/published_at", "/origin/sources/0/retrieved_at", "/inputs/0/imported_at",
        "/asset/metadata/claims/0/created_at", "/asset/metadata/claims/0/review/at", "/field_definitions/0/default",
        "/field_definitions/0/constraints/minimum/value", "/asset/metadata/custom_values/0/value"}) {
    auto copy = j;
    copy[J::json_pointer(pointer)] = "2100-02-29T00:00:00Z";
    auto r = validate(copy);
    ASSERT_EQ(r.findings.size(), 1u);
    EXPECT_EQ(r.findings[0].rule, "timestamps");
    EXPECT_EQ(r.findings[0].pointer, pointer);
  }
}
TEST(ManifestSemantics, UriControlsInvalidUtf8AndLocations) {
  RecordProperty("req", "O-14");
  auto j = example();
  field(j, "url", "https://valid/");
  auto doc = take(parse_manifest(j.dump()));
  for (const auto& s :
       std::vector<std::string>{"http://x/\x01", "http://x/\xC2\x80", "http://x/\xFF", "1http://x", "http://[::1"}) {
    cv(doc.known())["value"] = s;
    auto r = take(validate_semantics(doc));
    ASSERT_EQ(r.findings.size(), 1u);
    EXPECT_EQ(r.findings[0].rule, "urls");
  }
  for (auto pointer : {"/asset/metadata/publisher/url", "/asset/metadata/license/url", "/asset/metadata/links/0/url"}) {
    auto copy = example();
    copy[J::json_pointer(pointer)] = "http://host:port/";
    auto r = validate(copy);
    ASSERT_EQ(r.findings.size(), 1u);
    EXPECT_EQ(r.findings[0].pointer, pointer);
  }
}
TEST(ManifestSemantics, ScopedDependenciesAndClaimsResolveWithinOwner) {
  RecordProperty("req", "O-12|R-05");
  auto j = example();
  auto fid = j["field_definitions"][0]["id"];
  auto& deps = j["extensions"]["org.example.stlstats"]["dependencies"];
  deps["refs"].push_back(
      {{"kind", "field_value"}, {"owner_scope", "asset"}, {"owner_id", j["asset"]["id"]}, {"field_id", fid}});
  passes(j);
  deps["refs"].back()["owner_id"] = unknown;
  auto r = validate(j);
  ASSERT_EQ(r.findings.size(), 1u);
  EXPECT_EQ(r.findings[0].rule, "dependency_refs");
  j = example();
  j["files"][0]["metadata"]["claims"] = j["asset"]["metadata"]["claims"];
  claim(j)["id"] = unknown;
  EXPECT_EQ(validate(j).findings[0].rule, "claim_target_item");
}
TEST(ManifestSemantics, SameInputObjectReferencesAndNestedExclusion) {
  RecordProperty("req", "X-01");
  auto j = example();
  auto child = j["inputs"][0]["objects"][0];
  child["id"] = unknown;
  child["parent_object_id"] = j["inputs"][0]["objects"][0]["id"];
  child["parent_entry_id"] = j["inputs"][0]["entries"][0]["id"];
  child["nested_action"] = "extract";
  j["inputs"][0]["objects"].push_back(child);
  passes(j);
  j["inputs"][0]["objects"][1]["nested_action"] = "exclude";
  EXPECT_EQ(validate(j).findings[0].rule, "input_nesting");
  j["inputs"][0]["entries"][0]["result"] = "excluded";
  j["inputs"][0]["entries"][0].erase("file_id");
  passes(j);
  auto input = j["inputs"][0];
  input["id"] = "00000000-0000-4000-8000-000000000002";
  input["objects"] = J::array();
  input["entries"] = J::array();
  j["inputs"].push_back(input);
  auto entry = j["inputs"][0]["entries"][0];
  entry["id"] = "00000000-0000-4000-8000-000000000003";
  j["inputs"][1]["entries"].push_back(entry);
  EXPECT_EQ(validate(j).findings[0].rule, "input_entry_object");
}
TEST(ManifestSemantics, LargeGraphAndClaimsMeasurement) {
  RecordProperty("req", "O-17|ADR-0035");
  auto j = example();
  j["files"] = J::array();
  j["groups"] = J::array();
  j.erase("previews");
  j["inputs"][0]["entries"] = J::array();
  j.erase("extensions");
  auto& claims = j["asset"]["metadata"]["claims"];
  claims = J::array();
  auto make_id = [](std::size_t n) {
    std::string s = std::to_string(n);
    return "00000000-0000-4000-8000-" + std::string(12 - s.size(), '0') + s;
  };
  for (std::size_t i = 0; i < 100000; ++i) {
    j["files"].push_back({{"id", make_id(i)},
                          {"storage_path", "generated/" + std::to_string(i)},
                          {"size_bytes", "0"},
                          {"sha256", std::string(64, '0')},
                          {"origin_kind", "generated"},
                          {"generation", {{"tool", "test.generator"}}}});
    claims.push_back(
        {{"id", make_id(i)},
         {"target",
          {{"kind", "standard_item"}, {"field", "authors"}, {"item_id", j["asset"]["metadata"]["authors"][0]["id"]}}},
         {"relation", "supports_selected"},
         {"method", "user"},
         {"created_at", "2026-11-12T00:00:00Z"}});
  }
  for (std::size_t i = 0; i < 10000; ++i) {
    J g{{"id", make_id(i)}, {"name", "Group"}};
    if (i) g["parent_id"] = make_id(i - 1);
    j["groups"].push_back(g);
  }
  auto doc = take(parse_manifest(j.dump()));
  auto started = std::chrono::steady_clock::now();
  auto r = take(validate_semantics(doc));
  auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  EXPECT_TRUE(r.complete);
  EXPECT_TRUE(r.findings.empty());
  std::cout << "SEMANTICS files=100000 groups=10000 claims=100000 seconds=" << seconds;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  ASSERT_TRUE(
      GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)));
  std::cout << " peak_working_set_bytes=" << memory.PeakWorkingSetSize
            << " peak_commit_bytes=" << memory.PeakPagefileUsage;
#endif
  std::cout << '\n';
}
}  // namespace
