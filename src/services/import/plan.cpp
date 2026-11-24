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
#include "am/services/import/plan_view.hpp"
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
PlanJson schema_instance(const PlanJson& value) {
  static const auto normative=PlanJson::parse(import_schema);
  auto instance=value;
  for(const auto location:plan_raw_schema_fields) {
    const std::string pointer(location);
    const auto& spec=normative.at(PlanJson::json_pointer(pointer));
    // Fail closed if a future schema adds a constraint this linear check cannot enforce.
    for(auto it=spec.begin();it!=spec.end();++it)
      if(it.key()!="type" && it.key()!="pattern" && it.key()!="minLength" && it.key()!="maxLength" && it.key()!="description" && it.key()!="title" && it.key()!="$comment") invalid(pointer);
    if(spec.at("pattern")!="^([0-9a-f]{2})+$" || spec.at("type")!="string") invalid(pointer);
    const auto begin=std::string("/properties/").size();
    const auto section=pointer.substr(begin,pointer.find('/',begin)-begin);
    const auto field=pointer.substr(pointer.rfind('/')+1);
    if(!instance.is_object() || !instance.contains(section) || !instance.at(section).is_array()) continue;
    for(std::size_t i=0;i<instance.at(section).size();++i) {
      auto& item=instance.at(section).at(i);
      if(!item.is_object() || !item.contains(field) || !item.at(field).is_string()) continue;
      const auto& raw=item.at(field).get_ref<const std::string&>();
      const auto at="/"+section+"/"+std::to_string(i)+"/"+field;
      if(raw.empty() || raw.size()%2 || raw.find_first_not_of("0123456789abcdef")!=std::string::npos) invalid(at);
      if(spec.contains("minLength") && raw.size()<spec.at("minLength").get<std::size_t>()) invalid(at);
      if(spec.contains("maxLength") && raw.size()>spec.at("maxLength").get<std::size_t>()) invalid(at);
      // Only the schema's copy is compacted. References and the typed view use original bytes.
      item.at(field)="00";
    }
  }
  return instance;
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
    schema.validate(jsoncons::json::parse(schema_instance(value).dump()), [&](const jsoncons::jsonschema::validation_message& message) {
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
      if (n.contains("mtime_ns")) {
        const auto v = n["mtime_ns"].get<std::string>();
        std::int64_t number = 0;
        const auto r = std::from_chars(v.data(),v.data()+v.size(),number);
        if (r.ec != std::errc{} || r.ptr != v.data()+v.size()) invalid(at + "/mtime_ns");
      }
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

namespace am::import {
namespace {
std::optional<RawBytes> raw_bytes(const PlanJson& j, const char* field) {
  if (!j.contains(field)) return {};
  const auto encoded = j.at(field).get<std::string>();
  RawBytes out{{},j.value("raw_encoding",std::string{})};
  for (std::size_t i=0;i<encoded.size();i+=2) {
    unsigned byte=0;
    std::from_chars(encoded.data()+i,encoded.data()+i+2,byte,16);
    out.bytes += static_cast<char>(byte);
  }
  return out;
}
template<class T> std::optional<T> integer(const PlanJson& j,const char* key) {
  if (!j.contains(key)) return {};
  const auto s=j.at(key).get<std::string>(); T n{};
  std::from_chars(s.data(),s.data()+s.size(),n); return n;
}
std::filesystem::path native_text(const std::string& display, const std::optional<RawBytes>& raw) {
  if (!raw) {
    if(display.find('\0')!=std::string::npos) throw detail::error("am.import.plan_invalid");
    return std::filesystem::path(std::u8string(display.begin(),display.end()));
  }
#ifdef _WIN32
  if (raw->encoding != "utf-16le" || raw->bytes.size()%2) throw detail::error("am.import.run.source_unavailable");
  std::wstring text;
  for (std::size_t i=0;i<raw->bytes.size();i+=2)
    text+=static_cast<wchar_t>(static_cast<unsigned char>(raw->bytes[i]) |
                             (static_cast<unsigned char>(raw->bytes[i+1])<<8));
  if(text.find(L'\0')!=std::wstring::npos) throw detail::error("am.import.plan_invalid");
  return std::filesystem::path(text);
#else
  if (raw->encoding != "posix") throw detail::error("am.import.run.source_unavailable");
  if(raw->bytes.find('\0')!=std::string::npos) throw detail::error("am.import.plan_invalid");
  return std::filesystem::path(raw->bytes);
#endif
}
}
Result<std::filesystem::path> source_path(const PlanView& plan, const PlanNode& node) {
  try {
    const auto& root=plan.roots().at(node.root);
    auto path=native_text(root.path,root.raw);
    if (!node.path.empty() || node.path_raw) {
      const auto relative=native_text(node.path,node.path_raw);
      if(relative.is_absolute() || relative.has_root_name()) throw detail::error("am.import.plan_invalid");
      for(const auto& component:relative) if(component=="..") throw detail::error("am.import.plan_invalid");
      path/=relative;
    }
    return path;
  } catch(const Error& e) { return e; }
  catch(...) { return detail::error("am.import.run.source_unavailable"); }
}
Result<PlanView> parse_plan_view(std::string_view input) {
  try {
    auto parsed=parse_plan(input); if(!parsed) return parsed.error();
    const auto& j=parsed->value;
    PlanView out;
    out.mode_=j.at("mode")=="partial" ? ImportMode::partial : ImportMode::strict;
    out.scan_complete_=j.at("scan").at("completeness")=="complete";
    for(const auto& r:j.at("roots")) out.roots_.push_back({r.at("path").get<std::string>(),raw_bytes(r,"path_raw")});
    std::map<std::string,std::size_t> indices;
    for(const auto& n:j.at("nodes")) {
      PlanNode v;
      v.id=n.at("id"); v.path=n.at("path"); v.name=n.at("name"); v.root=n.at("root");
      const std::map<std::string,NodeKind> kinds{{"file",NodeKind::file},{"directory",NodeKind::directory},{"symlink",NodeKind::symlink},{"other",NodeKind::other}};
      v.kind=kinds.at(n.at("kind").get<std::string>()); v.available=n.at("status")=="available";
      v.empty=n.value("empty",false); v.excluded_only=n.value("excluded_only",false);
      v.detected=n.value("detected",std::string{}); v.size=integer<std::uint64_t>(n,"size"); v.mtime_ns=integer<std::int64_t>(n,"mtime_ns");
      if(n.contains("sha256")) v.sha256=n.at("sha256").get<std::string>();
      v.name_raw=raw_bytes(n,"name_raw"); v.path_raw=raw_bytes(n,"path_raw");
      if(n.contains("problem")) v.problem=PlanProblem{n.at("problem").at("code"),n.at("problem").at("message_id")};
      indices.emplace(v.id,out.nodes_.size()); out.nodes_.push_back(std::move(v));
    }
    for(const auto& c:j.at("candidates")) {
      PlanCandidate v;
      v.id=c.at("id"); v.root=c.at("root"); v.base_path=c.at("base_path"); v.title_hint=c.at("title_hint"); v.confidence=c.at("confidence");
      v.kind=c.at("kind")=="ready_asset" ? CandidateKind::ready_asset : CandidateKind::new_asset;
      const std::map<std::string,CandidateAction> actions{{"import",CandidateAction::import},{"skip",CandidateAction::skip},{"ask",CandidateAction::ask}};
      v.action=actions.at(c.at("action").get<std::string>());
      v.reasons=c.at("reasons").get<std::vector<std::string>>(); v.warnings=c.at("warnings").get<std::vector<std::string>>();
      if(c.contains("asset_id")) v.asset_id=c.at("asset_id").get<std::string>();
      if(c.contains("revision")) v.revision=c.at("revision").get<std::uint64_t>();
      if(c.at("context").contains("sources")) for(const auto& s:c.at("context").at("sources")) v.sources.push_back({s.at("kind"),s.at("name")});
      std::map<std::string,std::size_t> volumes;
      for(const auto& s:c.at("volume_sets")) {
        PlanVolumeSet set; set.id=s.at("id"); set.scheme=s.at("scheme"); set.base=s.at("base"); set.confirmed=s.at("confirmed");
        set.missing_indices=s.at("missing_indices").get<std::vector<std::uint64_t>>();
        for(const auto& m:s.at("members")) set.members.push_back(indices.at(m.get<std::string>()));
        volumes.emplace(set.id,v.volume_sets.size()); v.volume_sets.push_back(std::move(set));
      }
      const std::map<std::string,InputKind> kinds{{"file",InputKind::file},{"directory",InputKind::directory},{"archive",InputKind::archive},{"multivolume_archive",InputKind::multivolume_archive}};
      for(const auto& in:c.at("inputs")) {
        PlanInput item; item.id=in.at("id"); item.kind=kinds.at(in.at("kind").get<std::string>());
        for(const auto& ref:in.at("objects")) {
          PlanObjectRef r;
          if(ref.contains("node")) r.node=indices.at(ref.at("node").get<std::string>());
          else r.volume_set=volumes.at(ref.at("volume_set").get<std::string>());
          item.objects.push_back(r);
        }
        v.inputs.push_back(std::move(item));
      }
      const std::map<std::string,EntryDecision> decisions{{"include",EntryDecision::include},{"exclude",EntryDecision::exclude},{"ask",EntryDecision::ask},{"skip",EntryDecision::skip}};
      for(const auto& e:c.at("entries")) v.entries.push_back({indices.at(e.at("node").get<std::string>()),decisions.at(e.at("decision").get<std::string>()),e.value("rule_id",std::string{}),e.value("confidence",std::string{}),e.value("explanation_id",std::string{})});
      for(const auto& d:c.at("directories")) v.directories.push_back(indices.at(d.get<std::string>()));
      for(const auto& l:c.at("suggested_links")) v.suggested_links.push_back({l.at("url"),indices.at(l.at("node").get<std::string>()),l.at("ad")});
      for(const auto& m:c.at("matches")) {
        PlanMatch match; match.kind=m.at("kind"); match.asset_id=m.at("asset_id"); match.object_id=m.value("object_id",std::string{}); match.relation=m.value("relation",std::string{});
        if(m.contains("node")) match.node=indices.at(m.at("node").get<std::string>());
        v.matches.push_back(std::move(match));
      }
      out.candidates_.push_back(std::move(v));
    }
    return out;
  } catch(const Error& e) { return e; }
  catch(const std::exception& e) { return detail::error("am.import.plan_invalid",e.what()); }
  catch(...) { return detail::error("am.import.plan_invalid"); }
}
} // namespace am::import
