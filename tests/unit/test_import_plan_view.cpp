#include <gtest/gtest.h>

#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <type_traits>

#include "am/application/import_service.hpp"
#include "am/domain/manifest/codec.hpp"
#include "am/domain/manifest/semantics.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/import/plan_view.hpp"
#include "am/storage/asset_zip/probe.hpp"
#include "am/storage/asset_zip/writer.hpp"

namespace {
namespace imp = am::import;
imp::ImportPlan fixture() {
  const auto root = std::filesystem::path(AM_IMPORT_SCAN_ROOT) / am::Uuid::v4().to_string();
  std::filesystem::create_directories(root);
  std::ofstream(root / "file", std::ios::binary) << "bytes";
  imp::ScanRequest r;
  r.roots = {root};
  r.profile = *am::application::load_import_profile();
  auto result = imp::scan(r);
  if (!result) throw std::runtime_error(am::to_log_string(result.error()));
  return *result;
}
}  // namespace
TEST(ImportPlanView, OnlyValidatedBytesBuildAnImmutableTypedView) {
  RecordProperty("req", "SC-1|SC-2|ADR-0038");
  static_assert(!std::is_default_constructible_v<imp::PlanView>);
  static_assert(!std::is_constructible_v<imp::PlanView, imp::ImportPlan>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<imp::PlanView>().nodes())>>);
  const auto p = fixture();
  auto parsed = imp::parse_plan_view(imp::serialize_plan(p));
  ASSERT_TRUE(parsed) << am::to_log_string(parsed.error());
  ASSERT_EQ(parsed->roots().size(), 1u);
  ASSERT_EQ(parsed->candidates().size(), 1u);
  EXPECT_EQ(parsed->mode(), imp::ImportMode::strict);
  const auto& entry = parsed->candidates()[0].entries.at(0);
  const auto& n = parsed->nodes().at(entry.node);
  EXPECT_EQ(n.name, "file");
  EXPECT_EQ(n.size, 5u);
  EXPECT_TRUE(n.mtime_ns);
  EXPECT_EQ(n.kind, imp::NodeKind::file);
  EXPECT_EQ(entry.decision, imp::EntryDecision::include);
  EXPECT_FALSE(imp::parse_plan_view("{}"));
  auto broken = p;
  broken.value["candidates"][0]["entries"][0]["node"] = "missing";
  EXPECT_FALSE(imp::parse_plan_view(imp::serialize_plan(broken)));
}
TEST(ImportPlanView, ExactIntegerLimitsAreValidatedByParsePlan) {
  RecordProperty("req", "ADR-0038|SC-1");
  auto p = fixture();
  auto& n = p.value["nodes"][1];
  n["size"] = "18446744073709551615";
  n["mtime_ns"] = "-9223372036854775808";
  auto v = imp::parse_plan_view(imp::serialize_plan(p));
  ASSERT_TRUE(v);
  EXPECT_EQ(v->nodes()[1].size, UINT64_MAX);
  EXPECT_EQ(v->nodes()[1].mtime_ns, INT64_MIN);
  for (const auto& value : {"9223372036854775808", "-9223372036854775809"}) {
    n["mtime_ns"] = value;
    auto result = imp::parse_plan(imp::serialize_plan(p));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().ctx.detail, "/nodes/1/mtime_ns");
  }
  n["mtime_ns"] = "9223372036854775807";
  n["size"] = "18446744073709551616";
  EXPECT_FALSE(imp::parse_plan(imp::serialize_plan(p)));
}
TEST(ImportPlanView, VersionTwoRetainsLimitsAndArchiveReferencesAndVersionOneStillReads) {
  RecordProperty("req","SC-1|K-6|ADR-0038");
  auto plan=fixture();
  auto& j=plan.value;
  j["version"]=2;
  j["profile"]["limits"]={{"max_entries","100000"},{"max_unpacked_bytes","18446744073709551615"},
    {"max_ratio",200},{"max_seconds_per_container","3600"},{"max_temp_bytes","107374182400"}};
  const std::string node=j["nodes"][1]["id"];
  j["nodes"][1]["detected"]="archive.zip";
  j["archives"]=imp::PlanJson::array({{{"node",node},{"container","zip"},{"status","listed"},{"entry_count",1},
    {"name_encodings",imp::PlanJson::array({"utf-8(valid)"})},{"zip64",false},{"comment_length",0},{"encrypted_entries",0},{"unsupported_entries",0}}});
  j["archive_entries"]=imp::PlanJson::array({{{"id","a1"},{"archive",node},{"index",0},{"path","part.bin"},
    {"path_raw","706172742e62696e"},{"name_encoding","utf-8(valid)"},{"encoding_exact",true},{"kind","file"},
    {"size","3"},{"compressed_size","3"},{"crc32","00000000"},{"method",0},{"encrypted",false},{"detected","unknown"},{"status","available"}}});
  j["candidates"][0]["inputs"].push_back({{"id","archive-input"},{"kind","archive"},{"objects",imp::PlanJson::array({{{"node",node}}})}});
  j["candidates"][0]["entries"].push_back({{"archive_entry","a1"},{"decision","include"}});
  auto view=imp::parse_plan_view(imp::serialize_plan(plan));
  ASSERT_TRUE(view)<<am::to_log_string(view.error());
  EXPECT_EQ(view->version(),2u); ASSERT_TRUE(view->limits());
  EXPECT_EQ(view->limits()->max_unpacked_bytes,UINT64_MAX);
  ASSERT_EQ(view->archives().size(),1u); ASSERT_EQ(view->archive_entries().size(),1u);
  EXPECT_EQ(view->archive_entries()[0].raw_path,"part.bin");
  EXPECT_EQ(view->candidates()[0].entries.back().archive_entry,0u);
  const auto reject=[&](imp::ImportPlan broken,std::string pointer) {
    const auto bad=imp::parse_plan(imp::serialize_plan(broken));
    EXPECT_FALSE(bad);
    if(!bad) { EXPECT_EQ(bad.error().ctx.detail,pointer); }
  };
  auto bad=plan; bad.value["archive_entries"][0]["archive"]="missing"; reject(bad,"/archive_entries/0/archive");
  bad=plan; bad.value["candidates"][0]["entries"].back()["archive_entry"]="missing"; reject(bad,"/candidates/0/entries/1/archive_entry");
  bad=plan; bad.value["archive_entries"][0]["path_raw"]="AB"; reject(bad,"/archive_entries/0/path_raw");
  bad=plan; bad.value["profile"]["limits"]["max_entries"]="18446744073709551616"; reject(bad,"/profile/limits/max_entries");
  auto legacy=fixture(); legacy.value["version"]=1; legacy.value.erase("archives"); legacy.value.erase("archive_entries");
  legacy.value["profile"].erase("limits");
  legacy.value["profile"]["not_applied"]=imp::PlanJson::array();
  ASSERT_TRUE(imp::parse_plan_view(imp::serialize_plan(legacy)));
}
TEST(ImportPlanView, RawFieldLocationsMatchSchemaAndLongHexUsesLinearValidation) {
  RecordProperty("req", "SC-1|ADR-0012");
  const auto file = std::filesystem::path(AM_MANIFEST_SCHEMA).parent_path() / "import-plan.v1.schema.json";
  std::ifstream stream(file, std::ios::binary);
  const auto schema = imp::PlanJson::parse(std::string{std::istreambuf_iterator<char>(stream), {}});
  std::set<std::string> actual;
  std::function<void(const imp::PlanJson&, std::string)> visit = [&](const auto& j, std::string at) {
    if (j.is_object()) {
      if (j.value("pattern", std::string{}) == "^([0-9a-f]{2})+$") actual.insert(at);
      for (auto i = j.begin(); i != j.end(); ++i) visit(i.value(), at + "/" + i.key());
    } else if (j.is_array())
      for (std::size_t i = 0; i < j.size(); ++i) visit(j.at(i), at + "/" + std::to_string(i));
  };
  visit(schema, "");
  std::set<std::string> handled;
  for (const auto field : imp::plan_raw_schema_fields) handled.insert(std::string(field));
  EXPECT_EQ(actual, handled);
  auto p = fixture();
  const auto original = p.value.dump();
  for (const auto location : imp::plan_raw_schema_fields) {
    const auto group = std::string(location).find("/roots/") != std::string::npos ? "roots" : "nodes";
    const auto index = std::string(group) == "roots" ? 0u : 1u;
    const auto field = std::string(location.substr(location.rfind('/') + 1));
    auto long_plan = p;
    long_plan.value[group][index][field] = std::string(65536, 'a');
    const auto text = imp::serialize_plan(long_plan);
    auto valid = imp::parse_plan_view(text);
    ASSERT_TRUE(valid) << am::to_log_string(valid.error());
    EXPECT_EQ(imp::serialize_plan(*imp::parse_plan(text)), text);
    for (const auto invalid : {"", "0", "0A", "GG", "00xz"}) {
      long_plan.value[group][index][field] = invalid;
      auto rejected = imp::parse_plan(imp::serialize_plan(long_plan));
      ASSERT_FALSE(rejected);
      EXPECT_EQ(rejected.error().message_id, "am.import.plan_invalid");
      EXPECT_EQ(rejected.error().ctx.detail, "/" + std::string(group) + "/" + std::to_string(index) + "/" + field);
    }
  }
  EXPECT_EQ(p.value.dump(), original);
}
TEST(ImportRunManifest, LongRelativePathRawPassesV1AndPublishedSchema) {
  RecordProperty("req", "SC-1|ADR-0012|ADR-0039");
  const auto file = std::filesystem::path(AM_MANIFEST_SCHEMA).parent_path() / "examples/manifest-1.0.example.json";
  std::ifstream stream(file, std::ios::binary);
  auto j = imp::PlanJson::parse(std::string{std::istreambuf_iterator<char>(stream), {}});
  std::string original, encoded;
  for (unsigned i = 0; i < 2048; ++i) {
    original += "a/\xef\xbf\xbd";
    encoded += "YS//";
  }
  original += "file";
  encoded += "ZmlsZQ==";
  auto& entry = j["inputs"][0]["entries"][0];
  entry["path"] = original;
  entry["path_raw"] = {{"base64", encoded}, {"encoding_guess", "posix"}};
  const auto text = j.dump();
  const auto doc = am::manifest::parse_manifest(text);
  ASSERT_TRUE(doc) << am::to_log_string(doc.error());
  auto sink = std::make_shared<am::asset_zip::MemorySink>();
  auto writer = am::asset_zip::ZipWriter::create(sink);
  ASSERT_TRUE(writer);
  am::asset_zip::EntryOptions options;
  options.uncompressed_size = text.size();
  auto zip_entry = writer->begin_entry("manifest.json", options);
  ASSERT_TRUE(zip_entry);
  ASSERT_TRUE(zip_entry->write(std::as_bytes(std::span(text.data(), text.size()))));
  ASSERT_TRUE(zip_entry->finish());
  ASSERT_TRUE(writer->finish());
  const auto zip_bytes = sink->bytes();
  auto source = am::asset_zip::memory_source(std::vector<std::byte>(zip_bytes.begin(), zip_bytes.end()));
  ASSERT_TRUE(source);
  auto probe = am::asset_zip::probe_container(*source);
  ASSERT_TRUE(probe);
  EXPECT_EQ(probe->outcome, am::asset_zip::ProbeOutcome::own);
  std::ifstream schema_file(AM_MANIFEST_SCHEMA, std::ios::binary);
  auto schema = am::manifest::SchemaValidator::create(std::string{std::istreambuf_iterator<char>(schema_file), {}});
  ASSERT_TRUE(schema);
  const auto valid = schema->validate(*doc);
  std::cout << "RAW_MANIFEST base64_bytes=" << encoded.size() << " display_bytes=" << original.size()
            << " V1=PASS V2=" << (valid ? "PASS" : "FAIL") << '\n';
  ASSERT_TRUE(valid) << valid.error().message_id << ": " << valid.error().ctx.detail;
}
TEST(ImportRunManifest, PublishedPatternSurveyOnMsvc) {
  RecordProperty("req", "ADR-0038|ADR-0039");
#ifndef _WIN32
  GTEST_SKIP() << "This measurement targets the MSVC std::regex implementation";
#else
  std::ifstream input(AM_MANIFEST_SCHEMA, std::ios::binary);
  const auto schema = imp::PlanJson::parse(std::string{std::istreambuf_iterator<char>(input), {}});
  std::string plugin, key;
  for (unsigned i = 0; i < 47; ++i) plugin += "a.";
  plugin += "aa";
  for (unsigned i = 0; i < 31; ++i) key += "a.";
  key += "aa";
  std::function<void(const imp::PlanJson&, std::string)> visit = [&](const auto& spec, std::string pointer) {
    if (spec.is_object()) {
      if (spec.contains("pattern")) {
        const auto pattern = spec.at("pattern").template get<std::string>();
        std::function<std::string(std::size_t)> sample;
        std::string fixed;
        if (pointer == "/$defs/uuid4")
          fixed = "550e8400-e29b-41d4-a716-446655440000";
        else if (pointer == "/$defs/sha256")
          fixed = std::string(64, 'f');
        else if (pointer == "/$defs/crc32")
          fixed = std::string(8, 'f');
        else if (pointer == "/$defs/uint64")
          fixed = "18446744073709551615";
        else if (pointer == "/$defs/int64String")
          fixed = "-9223372036854775808";
        else if (pointer == "/$defs/decimalString")
          sample = [](auto n) { return "1." + std::string(n, '1'); };
        else if (pointer == "/$defs/timestamp")
          sample = [](auto n) { return "2026-11-24T00:00:00." + std::string(n, '1') + "Z"; };
        else if (pointer == "/$defs/date")
          fixed = "2026-11-24";
        else if (pointer == "/$defs/url")
          sample = [](auto n) { return "https://example.invalid/" + std::string(n, 'a'); };
        else if (pointer == "/$defs/storagePath")
          fixed = "content/550e8400-e29b-41d4-a716-446655440000/" + std::string(155, 'a');
        else if (pointer == "/$defs/pluginId")
          fixed = plugin;
        else if (pointer == "/$defs/rawName/properties/base64")
          sample = [](auto n) { return std::string(n * 4, 'A'); };
        else if (pointer == "/$defs/file/properties/type/properties/id")
          sample = [](auto n) {
            std::string value = "a";
            for (std::size_t i = 0; i < n; ++i) value += ".a";
            return value;
          };
        else if (pointer == "/$defs/claimCore/properties/confidence")
          sample = [](auto n) { return "0." + std::string(n, '1'); };
        else if (pointer == "/$defs/extension/properties/attachments/items/properties/path")
          fixed = "extensions/" + std::string(189, 'a');
        else if (pattern.starts_with("^core:"))
          fixed = "core:" + key;
        else if (pattern.starts_with("^[a-z][a-z0-9_"))
          fixed = std::string(64, 'a');
        else if (pointer == "/$defs/fieldId" || pattern.starts_with("^plugin:"))
          fixed = "plugin:" + plugin + ":" + key;
        else
          FAIL() << "Missing survey sample for " << pointer;
        const std::regex regex(pattern, std::regex::ECMAScript);
        auto check = [&](const std::string& text) {
          try {
            const auto accepted = std::regex_search(text.c_str(), regex);
            EXPECT_TRUE(accepted) << pointer << " length=" << text.size();
            return true;
          } catch (const std::regex_error&) {
            return false;
          }
        };
        std::size_t last_pass = 0, first_error = 0, checked = 0;
        if (!sample) {
          checked = fixed.size();
          if (check(fixed))
            last_pass = fixed.size();
          else
            first_error = fixed.size();
        } else {
          std::size_t low = 0, high = 0;
          for (std::size_t n = 1; n <= 32768; n *= 2) {
            const auto text = sample(n);
            checked = text.size();
            if (check(text)) {
              last_pass = text.size();
              low = n;
            } else {
              high = n;
              break;
            }
          }
          if (high) {
            while (high - low > 1) {
              const auto middle = low + (high - low) / 2;
              if (check(sample(middle)))
                low = middle;
              else
                high = middle;
            }
            last_pass = sample(low).size();
            first_error = sample(high).size();
          }
        }
        std::cout << "PATTERN_SURVEY "
                  << imp::PlanJson{{"pointer", pointer},
                                   {"pattern", pattern},
                                   {"checked_bytes", checked},
                                   {"last_pass_bytes", last_pass},
                                   {"first_error_bytes", first_error}}
                         .dump()
                  << '\n';
      }
      for (auto i = spec.begin(); i != spec.end(); ++i) visit(i.value(), pointer + "/" + i.key());
    } else if (spec.is_array())
      for (std::size_t i = 0; i < spec.size(); ++i) visit(spec.at(i), pointer + "/" + std::to_string(i));
  };
  visit(schema, "");
#endif
}

namespace {
imp::PlanJson manifest_example() {
  const auto file = std::filesystem::path(AM_MANIFEST_SCHEMA).parent_path() / "examples/manifest-1.0.example.json";
  std::ifstream stream(file, std::ios::binary);
  return imp::PlanJson::parse(std::string{std::istreambuf_iterator<char>(stream), {}});
}
std::string schema_bytes() {
  std::ifstream stream(AM_MANIFEST_SCHEMA, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}
am::manifest::SchemaValidator schema_validator(std::string text) {
  auto result = am::manifest::SchemaValidator::create(text);
  if (!result) throw std::runtime_error(am::to_log_string(result.error()));
  return std::move(*result);
}
}  // namespace
TEST(ImportRunManifest, EmbeddedSchemaIsByteExactAndRawReferenceSitesMatch) {
  RecordProperty("req", "ADR-0039|ADR-0024");
  std::string embedded;
  for (const auto part : am::manifest::published_schema_parts()) embedded += part;
  EXPECT_EQ(embedded, schema_bytes());
  const auto schema = imp::PlanJson::parse(embedded);
  std::map<std::string, std::set<std::string>> references;
  std::function<void(const imp::PlanJson&, std::string)> visit = [&](const auto& j, std::string at) {
    if (j.is_object()) {
      if (j.contains("$ref")) references[j.at("$ref").template get<std::string>()].insert(at);
      for (auto i = j.begin(); i != j.end(); ++i) visit(i.value(), at + "/" + i.key());
    } else if (j.is_array())
      for (std::size_t i = 0; i < j.size(); ++i) visit(j.at(i), at + "/" + std::to_string(i));
  };
  visit(schema, "");
  const std::set<std::string> raw_sites{"/$defs/inputObject/properties/name_raw",
                                        "/$defs/inputEntry/properties/path_raw"};
  EXPECT_EQ(references["#/$defs/rawName"], raw_sites);
  EXPECT_EQ(references["#/$defs/file"], std::set<std::string>{"/properties/files/items"});
  auto validator = schema_validator(schema.dump(4));
  for (const auto& location :
       {std::string("/inputs/0/objects/0/name_raw"), std::string("/inputs/0/entries/0/path_raw")}) {
    auto j = manifest_example();
    j[imp::PlanJson::json_pointer(location)] = {{"base64", std::string(65536, 'A')}, {"encoding_guess", "posix"}};
    auto doc = am::manifest::parse_manifest(j.dump());
    ASSERT_TRUE(doc);
    auto before = am::manifest::write_manifest(*doc);
    ASSERT_TRUE(before);
    auto valid = validator.validate(*doc);
    ASSERT_TRUE(valid) << valid.error().ctx.detail;
    EXPECT_EQ(am::manifest::write_manifest(*doc).value(), *before);
    j[imp::PlanJson::json_pointer(location + "/base64")] = "-___";
    doc = am::manifest::parse_manifest(j.dump());
    ASSERT_TRUE(doc);
    valid = validator.validate(*doc);
    ASSERT_FALSE(valid);
    EXPECT_EQ(valid.error().message_id, "am.manifest.schema_violation");
    EXPECT_EQ(valid.error().code, am::Errc::corrupt_data);
    EXPECT_NE(valid.error().ctx.detail.find(location + "/base64"), std::string::npos);
  }
}
TEST(ImportRunManifest, LinearPatternDecisionsEqualRegexOnShortStrings) {
  RecordProperty("req", "ADR-0039|ADR-0038");
  const auto schema = imp::PlanJson::parse(schema_bytes());
  auto validator = schema_validator(schema_bytes());
  const auto raw_regex = schema["$defs"]["rawName"]["properties"]["base64"]["pattern"].get<std::string>();
  const auto type_regex =
      schema["$defs"]["file"]["properties"]["type"]["properties"]["id"]["pattern"].get<std::string>();
  EXPECT_EQ(type_regex, "^[a-z0-9]+(\\.[a-z0-9_-]+)*$");
  const std::vector<std::string> base64{"",
                                        "A",
                                        "AA",
                                        "AAA",
                                        "AAAA",
                                        "a0+/",
                                        "Zz09",
                                        "=",
                                        "==",
                                        "===",
                                        "====",
                                        "A===",
                                        "AA==",
                                        "AB==",
                                        "AAA=",
                                        "AAB=",
                                        "AAAA=",
                                        "AAAA==",
                                        "AAAA===",
                                        "AAAA====",
                                        "=AAA",
                                        "A=AA",
                                        "AA=A",
                                        "AA==AAAA",
                                        "-_AA",
                                        "AA A",
                                        "AAAA\n",
                                        "\nAAAA",
                                        "AAAA\r\n",
                                        "AAAA\t",
                                        std::string("\xc3\xa9") + "AAA",
                                        std::string("AAAA\0", 5)};
  const std::vector<std::string> type{
      "",   "a",  "0",   "9",   "z", "a0",  "0.a", "a.0", "a._", "a.-",  "a.a_b-c",    "a..b",
      ".a", "a.", "a_b", "a-b", "A", "a.A", "a/b", "a:b", "a b", "a\nb", "a.\xc3\xa9", std::string("a\0", 2)};
  for (const bool raw : {true, false}) {
    const std::regex regex(raw ? raw_regex : type_regex, std::regex::ECMAScript);
    for (const auto& text : raw ? base64 : type) {
      SCOPED_TRACE(imp::PlanJson(text).dump());
      auto j = manifest_example();
      if (raw)
        j["inputs"][0]["entries"][0]["path_raw"] = {{"base64", text}};
      else
        j["files"][0]["type"]["id"] = text;
      auto doc = am::manifest::parse_manifest(j.dump());
      ASSERT_TRUE(doc);
      const auto valid = validator.validate(*doc);
      EXPECT_EQ(static_cast<bool>(valid), std::regex_match(text, regex));
    }
  }
  for (const auto ending : {"\n", "\r", "\r\n"})
    for (const bool raw : {true, false}) {
      const std::string good = raw ? "AAAA" : "a.b";
      for (const auto& text : {std::string(ending) + good, good + ending, good + ending + good}) {
        auto j = manifest_example();
        if (raw)
          j["inputs"][0]["entries"][0]["path_raw"] = {{"base64", text}};
        else
          j["files"][0]["type"]["id"] = text;
        auto doc = am::manifest::parse_manifest(j.dump());
        ASSERT_TRUE(doc);
        EXPECT_FALSE(validator.validate(*doc)) << imp::PlanJson(text).dump();
      }
    }
}
TEST(ImportRunManifest, LongFileTypeAndCustomSchemasKeepTheirOwnSemantics) {
  RecordProperty("req", "ADR-0039|ADR-0038");
  auto j = manifest_example();
  std::string type = "a";
  for (unsigned i = 0; i < 32768; ++i) type += ".a";
  j["files"][0]["type"]["id"] = type;
  auto doc = am::manifest::parse_manifest(j.dump());
  ASSERT_TRUE(doc);
  auto normative = schema_validator(schema_bytes());
  const auto before = am::manifest::write_manifest(*doc);
  ASSERT_TRUE(before);
  auto valid = normative.validate(*doc);
  ASSERT_TRUE(valid) << valid.error().ctx.detail;
  EXPECT_EQ(am::manifest::write_manifest(*doc).value(), *before);
  std::cout << "FILE_TYPE_PATTERN bytes=" << type.size() << " V2=PASS\n";
  j = manifest_example();
  j["inputs"][0]["entries"][0]["path_raw"] = {{"base64", "AQID"}};
  doc = am::manifest::parse_manifest(j.dump());
  ASSERT_TRUE(doc);
  auto custom = imp::PlanJson::parse(schema_bytes());
  custom["properties"]["inputs"]["const"] = j["inputs"];
  auto exact = schema_validator(custom.dump());
  EXPECT_TRUE(exact.validate(*doc));
  custom = imp::PlanJson::parse(schema_bytes());
  auto& pattern = custom["$defs"]["rawName"]["properties"]["base64"]["pattern"];
  auto changed = pattern.get<std::string>();
  changed[changed.find('A')] = 'B';
  pattern = changed;
  auto restricted = schema_validator(custom.dump());
  EXPECT_FALSE(restricted.validate(*doc));
  EXPECT_TRUE(normative.validate(*doc));
  custom = imp::PlanJson::parse(schema_bytes());
  custom["$comment"] = "one added field";
  custom["properties"]["inputs"]["const"] = j["inputs"];
  auto annotated = schema_validator(custom.dump());
  EXPECT_TRUE(annotated.validate(*doc));
}

TEST(ImportRunManifest, AnchoredPatternSurveyReportsV2AndIndependentV3) {
  RecordProperty("req", "ADR-0039|ADR-0038");
#ifndef _WIN32
  GTEST_SKIP() << "This measurement records the MSVC regex_search anchoring defect";
#else
  const auto schema = imp::PlanJson::parse(schema_bytes());
  auto normative = schema_validator(schema_bytes());
  std::function<void(const imp::PlanJson&, std::string)> visit = [&](const auto& spec, std::string schema_at) {
    if (spec.is_object()) {
      if (spec.contains("pattern")) {
        const auto pattern = spec.at("pattern").template get<std::string>();
        auto j = manifest_example();
        std::string location, sample;
        bool extension_key = false;
        const auto plugin = std::string("plugin:org.example:test");
        if (schema_at == "/$defs/uuid4") {
          location = "/asset/id";
          sample = j["asset"]["id"];
        } else if (schema_at == "/$defs/sha256") {
          location = "/files/0/sha256";
          sample = j["files"][0]["sha256"];
        } else if (schema_at == "/$defs/crc32") {
          location = "/inputs/0/entries/0/crc32";
          sample = "f3c218f0";
        } else if (schema_at == "/$defs/uint64") {
          location = "/files/0/size_bytes";
          sample = j["files"][0]["size_bytes"];
        } else if (schema_at == "/$defs/int64String" || schema_at == "/$defs/decimalString") {
          location = "/asset/metadata/custom_values/0/value";
          sample = schema_at == "/$defs/int64String" ? "42" : "1.25";
          const auto id = j["field_definitions"][0]["id"];
          j["field_definitions"][0] = {{"id", id},
                                       {"schema_revision", 1},
                                       {"name", "Survey"},
                                       {"type", schema_at == "/$defs/int64String" ? "integer" : "decimal"},
                                       {"scopes", imp::PlanJson::array({"asset"})}};
          j["asset"]["metadata"]["custom_values"][0] = {{"field_id", id}, {"value", sample}};
        } else if (schema_at == "/$defs/timestamp") {
          location = "/asset/created_at";
          sample = j["asset"]["created_at"];
        } else if (schema_at == "/$defs/date") {
          location = "/asset/metadata/dates/acquired";
          sample = "2026-11-24";
        } else if (schema_at == "/$defs/url") {
          location = "/asset/metadata/links/0/url";
          sample = "https://example.invalid/a";
          j["asset"]["metadata"]["links"] = imp::PlanJson::array({{{"url", sample}, {"rel", "related"}}});
        } else if (schema_at == "/$defs/storagePath") {
          location = "/files/0/storage_path";
          sample = j["files"][0]["storage_path"];
        } else if (schema_at == "/$defs/pluginId") {
          location = "/extensions/<key>";
          sample = j["extensions"].begin().key();
          extension_key = true;
        } else if (schema_at == "/$defs/fieldId") {
          location = "/field_definitions/0/id";
          sample = j["field_definitions"][0]["id"];
        } else if (schema_at == "/$defs/typeId/anyOf/1") {
          location = "/field_definitions/0/type";
          sample = plugin;
        } else if (schema_at.starts_with("/$defs/relKey/")) {
          location = "/asset/metadata/links/0/rel";
          sample = schema_at.ends_with("/0") ? "related" : plugin;
          j["asset"]["metadata"]["links"] =
              imp::PlanJson::array({{{"url", "https://example.invalid/a"}, {"rel", sample}}});
        } else if (schema_at == "/$defs/rawName/properties/base64") {
          location = "/inputs/0/entries/0/path_raw/base64";
          sample = "AQID";
        } else if (schema_at == "/$defs/file/properties/type/properties/id") {
          location = "/files/0/type/id";
          sample = "mesh.stl";
        } else if (schema_at.starts_with("/$defs/file/properties/generation/properties/tool/")) {
          location = "/files/0/generation/tool";
          sample = schema_at.ends_with("/0") ? "core:test" : plugin;
        } else if (schema_at.starts_with("/$defs/fieldDefinition/properties/dimension/")) {
          location = "/field_definitions/0/dimension";
          sample = schema_at.ends_with("/0") ? "dimensionless" : plugin;
        } else if (schema_at == "/$defs/claimCore/properties/confidence") {
          location = "/asset/metadata/claims/0/confidence";
          sample = "0.5";
        } else if (schema_at == "/$defs/extension/properties/attachments/items/properties/path") {
          const auto key = j["extensions"].begin().key();
          location = "/extensions/" + key + "/attachments/0/path";
          sample = "extensions/" + key + "/a";
          j["extensions"][key]["attachments"] = imp::PlanJson::array({{{"id", "11111111-1111-4111-8111-111111111111"},
                                                                       {"path", sample},
                                                                       {"size_bytes", "0"},
                                                                       {"sha256", std::string(64, '0')}}});
        } else if (schema_at.starts_with("/$defs/revisionRecord/properties/operation/")) {
          location = "/history/entries/0/operation";
          sample = schema_at.ends_with("/0") ? "asset_created" : plugin;
        } else if (schema_at.starts_with("/$defs/revisionRecord/properties/changes/items/properties/kind/")) {
          location = "/history/entries/0/changes/0/kind";
          sample = schema_at.ends_with("/0") ? "metadata_set" : plugin;
        } else
          FAIL() << "Missing anchor survey placement: " << schema_at;
        ASSERT_TRUE(std::regex_search(sample, std::regex(pattern))) << schema_at;
        if (!extension_key) j[imp::PlanJson::json_pointer(location)] = sample;
        auto base_doc = am::manifest::parse_manifest(j.dump());
        ASSERT_TRUE(base_doc) << schema_at;
        auto base_sem = am::manifest::validate_semantics(*base_doc);
        ASSERT_TRUE(base_sem);
        std::set<std::string> baseline;
        for (const auto& f : base_sem->findings)
          if (f.severity == am::manifest::FindingSeverity::error)
            baseline.insert(f.rule + ":" + f.pointer + ":" + f.detail);
        // A caller-supplied leaf schema exercises the unchanged jsoncons path on a
        // known string, including definitions not directly referenced by V2 values.
        auto leaf = imp::PlanJson{
            {"properties", {{"asset", {{"properties", {{"metadata", {{"properties", {{"title", spec}}}}}}}}}}}};
        auto engine = schema_validator(leaf.dump());
        const auto base_valid = normative.validate(*base_doc);
        imp::PlanJson row{{"pointer", schema_at}, {"sample",sample},
                          {"instance", location},
                          {"baseline_v2", static_cast<bool>(base_valid)},
                          {"baseline_v3_errors", baseline.size()}};
        for (const bool prefix : {false, true}) {
          const auto value = prefix ? "X\n" + sample : sample + "\nX";
          auto probe = manifest_example();
          probe["asset"]["metadata"]["title"] = value;
          auto probe_doc = am::manifest::parse_manifest(probe.dump());
          ASSERT_TRUE(probe_doc);
          auto actual = j;
          if (extension_key) {
            const auto content = actual["extensions"][sample];
            actual["extensions"].erase(sample);
            actual["extensions"][value] = content;
          } else
            actual[imp::PlanJson::json_pointer(location)] = value;
          auto doc = am::manifest::parse_manifest(actual.dump());
          imp::PlanJson outcome{{"engine_v2", static_cast<bool>(engine.validate(*probe_doc))},
                                {"v1", static_cast<bool>(doc)}};
          if (doc) {
            outcome["normative_v2"] = static_cast<bool>(normative.validate(*doc));
            auto semantic = am::manifest::validate_semantics(*doc);
            ASSERT_TRUE(semantic);
            outcome["new_v3_errors"] = imp::PlanJson::array();
            for (const auto& f : semantic->findings)
              if (f.severity == am::manifest::FindingSeverity::error &&
                  !baseline.contains(f.rule + ":" + f.pointer + ":" + f.detail))
                outcome["new_v3_errors"].push_back(f.rule + ":" + f.pointer);
          }
          row[prefix ? "prefix" : "suffix"] = std::move(outcome);
        }
        std::cout << "ANCHOR_SURVEY " << row.dump() << '\n';
      }
      for (auto i = spec.begin(); i != spec.end(); ++i) visit(i.value(), schema_at + "/" + i.key());
    } else if (spec.is_array())
      for (std::size_t i = 0; i < spec.size(); ++i) visit(spec.at(i), schema_at + "/" + std::to_string(i));
  };
  visit(schema, "");
#endif
}
