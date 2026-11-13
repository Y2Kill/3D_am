#include <gtest/gtest.h>
#include "am/domain/manifest/codec.hpp"
#include "am/domain/manifest/semantics.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using namespace am::manifest;
using Json = nlohmann::ordered_json;
std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read test input: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string fixture(std::string_view name = "valid_control.json") { return read(std::filesystem::path(AM_MANIFEST_FIXTURES) / name); }
SchemaValidator validator() {
  auto result = SchemaValidator::create(read(AM_MANIFEST_SCHEMA));
  if (!result) throw std::runtime_error(am::to_log_string(result.error()));
  return std::move(result).value();
}
std::string with_data(std::string_view raw) {
  auto base = Json::parse(fixture());
  base["extensions"]["org.example.s7one"]["data"] = "RAW_TEST_VALUE";
  auto text = base.dump();
  const auto start = text.find("\"RAW_TEST_VALUE\"");
  text.replace(start, std::string("\"RAW_TEST_VALUE\"").size(), raw);
  return text;
}
void expect_id(const auto& result, std::string_view id) {
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().message_id, id) << am::to_log_string(result.error());
}
std::string with_history_changes(std::string_view changes) {
  auto base = Json::parse(read(std::filesystem::path(AM_MANIFEST_SCHEMA).parent_path() / "examples/manifest-1.0.example.json"));
  base["history"]["entries"][0]["changes"] = "RAW_HISTORY_CHANGES";
  auto text = base.dump();
  const auto start = text.find("\"RAW_HISTORY_CHANGES\"");
  text.replace(start, std::string("\"RAW_HISTORY_CHANGES\"").size(), changes);
  return text;
}
TEST(ManifestCodec, NamedChangeFieldsMatchOnlyBaseVocabularyPairs) {
  RecordProperty("req", "ADR-0039");
  EXPECT_TRUE(is_named_change_field("file_replace", "old_sha256"));
  EXPECT_TRUE(is_named_change_field("file_replace", "new_sha256"));
  EXPECT_TRUE(is_named_change_field("file_move", "old_path"));
  for (const auto& pair : std::vector<std::pair<std::string_view, std::string_view>>{
           {"file_replace", "old_path"}, {"file_move", "old_sha256"}, {"file_move", "new_sha256"},
           {"plugin:org.test.history:replace", "old_sha256"}, {"preview_set_primary", "old_path"},
           {"file_add", "old_sha256"}, {"", "old_path"}, {"file_replace", ""}, {"", ""},
           {"file_replace", "kind"}, {"file_move", "target"}}) {
    EXPECT_FALSE(is_named_change_field(pair.first, pair.second)) << pair.first << " " << pair.second;
  }
}
TEST(ManifestCodec, BaseHistoryStringsAreKnownWhileOtherExtrasKeepExactLexemes) {
  RecordProperty("req", "ADR-0039");
  const auto hash = std::string(64, 'a');
  const auto text = with_history_changes(
      R"([{"kind":"file_replace","target":"file","old_sha256":")" + hash +
      R"(","new_sha256":")" + hash + R"(","note":1.500},)" +
      R"({"kind":"file_move","target":"file","old_path":"content/old.bin","note":-0.0},)" +
      R"({"kind":"preview_set_primary","old_sha256":"unknown","old_path":1e400},)" +
      R"({"kind":"plugin:org.test.history:replace","old_sha256":1e400,"note":1.500}])");
  auto doc = parse_manifest(text);
  ASSERT_TRUE(doc) << am::to_log_string(doc.error());
  const auto& history = doc->known()["history"]["entries"][0];
  const auto& changes = history["changes"];
  EXPECT_EQ(changes[0]["old_sha256"], hash);
  EXPECT_EQ(changes[0]["new_sha256"], hash);
  EXPECT_EQ(changes[1]["old_path"], "content/old.bin");
  EXPECT_TRUE(changes[0]["note"].is_binary());
  EXPECT_TRUE(changes[1]["note"].is_binary());
  EXPECT_TRUE(changes[2]["old_sha256"].is_binary());
  EXPECT_TRUE(changes[2]["old_path"].is_binary());
  EXPECT_TRUE(changes[3]["old_sha256"].is_binary());
  EXPECT_TRUE(changes[3]["note"].is_binary());
  const auto prefix = "/history/" + history["operation_id"].get<std::string>() + "/changes/";
  const auto keys = doc->opaque_keys();
  auto contains = [&](const std::string& suffix) {
    return std::find(keys.begin(), keys.end(), OpaqueKey{prefix + suffix}) != keys.end();
  };
  EXPECT_FALSE(contains("0/old_sha256"));
  EXPECT_FALSE(contains("0/new_sha256"));
  EXPECT_FALSE(contains("1/old_path"));
  for (const auto* suffix : {"0/note", "1/note", "2/old_sha256", "2/old_path", "3/old_sha256", "3/note"})
    EXPECT_TRUE(contains(suffix)) << suffix;
  auto written = write_manifest(*doc);
  ASSERT_TRUE(written);
  EXPECT_EQ(*written, text);
  EXPECT_TRUE(verify_preserved(text, *written));
  auto changed_lexeme = text;
  changed_lexeme.replace(changed_lexeme.find("1.500"), 5, "1.5");
  expect_id(verify_preserved(text, changed_lexeme), "am.manifest.preservation_failed");
  doc->known()["asset"]["metadata"]["title"] = "Edited with existing replacement history";
  auto edited = write_manifest(*doc);
  ASSERT_TRUE(edited);
  EXPECT_TRUE(verify_preserved(text, *edited));
}
TEST(ManifestCodec, NonStringBaseHistoryFieldsStayOpaqueAndProduceV3Errors) {
  RecordProperty("req", "ADR-0039");
  for (const auto* key : {"old_sha256", "new_sha256", "old_path"}) {
    for (const auto* invalid : {"1e400", "null", "[]", "{\"number\":1.500}"}) {
      SCOPED_TRACE(std::string(key) + " " + invalid);
      Json change;
      if (std::string_view(key) == "old_path")
        change = {{"kind", "file_move"}, {"target", "file"}, {"old_path", "RAW_BAD_FIELD"}};
      else {
        change = {{"kind", "file_replace"}, {"target", "file"},
                  {"old_sha256", std::string(64, 'a')}, {"new_sha256", std::string(64, 'b')}};
        change[key] = "RAW_BAD_FIELD";
      }
      auto changes = Json::array({change}).dump();
      changes.replace(changes.find("\"RAW_BAD_FIELD\""), std::string("\"RAW_BAD_FIELD\"").size(), invalid);
      const auto text = with_history_changes(changes);
      auto doc = parse_manifest(text);
      ASSERT_TRUE(doc) << am::to_log_string(doc.error());
      EXPECT_TRUE(doc->known()["history"]["entries"][0]["changes"][0][key].is_binary());
      auto written = write_manifest(*doc);
      ASSERT_TRUE(written);
      EXPECT_EQ(*written, text);
      EXPECT_TRUE(verify_preserved(text, *written));
      auto report = validate_semantics(*doc);
      ASSERT_TRUE(report);
      ASSERT_TRUE(report->complete);
      ASSERT_EQ(report->findings.size(), 1u);
      EXPECT_EQ(report->findings[0].rule, "change_kind_fields");
      EXPECT_EQ(report->findings[0].severity, FindingSeverity::error);
      EXPECT_EQ(report->findings[0].pointer, "/history/entries/0/changes/0/" + std::string(key));
    }
  }
}
TEST(ManifestCodec, EveryValidFixtureRoundTripsAndValidates) {
  RecordProperty("req", "ADR-0038|R-09.8");
  auto schema = validator();
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(AM_MANIFEST_FIXTURES)) {
    if (!entry.path().filename().string().starts_with("valid_")) continue;
    const auto text = read(entry.path());
    auto parsed = parse_manifest(text);
    ASSERT_TRUE(parsed) << entry.path() << ' ' << am::to_log_string(parsed.error());
    EXPECT_TRUE(schema.validate(*parsed)) << entry.path();
    auto written = write_manifest(*parsed);
    ASSERT_TRUE(written) << entry.path() << ' ' << am::to_log_string(written.error());
    EXPECT_TRUE(verify_preserved(text, *written)) << entry.path();
    auto again = parse_manifest(*written);
    ASSERT_TRUE(again);
    EXPECT_EQ(parsed->known(), again->known()) << entry.path();
    ++count;
  }
  EXPECT_EQ(count, 14u);
}
TEST(ManifestCodec, StructuralEditRetainsOwnersAfterIndexShift) {
  RecordProperty("req", "ADR-0038|R-09.8");
  const auto text = fixture("valid_all.json");
  auto doc = parse_manifest(text);
  ASSERT_TRUE(doc);
  const auto owners = doc->opaque_keys();
  auto& dom = doc->known();
  dom["asset"]["metadata"]["title"] = "Edited title";
  auto& values = dom["asset"]["metadata"]["custom_values"];
  values.erase(values.begin());
  const auto revision = dom["asset"]["revision"].get<std::uint64_t>() + 1;
  dom["asset"]["revision"] = revision;
  dom["asset"]["modified_at"] = "2026-11-11T00:00:00Z";
  dom["history"]["entries"].push_back({{"revision", revision}, {"operation_id", "12345678-1234-4123-8123-000000000006"},
      {"timestamp", "2026-11-11T00:00:00Z"}, {"operation", "metadata_changed"}});
  EXPECT_EQ(doc->opaque_keys(), owners);
  auto text_after = write_manifest(*doc);
  ASSERT_TRUE(text_after);
  EXPECT_TRUE(verify_preserved(text, *text_after));
  EXPECT_TRUE(validator().validate(*doc));
}
TEST(ManifestCodec, GuardRejectsUnannouncedChangesAndHonorsExplicitIntent) {
  RecordProperty("req", "ADR-0038|R-09.8");
  const auto original = fixture("valid_trailing_zeros.json");
  auto lexeme = original;
  const auto offset = lexeme.find("1.500"); ASSERT_NE(offset, std::string::npos);
  lexeme.replace(offset, 5, "1.5");
  expect_id(verify_preserved(original, lexeme), "am.manifest.preservation_failed");
  auto doc = parse_manifest(original); ASSERT_TRUE(doc);
  const auto keys = doc->opaque_keys();
  const auto key = std::find_if(keys.begin(), keys.end(), [](const auto& k) { return k.value == "/extensions/org.example.s7one/data"; });
  ASSERT_NE(key, keys.end());
  doc->known()["extensions"].erase("org.example.s7one");
  auto after = write_manifest(*doc); ASSERT_TRUE(after);
  expect_id(verify_preserved(original, *after), "am.manifest.preservation_failed");
  IntendedChanges intent; intent.owners.insert(*key);
  EXPECT_TRUE(verify_preserved(original, *after, intent));
  // A rename is a deletion plus an addition, not the same owner at another index.
  doc = parse_manifest(original); ASSERT_TRUE(doc);
  auto& extensions = doc->known()["extensions"];
  auto moved = extensions["org.example.s7one"]; extensions.erase("org.example.s7one");
  extensions["org.example.renamed"] = std::move(moved);
  after = write_manifest(*doc); ASSERT_TRUE(after);
  expect_id(verify_preserved(original, *after), "am.manifest.preservation_failed");
  EXPECT_FALSE(verify_preserved(original, *after, intent));
  intent.owners.insert({"/extensions/org.example.renamed/data"});
  EXPECT_TRUE(verify_preserved(original, *after, intent));
  // Unknown keys, types, array positions and decoded strings are protected too.
  const std::string a = R"({"extensions":{"p":{"data":{"a":[1,"x",true,{}]}}}})";
  for (const auto b : {R"({"extensions":{"p":{"data":{"b":[1,"x",true,{}]}}}})",
                       R"({"extensions":{"p":{"data":{"a":["x",1,true,{}]}}}})",
                       R"({"extensions":{"p":{"data":{"a":[1,"y",true,{}]}}}})",
                       R"({"extensions":{"p":{"data":{"a":[1,"x",false,{}]}}}})",
                       R"({"extensions":{"p":{"data":{"a":[1,"x",true,[]]}}}})"})
    expect_id(verify_preserved(a, b), "am.manifest.preservation_failed");
  EXPECT_TRUE(verify_preserved(a, R"({"extensions":{"p":{"data":{"a":[1,"\u0078",true,{}]}}}})"));
}
TEST(ManifestCodec, AmbiguousOwnersAndDuplicateKeysBlockWrites) {
  RecordProperty("req", "ADR-0038|R-09.8");
  expect_id(parse_manifest(R"({"extensions":{"p":{"data":1},"p":{"data":2}}})"), "am.manifest.duplicate_key");
  const auto text = fixture();
  for (const auto array : {"files", "groups", "field_definitions"}) {
    auto doc = parse_manifest(text); ASSERT_TRUE(doc);
    auto& list = doc->known()[array];
    ASSERT_FALSE(list.empty());
    list.push_back(list.front());
    expect_id(write_manifest(*doc), "am.manifest.write_blocked");
  }
  auto doc = parse_manifest(text); ASSERT_TRUE(doc);
  auto& entries = doc->known()["history"]["entries"];
  // No binary duplication is involved: ambiguity alone must block the write.
  entries[1]["operation_id"] = entries[0]["operation_id"];
  expect_id(write_manifest(*doc), "am.manifest.write_blocked");
  auto invalid = Json::parse(text);
  invalid["history"]["entries"][1]["operation_id"] = invalid["history"]["entries"][0]["operation_id"];
  expect_id(verify_preserved(text, invalid.dump()), "am.manifest.write_blocked");
}
TEST(ManifestCodec, HandlesCannotBeForgedRepeatedOrOutOfRange) {
  RecordProperty("req", "ADR-0038|R-09.8");
  const std::string text = R"({"plain":{"bytes":[0,0,0,0,0,0,0,0],"subtype":null},"marker":"S7_FRAGMENT_0","extensions":{"p":{"data":1e400}}})";
  auto doc = parse_manifest(text); ASSERT_TRUE(doc);
  EXPECT_TRUE(doc->known()["plain"].is_object());
  EXPECT_FALSE(doc->known()["plain"].is_binary());
  auto output = write_manifest(*doc); ASSERT_TRUE(output);
  EXPECT_TRUE(verify_preserved(text, *output));
  for (const auto& bad : {Json::binary({1}), Json::binary(std::vector<std::uint8_t>(8, 255))}) {
    doc->known()["plain"] = bad;
    expect_id(write_manifest(*doc), "am.manifest.write_blocked");
  }
  doc = parse_manifest(text); ASSERT_TRUE(doc);
  doc->known()["plain"] = doc->known()["extensions"]["p"]["data"];
  expect_id(write_manifest(*doc), "am.manifest.write_blocked");
  // Moving (not copying) a valid handle into a known field cannot bypass V2.
  doc = parse_manifest(text); ASSERT_TRUE(doc);
  doc->known()["plain"] = doc->known()["extensions"]["p"]["data"];
  doc->known()["extensions"]["p"].erase("data");
  expect_id(write_manifest(*doc), "am.manifest.write_blocked");
  expect_id(validator().validate(*doc), "am.manifest.write_blocked");
  // An ordinary JSON value cannot silently replace a raw root either.
  doc = parse_manifest(text); ASSERT_TRUE(doc);
  doc->known()["extensions"]["p"]["data"] = 1;
  expect_id(write_manifest(*doc), "am.manifest.write_blocked");
}
TEST(ManifestCodec, NewFragmentsAreStrictAndIntentIsRequiredForReplacement) {
  RecordProperty("req", "ADR-0038|R-09.8");
  const auto original = fixture(); auto doc = parse_manifest(original); ASSERT_TRUE(doc);
  for (const auto raw : {"1,\"x\":2", "1}", "{} []", "{\"x\":1,\"x\":2}"}) EXPECT_FALSE(doc->make_opaque(raw));
  auto raw = doc->make_opaque("[1e400,-0,1.500]"); ASSERT_TRUE(raw);
  doc->known()["extensions"]["org.example.s7one"]["data"] = std::move(raw).value();
  auto after = write_manifest(*doc); ASSERT_TRUE(after);
  EXPECT_NE(after->find("[1e400,-0,1.500]"), std::string::npos);
  expect_id(verify_preserved(original, *after), "am.manifest.preservation_failed");
  IntendedChanges intent; intent.owners.insert({"/extensions/org.example.s7one/data"});
  EXPECT_TRUE(verify_preserved(original, *after, intent));
  // Newly added owner requires explicit consent too.
  auto added = doc->make_opaque("null"); ASSERT_TRUE(added);
  doc->known()["extensions"]["org.example.added"] = {{"schema_version", "1"}, {"data", std::move(added).value()}};
  after = write_manifest(*doc); ASSERT_TRUE(after);
  EXPECT_FALSE(verify_preserved(original, *after, intent));
  intent.owners.insert({"/extensions/org.example.added/data"});
  EXPECT_TRUE(verify_preserved(original, *after, intent));
}
TEST(ManifestCodec, UnresolvedFieldIdsRemainOpaqueAcrossMetadataScopes) {
  RecordProperty("req", "ADR-0038|R-09.8");
  const std::string text = R"({"asset":{"metadata":{"custom_values":[{"field_id":"missing","value":1e400}],"claims":[{"id":"c1","target":{"kind":"custom","field_id":"missing"},"value":-0}]}},"files":[{"id":"f1","metadata":{"custom_values":[{"field_id":"missing","value":1.500}]}}],"groups":[{"id":"g1","metadata":{"custom_values":[{"field_id":"missing","value":-1E-400}]}}]})";
  auto doc = parse_manifest(text); ASSERT_TRUE(doc);
  EXPECT_EQ(doc->opaque_keys().size(), 4u);
  auto after = write_manifest(*doc); ASSERT_TRUE(after);
  EXPECT_TRUE(verify_preserved(text, *after));
  // Stable scopes survive reordering files, groups, claims and field definitions.
  auto control = Json::parse(fixture());
  auto file = control["files"][0]; file["id"] = "another-file";
  control["files"].push_back(file);
  auto scoped = parse_manifest(control.dump()); ASSERT_TRUE(scoped);
  std::reverse(scoped->known()["files"].begin(), scoped->known()["files"].end());
  std::reverse(scoped->known()["field_definitions"].begin(), scoped->known()["field_definitions"].end());
  std::reverse(scoped->known()["asset"]["metadata"]["claims"].begin(), scoped->known()["asset"]["metadata"]["claims"].end());
  after = write_manifest(*scoped); ASSERT_TRUE(after);
  EXPECT_TRUE(verify_preserved(control.dump(), *after));
  expect_id(parse_manifest(R"({"field_definitions":[{"id":"p","type":"plugin:p:t"}],"asset":{"metadata":{"claims":[{"id":"c","target":{"kind":"standard","field_id":"p"},"value":1e400}]}}})"), "am.manifest.known_number_out_of_range");
}
TEST(ManifestCodec, KnownNumbersUseExactDecimalIntegerArithmetic) {
  RecordProperty("req", "ADR-0038");
  const std::vector<std::pair<std::string, std::string>> valid = {{"1.0", "1"}, {"1e2", "100"}, {"-0", "0"}, {"-0.0", "0"},
      {"0e99999999999999999999999", "0"}, {"0.00100e3", "1"}, {"1000e-3", "1"}, {"-9223372036854775808", "-9223372036854775808"},
      {"18446744073709551615", "18446744073709551615"}, {"1844674407370955161500e-2", "18446744073709551615"}};
  for (const auto& [source, expected] : valid) {
    auto parsed = parse_manifest("{\"revision\":" + source + '}'); ASSERT_TRUE(parsed) << source;
    EXPECT_EQ(parsed->known()["revision"].dump(), expected);
    EXPECT_FALSE(parsed->known()["revision"].is_number_float());
  }
  for (const auto number : {"1.5", "1e400", "18446744073709551616", "-9223372036854775809", "1e-99999999999", "100e-3", "-0.0001"})
    expect_id(parse_manifest("{\"revision\":" + std::string(number) + '}'), "am.manifest.known_number_out_of_range");
}
TEST(ManifestCodec, MutatedKnownDomStillHonorsUtf8NumberAndDepthLimits) {
  RecordProperty("req", "ADR-0038");
  auto doc = parse_manifest("{}"); ASSERT_TRUE(doc);
  doc->known()["text"] = std::string("\xc0\xaf");
  expect_id(write_manifest(*doc), "am.manifest.invalid_json");
  EXPECT_TRUE(doc->opaque_keys().empty());
  doc->known() = Json::object();
  doc->known()[std::string("\xed\xa0\x80")] = "bad key";
  expect_id(write_manifest(*doc), "am.manifest.invalid_json");
  doc->known() = {{"number", 1.5}};
  expect_id(write_manifest(*doc), "am.manifest.known_number_out_of_range");
  doc->known() = {{"number", 1.0}};
  auto integer = write_manifest(*doc); ASSERT_TRUE(integer);
  EXPECT_EQ(*integer, "{\"number\":1}");
  CodecLimits limits; limits.max_known_depth = 2;
  doc = parse_manifest("{}", limits); ASSERT_TRUE(doc);
  doc->known() = Json::array({Json::array({Json::array()})});
  expect_id(write_manifest(*doc), "am.manifest.limit_exceeded");
  CodecLimits bytes; bytes.max_bytes = 4;
  doc = parse_manifest("{}", bytes); ASSERT_TRUE(doc);
  doc->known()["text"] = "oversized";
  expect_id(write_manifest(*doc), "am.manifest.limit_exceeded");
}
TEST(ManifestCodec, EscapedOwnerKeysAuthorizeOnlyTheirExactOwner) {
  RecordProperty("req", "ADR-0038|R-09.8");
  const std::string input = R"({"extensions":{"p/~":{"data":1e400},"p":{"data":-0}}})";
  auto doc = parse_manifest(input); ASSERT_TRUE(doc);
  const auto keys = doc->opaque_keys();
  auto key = std::find_if(keys.begin(), keys.end(), [](const auto& k) { return k.value == "/extensions/p~1~0/data"; });
  ASSERT_NE(key, keys.end());
  doc->known()["extensions"].erase("p/~");
  auto output = write_manifest(*doc); ASSERT_TRUE(output);
  expect_id(verify_preserved(input, *output), "am.manifest.preservation_failed");
  IntendedChanges intent; intent.owners.insert(*key);
  EXPECT_TRUE(verify_preserved(input, *output, intent));
  intent.owners = {{"/extensions/p/data"}};
  EXPECT_FALSE(verify_preserved(input, *output, intent));
}
TEST(ManifestCodec, LongScopeWithManyOwnersUsesInternedComponents) {
  RecordProperty("req", "ADR-0038|R-09.8");
  // V1 must be bounded even before V2 rejects a non-UUID FileID. Do not enumerate
  // printable keys here: their explicit output length is inherently large.
  std::string input = "{\"files\":[{\"id\":\"" + std::string(128 * 1024, 's') + "\",\"metadata\":{\"custom_values\":[";
  for (std::size_t i = 0; i < 10000; ++i) {
    if (i) input += ',';
    input += "{\"field_id\":\"missing" + std::to_string(i) + "\",\"value\":1.500}";
  }
  input += "]}}]}";
  auto doc = parse_manifest(input); ASSERT_TRUE(doc);
  doc->known()["known_edit"] = "changed";
  auto output = write_manifest(*doc); ASSERT_TRUE(output);
  EXPECT_TRUE(verify_preserved(input, *output));
}
TEST(ManifestCodec, ErrorsRetainFoundationCauseAndV2RejectsUnevaluatedProperties) {
  RecordProperty("req", "ADR-0038|R-09.8");
  auto malformed = parse_manifest("{\"x\":01}"); ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error().message_id, "am.manifest.invalid_json");
  ASSERT_TRUE(malformed.error().cause);
  EXPECT_EQ(malformed.error().cause->message_id, "am.json.invalid_json");
  CodecLimits small; small.max_bytes = 1;
  auto limited = parse_manifest("{}", small); ASSERT_FALSE(limited);
  EXPECT_EQ(limited.error().message_id, "am.manifest.limit_exceeded");
  EXPECT_EQ(limited.error().code, am::Errc::resource_exhausted);
  auto doc = parse_manifest(fixture("schema_invalid_claim_extra.json")); ASSERT_TRUE(doc);
  expect_id(validator().validate(*doc), "am.manifest.schema_violation");
  auto schema = SchemaValidator::create("{}"); ASSERT_TRUE(schema);
  doc = parse_manifest(with_data("1e400")); ASSERT_TRUE(doc);
  EXPECT_TRUE(schema->validate(*doc));
}
TEST(ManifestCodec, DeepOpaqueInputNeverEntersRecursiveDom) {
  RecordProperty("req", "ADR-0038|R-09.8");
  const auto input = with_data(std::string(100000, '[') + "1e400" + std::string(100000, ']'));
  CodecLimits limits; limits.max_depth = 200000;
  auto doc = parse_manifest(input, limits); ASSERT_TRUE(doc);
  EXPECT_TRUE(validator().validate(*doc));
  auto output = write_manifest(*doc); ASSERT_TRUE(output);
  EXPECT_TRUE(verify_preserved(input, *output, {}, limits));
  const auto known = "{\"known\":" + std::string(100000, '[') + '0' + std::string(100000, ']') + '}';
  auto refused = parse_manifest(known, limits);
  expect_id(refused, "am.manifest.limit_exceeded");
  EXPECT_EQ(refused.error().code, am::Errc::resource_exhausted);
}
TEST(ManifestCodec, LargeManyLeafCycleReportsMeasurementsWithoutThreshold) {
  RecordProperty("req", "ADR-0038|R-09.8");
  std::string raw = "{\"padding\":\"" + std::string(16 * 1024 * 1024, 'z') + "\",\"values\":[";
  for (std::size_t i = 0; i < 100000; ++i) { if (i) raw += ','; raw += "1.500"; }
  raw += "]}";
  const auto input = with_data(raw);
  ASSERT_GE(input.size(), 16u * 1024 * 1024);
  const auto start = std::chrono::steady_clock::now();
  auto doc = parse_manifest(input); ASSERT_TRUE(doc);
  EXPECT_TRUE(validator().validate(*doc));
  doc->known()["asset"]["metadata"]["title"] = "Measured title";
  auto output = write_manifest(*doc); ASSERT_TRUE(output);
  EXPECT_TRUE(verify_preserved(input, *output));
  const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("cycle_ms", std::to_string(ms));
  std::cout << "Manifest cycle: bytes=" << input.size() << " opaque_leaves>=100000 time_ms=" << ms;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
  ASSERT_TRUE(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)));
  RecordProperty("peak_working_set", std::to_string(counters.PeakWorkingSetSize));
  RecordProperty("peak_pagefile", std::to_string(counters.PeakPagefileUsage));
  std::cout << " peak_working_set=" << counters.PeakWorkingSetSize << " peak_pagefile=" << counters.PeakPagefileUsage;
#endif
  std::cout << '\n';
}
}  // namespace
