#include "candidates.hpp"
#include "platform_memory.hpp"
#include "reference.hpp"
#include "zip.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::ordered_json;
double elapsed(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
fs::path utf8_path(const char* text) {
  const std::string_view s(text);
  return fs::path(std::u8string(s.begin(), s.end()));
}

Json schema_probes(std::string_view text, std::string_view schema) {
  Json result;
  result["pboettch_native"] = s7::validator_probe(text, schema);
  try {
    auto native = s7::jsoncons_candidate();
    native->parse(text);
    native->validate_native(schema);
    result["jsoncons_native"] = {{"accepted", true}};
  } catch (const std::exception& e) { result["jsoncons_native"] = {{"accepted", false}, {"error", e.what()}}; }
  try {
    const s7::Document reference(text);
    const auto shadow = reference.shadow();
    result["pboettch_shadow"] = s7::validator_probe(shadow, schema);
    try {
      s7::validate_shadow(shadow, schema);
      result["jsoncons_shadow"] = {{"accepted", true}};
    } catch (const std::exception& e) { result["jsoncons_shadow"] = {{"accepted", false}, {"error", e.what()}}; }
  } catch (const std::exception& e) { result["shadow_error"] = e.what(); }
  return result;
}

int main(int argc, char** argv) {
  s7::initialize_process();
  char id = 'A';
  fs::path input, out = "s7-output";
  bool probe = false;
  std::size_t hypothesis_bytes = 128 * 1024 * 1024;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--probe-schema") probe = true;
      else if (i + 1 < argc && arg == "--candidate") id = argv[++i][0];
      else if (i + 1 < argc && arg == "--input") input = utf8_path(argv[++i]);
      else if (i + 1 < argc && arg == "--out") out = utf8_path(argv[++i]);
      else if (i + 1 < argc && arg == "--hypothesis-max-depth") s7::hypothesis_max_depth = std::stoull(argv[++i]);
      else if (i + 1 < argc && arg == "--hypothesis-max-bytes") hypothesis_bytes = std::stoull(argv[++i]);
      else throw std::runtime_error("invalid runner arguments");
    }
    if (input.empty()) throw std::runtime_error("--input is required");
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }

  const auto cycle_start = Clock::now();
  Json report = {{"candidate", std::string(1, id)}, {"status", "unsafe_write"}, {"stage", "setup"},
                 {"parse_accepted", false}, {"v2_accepted", false}, {"committed", false},
                 {"parse_ms", nullptr}, {"v2_ms", nullptr}, {"serialize_ms", nullptr},
                 {"locations", {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
                 {"hypothesis_max_depth", s7::hypothesis_max_depth}, {"hypothesis_max_bytes", hypothesis_bytes}};
  fs::path zip, temporary;
  std::string original_zip;
  bool success = false;
  try {
    fs::create_directories(out);
    const auto schema = s7::read_file(fs::path(S7_ROOT) / "schemas/manifest-1.0.draft.schema.json");
    if (probe) {
      const auto text = input.extension() == ".zip" ? s7::read_zip(input).at("manifest.json") : s7::read_file(input);
      std::cout << schema_probes(text, schema).dump() << '\n';
      return 0;
    }
    zip = input.extension() == ".zip" ? input : out / "input.zip";
    if (input.extension() != ".zip") {
      if (fs::file_size(input) > hypothesis_bytes) throw std::runtime_error("hypothesis_max_bytes exceeded");
      s7::write_zip(zip, {{"manifest.json", s7::read_file(input)}, {"content/s7.bin", "S7 content must remain unchanged\n"}});
    }
    original_zip = s7::read_file(zip);
    auto entries = s7::read_zip(zip);
    const auto& text = entries.at("manifest.json");
    if (text.size() > hypothesis_bytes) throw std::runtime_error("hypothesis_max_bytes exceeded");
    auto mechanism = s7::candidate(id);
    report["stage"] = "parse";
    auto start = Clock::now();
    try { mechanism->parse(text); }
    catch (...) { report["parse_ms"] = elapsed(start); throw; }
    report["parse_ms"] = elapsed(start);
    report["parse_accepted"] = true;
    report["stage"] = "reference_v1";
    const s7::Document before(text);
    report["depth"] = before.max_depth;
    if (before.duplicates) {
      report["stage"] = "duplicate_probe";
      const auto serialized = mechanism->serialize();
      const s7::Document duplicate_result(serialized);
      report["duplicate_policy"] = duplicate_result.duplicates ? "preserved" : "coalesced";
      Json observed = Json::array();
      for (const auto& token : duplicate_result.tokens)
        if (token.edge == "duplicate" || token.edge == "description")
          observed.push_back(token.kind == s7::Kind::string ? token.decoded : std::string(duplicate_result.text.substr(token.begin, token.end - token.begin)));
      report["duplicate_values"] = std::move(observed);
      throw std::runtime_error("duplicate manifest keys: write denied, policy remains O-17");
    }
    report["stage"] = "v2";
    start = Clock::now();
    try {
      if (id == 'E') mechanism->validate_native(schema);
      else s7::validate_shadow(before.shadow(), schema);
    } catch (...) { report["v2_ms"] = elapsed(start); throw; }
    report["v2_ms"] = elapsed(start);
    report["v2_accepted"] = true;
    report["validator"] = id == 'E' ? "jsoncons_native_2020_12" : "jsoncons_same_type_shadow_2020_12";
    report["stage"] = "serialize";
    start = Clock::now();
    auto output = mechanism->serialize();
    report["serialize_ms"] = elapsed(start);
    report["stage"] = "guard";
    const s7::Document after(output);
    if (after.duplicates) throw std::runtime_error("output duplicates");
    const auto old_values = before.fingerprints(), new_values = after.fingerprints();
    const auto old_numbers = before.numeric_lexemes(), new_numbers = after.numeric_lexemes();
    report["opaque_number_counts"] = Json::array();
    bool exact = true;
    for (std::size_t i = 0; i < old_values.size(); ++i) {
      const bool equal = old_values[i] == new_values[i] && old_numbers[i] == new_numbers[i];
      report["opaque_number_counts"].push_back(old_numbers[i].size());
      report["locations"][i] = equal;
      exact = exact && equal;
    }
    if (!exact) throw std::runtime_error("unsafe write: opaque paths, types, values or numeric lexemes changed");
    const auto title = after.child(after.child(after.child(0, "asset"), "metadata"), "title");
    if (title == s7::missing || after.tokens[title].decoded != "S7 edited title") throw std::runtime_error("known title edit missing");
    auto replacement = entries;
    replacement["manifest.json"] = std::move(output);
    temporary = zip;
    temporary += ".s7-tmp";
    report["stage"] = "transaction";
    s7::write_zip(temporary, replacement);
    const auto checked = s7::read_zip(temporary);
    if (checked != replacement) throw std::runtime_error("ZIP reopen differs before replacement");
    const s7::Document checked_manifest(checked.at("manifest.json"));
    if (checked_manifest.fingerprints() != old_values || checked_manifest.numeric_lexemes() != old_numbers)
      throw std::runtime_error("ZIP numeric guard failed");
    s7::atomic_replace(temporary, zip);
    report["committed"] = true;
    report["stage"] = "reopen";
    if (s7::read_zip(zip) != replacement) throw std::runtime_error("final ZIP reopen differs");
    report["status"] = "success";
    success = true;
  } catch (const std::exception& e) { report["error"] = e.what(); }
  if (!temporary.empty()) { std::error_code ignored; fs::remove(temporary, ignored); }
  report["cycle_ms"] = elapsed(cycle_start);
  const auto memory = s7::memory();
  report["peak_working_set"] = memory.peak_working_set;
  report["peak_pagefile"] = memory.peak_pagefile;
  if (!original_zip.empty()) report["original_unchanged"] = s7::read_file(zip) == original_zip;
  std::cout << report.dump() << '\n';
  return success ? 0 : 10;
}
