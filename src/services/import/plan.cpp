#include <algorithm>
#include <charconv>
#include <cmath>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <map>
#include <regex>
#include <set>

#include "am/domain/manifest/codec.hpp"
#include "am/foundation/json_text.hpp"
#include "am/services/import/exclusion_rules.hpp"
#include "am/services/import/scan.hpp"
#include "detail.hpp"
#include "import_schema.hpp"

namespace am::import {
namespace {
using namespace detail;
[[noreturn]] void invalid(const std::string& at) { throw error("am.import.plan_invalid", at); }
std::string text(const PlanJson& j, const char* key) { return j.at(key).get<std::string>(); }
void decimal(const PlanJson& value, const std::string& at) {
  auto v = value.get<std::string>();
  std::uint64_t number = 0;
  const auto result = std::from_chars(v.data(), v.data() + v.size(), number);
  if (result.ec != std::errc{} || result.ptr != v.data() + v.size()) invalid(at);
}
}  // namespace
Result<void> validate_profile(const ImportProfile& profile) {
  try {
    auto bad = [](const std::string& path) { throw error("am.import.profile_invalid", path); };
    const auto& p = profile.settings;
    const std::map<std::string, std::map<std::string, char>> sections{
        {"boundaries", {{"candidate", 's'}, {"context_levels", 'n'}}},
        {"provenance", {{"parent_as", 's'}, {"url_files", 's'}, {"url_ad_patterns", 'a'}}},
        {"nested", {{"action", 's'}, {"max_depth", 'p'}}},
        {"exclusions", {{"rules", 'a'}}},
        {"previews", {{"primary", 'a'}}},
        {"groups", {{"hints", 'a'}}},
        {"mode", {{"strict", 'b'}}},
        {"volumes",
         {{"rar_part", 'b'},
          {"rar_old", 'b'},
          {"split_numeric", 'b'},
          {"zip_split", 'b'},
          {"copy_suffix", 'b'},
          {"copy_suffix_extensions", 'a'}}},
        {"limits",
         {{"max_entries", 'p'},
          {"max_unpacked_bytes", 'p'},
          {"max_ratio", 'r'},
          {"max_seconds_per_container", 'p'},
          {"max_temp_bytes", 'p'}}}};
    if (!p.is_object()) bad("");
    for (auto it = p.begin(); it != p.end(); ++it) {
      const auto path = "/" + it.key();
      if (it.key() == "id") {
        if (!it->is_string() || it->get_ref<const std::string&>().empty()) bad(path);
        continue;
      }
      if (it.key() == "version") {
        if (!it->is_number_integer() || *it != 1) bad(path);
        continue;
      }
      const auto section = sections.find(it.key());
      if (section == sections.end() || !it->is_object()) bad(path);
      for (auto value = it->begin(); value != it->end(); ++value) {
        const auto key = section->second.find(value.key());
        const auto at = path + "/" + value.key();
        if (key == section->second.end()) bad(at);
        switch (key->second) {
          case 's':
            if (!value->is_string()) bad(at);
            break;
          case 'b':
            if (!value->is_boolean()) bad(at);
            break;
          case 'n':
          case 'p':
            if (!value->is_number_integer() || *value < 0 || (key->second == 'p' && *value == 0)) bad(at);
            break;
          case 'r':
            if (!value->is_number() || !std::isfinite(value->get<double>()) || *value <= 0) bad(at);
            break;
          case 'a':
            if (!value->is_array()) bad(at);
            for (const auto& item : *value)
              if (!item.is_string()) bad(at);
            break;
        }
      }
    }
    if (!p.contains("id")) bad("/id");
    if (!p.contains("version")) bad("/version");
    if (p.contains("boundaries")) {
      const auto strategy = p["boundaries"].value("candidate", std::string("root"));
      if (strategy != "root" && strategy != "leaf_dir_with_payload") bad("/boundaries/candidate");
    }
    if (p.contains("provenance")) {
      const auto urls = p["provenance"].value("url_files", std::string("suggest"));
      if (urls != "suggest" && urls != "ignore") bad("/provenance/url_files");
      if (p["provenance"].contains("url_ad_patterns"))
        for (const auto& item : p["provenance"]["url_ad_patterns"]) {
          auto pattern = item.get<std::string>();
          auto flags = std::regex::ECMAScript;
          if (pattern.starts_with("(?i)")) {
            pattern.erase(0, 4);
            flags |= std::regex::icase;
          }
          try {
            (void)std::regex(pattern, flags);
          } catch (const std::regex_error&) {
            bad("/provenance/url_ad_patterns");
          }
        }
    }
    if (p.contains("exclusions") && p["exclusions"].contains("rules")) {
      const auto known = ExclusionEngine{}.rule_ids();
      for (const auto& id : p["exclusions"]["rules"])
        if (std::find(known.begin(), known.end(), id.get<std::string>()) == known.end()) bad("/exclusions/rules");
    }
    if (p.contains("nested") && p["nested"].contains("action")) {
      const auto action = p["nested"]["action"].get<std::string>();
      if (action != "expand" && action != "extract" && action != "preserve" && action != "exclude")
        bad("/nested/action");
    }
    if (profile.sha256.size() != 64 || profile.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
      bad("/sha256");
    return {};
  } catch (const Error& e) {
    return e;
  } catch (const std::exception&) {
    return error("am.import.profile_invalid");
  } catch (...) {
    return error("am.import.profile_invalid");
  }
}
std::string serialize_plan(const ImportPlan& plan) noexcept {
  try {
    return plan.value.dump(2) + '\n';
  } catch (...) {
    return {};
  }
}
Result<ImportPlan> parse_plan(std::string_view input) {
  try {
    const manifest::CodecLimits limits;
    auto checked = am::json::parse_strict(input, {limits.max_known_depth, limits.max_bytes});
    if (!checked) return error("am.import.plan_invalid").caused_by(checked.error());
    auto value = PlanJson::parse(input);
    static const auto schema = jsoncons::jsonschema::make_json_schema(jsoncons::json::parse(import_schema));
    std::optional<std::string> problem;
    schema.validate(jsoncons::json::parse(input), [&](const jsoncons::jsonschema::validation_message& message) {
      problem = message.instance_location().string();
      return jsoncons::jsonschema::walk_state::abort;
    });
    if (problem) invalid(*problem);
    std::map<std::string, std::size_t> nodes;
    for (std::size_t i = 0; i < value["nodes"].size(); ++i) {
      const auto& n = value["nodes"][i];
      const auto at = "/nodes/" + std::to_string(i);
      if (!nodes.emplace(text(n, "id"), i).second) invalid(at + "/id");
      if (n["root"].get<std::size_t>() >= value["roots"].size()) invalid(at + "/root");
      if (n.contains("size")) decimal(n["size"], at + "/size");
      if (n["kind"] == "file" && n["status"] == "available" && (!n.contains("size") || !n.contains("mtime_ns")))
        invalid(at);
      if (n["status"] == "unavailable" && !n.contains("problem")) invalid(at + "/problem");
    }
    auto node = [&](const std::string& id, const std::string& at) -> const PlanJson& {
      auto it = nodes.find(id);
      if (it == nodes.end()) invalid(at);
      return value["nodes"][it->second];
    };
    std::map<std::string, std::size_t> owners;
    std::set<std::string> candidates, inputs, volume_ids;
    for (std::size_t ci = 0; ci < value["candidates"].size(); ++ci) {
      const auto& c = value["candidates"][ci];
      const auto at = "/candidates/" + std::to_string(ci);
      if (!candidates.insert(text(c, "id")).second) invalid(at + "/id");
      if (c["root"].get<std::size_t>() >= value["roots"].size()) invalid(at + "/root");
      auto claim = [&](const std::string& id, const std::string& pointer) {
        const auto& n = node(id, pointer);
        if (n["kind"] == "file") {
          const auto [it, added] = owners.emplace(id, ci);
          if (!added && it->second != ci) invalid(pointer);
        }
      };
      std::map<std::string, std::vector<std::string>> sets;
      for (std::size_t vi = 0; vi < c["volume_sets"].size(); ++vi) {
        const auto& v = c["volume_sets"][vi];
        const auto vat = at + "/volume_sets/" + std::to_string(vi);
        if (!volume_ids.insert(text(v, "id")).second) invalid(vat + "/id");
        auto& members = sets[text(v, "id")];
        std::set<std::string> unique;
        for (std::size_t mi = 0; mi < v["members"].size(); ++mi) {
          const auto id = v["members"][mi].get<std::string>();
          const auto mat = vat + "/members/" + std::to_string(mi);
          if (!unique.insert(id).second || node(id, mat)["kind"] != "file") invalid(mat);
          claim(id, mat);
          members.push_back(id);
        }
        if (members.empty()) invalid(vat + "/members");
      }
      std::set<std::string> entries;
      for (std::size_t ei = 0; ei < c["entries"].size(); ++ei) {
        const auto& e = c["entries"][ei];
        const auto eat = at + "/entries/" + std::to_string(ei) + "/node";
        const auto id = text(e, "node");
        if (!entries.insert(id).second) invalid(eat);
        claim(id, eat);
        if (node(id, eat)["kind"] == "directory") invalid(eat);
        const auto& source = node(id, eat);
        if ((source["kind"] == "symlink" || (source["kind"] == "other" && source["status"] == "available")) &&
            e["decision"] != "skip")
          invalid(eat);
      }
      std::vector<std::string> object_nodes;
      for (std::size_t di = 0; di < c["directories"].size(); ++di) {
        const auto dat = at + "/directories/" + std::to_string(di);
        if (node(c["directories"][di].get<std::string>(), dat)["kind"] != "directory") invalid(dat);
      }
      for (const auto* field : {"suggested_links", "filtered_links"})
        if (c.contains(field))
          for (std::size_t li = 0; li < c[field].size(); ++li)
            node(text(c[field][li], "node"), at + "/" + field + "/" + std::to_string(li) + "/node");
      for (std::size_t ii = 0; ii < c["inputs"].size(); ++ii) {
        const auto& in = c["inputs"][ii];
        const auto iat = at + "/inputs/" + std::to_string(ii);
        if (!inputs.insert(text(in, "id")).second) invalid(iat + "/id");
        for (std::size_t oi = 0; oi < in["objects"].size(); ++oi) {
          const auto& ref = in["objects"][oi];
          const auto oat = iat + "/objects/" + std::to_string(oi);
          if (ref.contains("node")) {
            auto id = text(ref, "node");
            claim(id, oat + "/node");
            object_nodes.push_back(id);
          } else {
            const auto it = sets.find(text(ref, "volume_set"));
            if (it == sets.end()) invalid(oat + "/volume_set");
            object_nodes.insert(object_nodes.end(), it->second.begin(), it->second.end());
          }
        }
      }
      if (c["kind"] == "ready_asset") {
        if (c["inputs"].size() != 1 || c["inputs"][0]["objects"].size() != 1 ||
            !c["inputs"][0]["objects"][0].contains("node") || object_nodes.size() != 1)
          invalid(at + "/inputs");
        const auto& source = node(object_nodes[0], at + "/inputs");
        if (source["kind"] != "file" || source.value("detected", std::string{}) != "archive.zip")
          invalid(at + "/inputs");
      }
      for (std::size_t mi = 0; mi < c["matches"].size(); ++mi)
        if (c["matches"][mi].contains("node"))
          node(text(c["matches"][mi], "node"), at + "/matches/" + std::to_string(mi) + "/node");
      if (c["action"] == "skip") {
        if (object_nodes.empty()) invalid(at + "/action");
        std::set<std::string> exact;
        for (const auto& match : c["matches"])
          if (match["kind"] == "sha256_input" && match.contains("node")) exact.insert(text(match, "node"));
        for (const auto& id : object_nodes) {
          if (!exact.contains(id)) invalid(at + "/action");
        }
      }
    }
    return ImportPlan{std::move(value)};
  } catch (const Error& e) {
    return e;
  } catch (const std::bad_alloc&) {
    return error("am.import.out_of_memory", {}, Errc::resource_exhausted);
  } catch (const std::exception&) {
    return error("am.import.plan_invalid");
  } catch (...) {
    return error("am.import.plan_invalid");
  }
}
}  // namespace am::import
