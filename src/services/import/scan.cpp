#include "am/services/import/scan.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include "am/foundation/log.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/signature.hpp"
#include "am/foundation/unicode.hpp"
#include "am/foundation/version.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/services/import/exclusion_rules.hpp"
#include "am/services/import/zip_support.hpp"
#include "am/storage/archive/volume_detect.hpp"
#include "am/storage/asset_zip/probe.hpp"
#include "detail.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

namespace am::import {
namespace {
using namespace detail;
namespace fs = std::filesystem;
using J = PlanJson;
struct Name {
  std::string display, raw;
};
Name name(const fs::path& path) {
  Name result;
#ifdef _WIN32
  const auto& units = path.native();
  bool repaired = false;
  for (std::size_t i = 0; i < units.size(); ++i) {
    char32_t point = static_cast<unsigned short>(units[i]);
    if (point >= 0xd800 && point <= 0xdbff && i + 1 < units.size() && units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff)
      point = 0x10000 + ((point - 0xd800) << 10) + (static_cast<unsigned short>(units[++i]) - 0xdc00);
    else if (point >= 0xd800 && point <= 0xdfff) {
      point = 0xfffd;
      repaired = true;
    }
    if (point < 0x80)
      result.display += static_cast<char>(point);
    else if (point < 0x800) {
      result.display += static_cast<char>(0xc0 | (point >> 6));
      result.display += static_cast<char>(0x80 | (point & 63));
    } else if (point < 0x10000) {
      result.display += static_cast<char>(0xe0 | (point >> 12));
      result.display += static_cast<char>(0x80 | ((point >> 6) & 63));
      result.display += static_cast<char>(0x80 | (point & 63));
    } else {
      result.display += static_cast<char>(0xf0 | (point >> 18));
      result.display += static_cast<char>(0x80 | ((point >> 12) & 63));
      result.display += static_cast<char>(0x80 | ((point >> 6) & 63));
      result.display += static_cast<char>(0x80 | (point & 63));
    }
  }
  if (repaired) {
    std::vector<std::byte> bytes;
    for (auto unit : units) {
      const auto u = static_cast<unsigned short>(unit);
      bytes.push_back(static_cast<std::byte>(u & 255));
      bytes.push_back(static_cast<std::byte>(u >> 8));
    }
    result.raw = hex(bytes);
  }
#else
  const auto& bytes = path.native();
  bool repaired = false;
  for (std::size_t i = 0; i < bytes.size();) {
    const auto c = static_cast<unsigned char>(bytes[i]);
    std::size_t count = c < 128                  ? 1
                        : c >= 0xc2 && c <= 0xdf ? 2
                        : c >= 0xe0 && c <= 0xef ? 3
                        : c >= 0xf0 && c <= 0xf4 ? 4
                                                 : 0;
    bool valid = count && i + count <= bytes.size();
    for (std::size_t k = 1; valid && k < count; ++k) valid = (static_cast<unsigned char>(bytes[i + k]) & 0xc0) == 0x80;
    if (valid && count >= 3) {
      const auto second = static_cast<unsigned char>(bytes[i + 1]);
      valid = !(c == 0xe0 && second < 0xa0) && !(c == 0xed && second >= 0xa0) && !(c == 0xf0 && second < 0x90) &&
              !(c == 0xf4 && second >= 0x90);
    }
    if (valid) {
      result.display.append(bytes, i, count);
      i += count;
    } else {
      result.display += "\xef\xbf\xbd";
      ++i;
      repaired = true;
    }
  }
  if (repaired) result.raw = hex(std::as_bytes(std::span(bytes.data(), bytes.size())));
#endif
  return result;
}
void raw(J& value, const char* key, const Name& n) {
  if (n.raw.empty()) return;
  value[key] = n.raw;
#ifdef _WIN32
  value["raw_encoding"] = "utf-16le";
#else
  value["raw_encoding"] = "posix";
#endif
}
std::string folded(std::string_view text) {
  auto result = unicode::simple_casefold(text);
  return result ? *result : std::string(text);
}
fs::path normalized(const fs::path& path) {
  const auto absolute = paths::native(fs::absolute(path));
  const auto info = paths::node_info(absolute);
  if (info && info->cloud_placeholder) return fs::weakly_canonical(absolute.parent_path()) / absolute.filename();
  return fs::weakly_canonical(absolute);
}
bool within(const fs::path& child, const fs::path& parent) {
  const auto a_path = normalized(child);
  const auto b_path = normalized(parent);
  auto a = a_path.begin(), b = b_path.begin();
  for (; b != b_path.end(); ++a, ++b) {
    if (a == a_path.end()) return false;
#ifdef _WIN32
    if (CompareStringOrdinal(a->c_str(), static_cast<int>(a->native().size()), b->c_str(),
                             static_cast<int>(b->native().size()), TRUE) != CSTR_EQUAL)
      return false;
#else
    if (*a != *b) return false;
#endif
  }
  return true;
}
std::string numbered(char prefix, std::size_t index, unsigned width) {
  std::ostringstream out;
  out << prefix << std::setw(static_cast<int>(width)) << std::setfill('0') << index;
  return out.str();
}
std::string time_text(std::chrono::system_clock::time_point now) {
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char text[32]{};
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &tm);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::ostringstream out;
  out << text << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
  return out.str();
}
struct Node {
  J value;
  fs::path actual, relative;
  FileKind kind = FileKind::unknown;
  RuleMatch rule;
  bool ready = false;
  J asset;
  std::vector<std::string> urls;
};
struct Unit {
  J value;
  std::vector<std::size_t> members;
};
struct Candidate {
  J value;
  std::vector<std::size_t> members;
  std::size_t order = 0;
};
struct Scanner {
  const ScanRequest& request;
  J settings;
  std::vector<Node> nodes;
  J roots = J::array();
  std::vector<Unit> sets;
  std::map<std::size_t, std::size_t> node_set;
  std::uint64_t max_entries = 100000;  // Hypothesis inherited from the existing profile.
  bool incomplete = false, limited = false;
  ScanProgress progress;
  ExclusionEngine exclusions;
  std::vector<std::string> rules;
  std::shared_ptr<archive::ZipInputJob> zip_job;
  explicit Scanner(const ScanRequest& r) : request(r), settings(r.profile.settings) {
    rules = settings.value("exclusions", J::object()).value("rules", exclusions.rule_ids());
    max_entries = settings.value("limits", J::object()).value("max_entries", max_entries);
    zip_job=scan_archive_job(r);
  }
  void cancel() const {
    if (request.cancelled && request.cancelled()) throw error("am.import.cancelled", {}, Errc::cancelled);
  }
  void io(const fs::path& path, ScanIo kind) const {
    cancel();
    if (request.hooks.before_io) take(request.hooks.before_io(path, kind));
  }
  void opening(const fs::path& path) const {
    io(path, ScanIo::open);
    if (request.hooks.on_open) request.hooks.on_open(path, false);
  }
  paths::NodeInfo node_info(const fs::path& path) const {
    return request.hooks.node_data ? paths::classify_windows_node(take(request.hooks.node_data(path)))
                                   : take(paths::node_info(path));
  }
  void unavailable(Node& n, const Error& cause, std::string_view code = {}) {
    incomplete = true;
    n.value["status"] = "unavailable";
    n.value["problem"] = {{"code", std::string(code.empty() ? errc_name(cause.code) : code)},
                          {"message_id", cause.message_id.empty() ? "am.import.node_unavailable" : cause.message_id}};
    AM_LOG(warning, "import", "import.scan.node_unavailable", "Source node unavailable",
           {{"path", name(n.actual).display, log::FieldKind::path}}, &cause);
  }
  void file(Node& n) {
    opening(n.actual);
    std::ifstream input(paths::native(n.actual), std::ios::binary);
    if (!input) throw error("am.import.node_unavailable", {}, Errc::io_error);
    io(n.actual, ScanIo::header);
    std::array<std::uint8_t, kSignatureProbeBytes> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    const auto read = static_cast<std::size_t>(input.gcount());
    const auto size = std::stoull(n.value["size"].get<std::string>());
    if (input.bad() || read < std::min<std::uint64_t>(header.size(), size))
      throw error("am.import.node_unavailable", {}, Errc::io_error);
    n.kind = detect_kind(std::span(header.data(), read), size);
    n.value["detected"] = to_string(n.kind);
    n.rule = exclusions.evaluate({n.value["path"].get_ref<const std::string&>(), size, n.kind}, rules);
    if (request.hash_inputs) {
      io(n.actual, ScanIo::hash);
      input.clear();
      input.seekg(0);
      auto hash_state = take(asset_zip::Sha256::create());
      std::array<char, 65536> buffer{};
      std::uint64_t total = 0;
      while (input) {
        cancel();
        input.read(buffer.data(), buffer.size());
        auto count = input.gcount();
        if (count > 0) {
          total += static_cast<std::uint64_t>(count);
          take(hash_state.update(std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(count)))));
        }
      }
      if (input.bad() || total != size) throw error("am.import.node_unavailable", {}, Errc::io_error);
      n.value["sha256"] = hex(take(hash_state.finish()));
    }
    if (n.kind == FileKind::zip) {
      io(n.actual, ScanIo::zip_probe);
      if (request.hooks.on_open) request.hooks.on_open(n.actual, false);
      auto source = take(asset_zip::file_source(n.actual, true));
      const auto probe = take(archive::ZipInput::probe(source,n.actual,zip_job));
      n.ready = probe.outcome == asset_zip::ProbeOutcome::own ||
                probe.outcome == asset_zip::ProbeOutcome::own_corrupt ||
                probe.outcome == asset_zip::ProbeOutcome::own_unsupported_version;
      if (n.ready) {
        n.asset = {{"valid", false}};
        if(probe.error) n.asset["probe_error"]=probe.error->message_id;
        if(probe.error && probe.error->message_id=="am.zip.duplicate_name") n.asset["manifest_detected"]=true;
        repository::ContainerContents contents;
        auto verified = take(repository::verify_container(n.actual, {}, &contents));
        n.asset["valid"] = !verified.error.has_value() && probe.outcome == asset_zip::ProbeOutcome::own;
        if (contents.document) {
          const auto& manifest = contents.document->known();
          if (manifest.contains("asset") && manifest["asset"].is_object()) {
            const auto& asset = manifest["asset"];
            if (asset.contains("id") && asset["id"].is_string()) n.asset["asset_id"] = asset["id"].get<std::string>();
            if (asset.contains("revision") && asset["revision"].is_number_unsigned())
              n.asset["revision"] = asset["revision"].get<std::uint64_t>();
          }
        }
      } else if (probe.outcome == asset_zip::ProbeOutcome::foreign_zip) {
        auto input_archive = archive::ZipInput::open(n.actual,zip_job);
        if (input_archive)
          for (const auto& entry : input_archive->entries())
            if (entry.info.raw_name == "3D/3dmodel.model") n.value["detected"] = "project.3mf";
      }
    }
    if (folded(name(n.actual.extension()).display) == ".url" &&
        settings.value("provenance", J::object()).value("url_files", std::string("suggest")) == "suggest") {
      io(n.actual, ScanIo::shortcut);
      input.clear();
      input.seekg(0);
      std::string line;
      bool section = false;
      while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with('[')) {
          section = folded(line) == "[internetshortcut]";
          continue;
        }
        if (section && line.size() >= 4 && folded(line.substr(0, 4)) == "url=")
          n.urls.push_back(name(fs::path(std::u8string(line.begin() + 4, line.end()))).display);
      }
      if (input.bad()) throw error("am.import.node_unavailable", {}, Errc::io_error);
    }
  }
  void walk() {
    struct Pending {
      fs::path actual, relative;
      std::size_t root;
      bool top;
    };
    std::vector<fs::path> resolved;
    for (std::size_t ri = 0; ri < request.roots.size(); ++ri) {
      cancel();
      fs::path actual;
      try {
        io(request.roots[ri], ScanIo::stat);
        const auto info = node_info(request.roots[ri]);
        actual =
            info.cloud_placeholder ? normalized(request.roots[ri]) : fs::canonical(paths::native(request.roots[ri]));
      } catch (const Error& e) {
        throw error("am.import.root_unavailable", {}, e.code).caused_by(e);
      } catch (const std::exception&) {
        throw error("am.import.root_unavailable", {}, Errc::io_error);
      }
      const auto root_name = name(actual);
      J root = {{"path", root_name.display}};
      raw(root, "path_raw", root_name);
      roots.push_back(root);
      resolved.push_back(std::move(actual));
    }
    for (std::size_t ri = 0; ri < resolved.size(); ++ri) {
      std::vector<Pending> pending{{resolved[ri], {}, ri, true}};
      while (!pending.empty()) {
        cancel();
        if (nodes.size() >= max_entries) {
          limited = incomplete = true;
          AM_LOG(warning, "import", "import.scan.limit_reached", "Import scan limit reached",
                 {{"limit", std::string("max_entries")}});
          break;
        }
        auto p = std::move(pending.back());
        pending.pop_back();
        Node n;
        n.actual = p.actual;
        n.relative = p.relative;
        const auto display = name(p.relative);
        auto relative = display.display;
#ifdef _WIN32
        std::replace(relative.begin(), relative.end(), '\\', '/');
#endif
        const auto original = name(p.actual.filename());
        n.value = {{"root", p.root},
                   {"path", relative},
                   {"name", original.display},
                   {"kind", "other"},
                   {"status", "available"}};
        raw(n.value, "name_raw", original);
        raw(n.value, "path_raw", display);
        try {
          if (!p.top) io(p.actual, ScanIo::stat);
          const auto info = node_info(p.actual);
          const bool link = info.kind == paths::NodeKind::link;
          const auto kind = link                                      ? "symlink"
                            : info.kind == paths::NodeKind::directory ? "directory"
                            : info.kind == paths::NodeKind::file      ? "file"
                                                                      : "other";
          n.value["kind"] = kind;
          if (!link) {
            if (!info.mtime_ns) throw error("am.import.node_unavailable", {}, Errc::io_error);
            n.value["mtime_ns"] = std::to_string(*info.mtime_ns);
            if (info.kind == paths::NodeKind::file) {
              if (!info.size) throw error("am.import.node_unavailable", {}, Errc::io_error);
              n.value["size"] = std::to_string(*info.size);
              if (info.cloud_placeholder)
                unavailable(n, error("am.import.cloud_placeholder", {}, Errc::incomplete), "cloud_placeholder");
              else
                file(n);
              ++progress.files;
            } else if (info.kind == paths::NodeKind::directory) {
              io(p.actual, ScanIo::enumerate);
              std::vector<fs::path> children;
              for (const auto& entry : fs::directory_iterator(paths::native(p.actual)))
                children.push_back(entry.path().filename());
              n.value["empty"] = children.empty();
              std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
                const auto x = name(a), y = name(b);
                return x.display == y.display ? a.native() < b.native() : x.display < y.display;
              });
              for (auto it = children.rbegin(); it != children.rend(); ++it)
                pending.push_back({p.actual / *it, p.relative / *it, p.root, false});
            }
          }
        } catch (const Error& e) {
          if (e.code == Errc::cancelled) throw;
          if (p.top) throw error("am.import.root_unavailable", {}, e.code).caused_by(e);
          unavailable(n, e);
        } catch (const std::exception&) {
          auto e = error("am.import.node_unavailable", {}, Errc::io_error);
          if (p.top) throw error("am.import.root_unavailable", {}, Errc::io_error);
          unavailable(n, e);
        }
        nodes.push_back(std::move(n));
        ++progress.nodes;
        if (request.on_progress) request.on_progress(progress);
      }
      if (limited) break;
    }
    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) {
      if (a.value["root"] != b.value["root"]) return a.value["root"] < b.value["root"];
      const auto x = a.value["path"].template get<std::string>(), y = b.value["path"].template get<std::string>();
      return x == y ? a.relative.native() < b.relative.native() : x < y;
    });
    for (std::size_t i = 0; i < nodes.size(); ++i) nodes[i].value["id"] = numbered('n', i + 1, 6);
  }
  void volumes() {
    archive::VolumeNamingRules config;
    const auto v = settings.value("volumes", J::object());
    config.rar_part = v.value("rar_part", true);
    config.rar_old = v.value("rar_old", true);
    config.split_numeric = v.value("split_numeric", true);
    config.zip_split = v.value("zip_split", true);
    config.copy_suffix = v.value("copy_suffix", true);
    config.copy_suffix_extensions = v.value("copy_suffix_extensions", config.copy_suffix_extensions);
    std::map<fs::path, std::vector<std::size_t>> directories;
    for (std::size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].value["kind"] == "file" && !nodes[i].ready &&
          nodes[i].value.value("detected", std::string{}) != "project.3mf")
        directories[nodes[i].actual.parent_path()].push_back(i);
    for (const auto& [key, members] : directories) {
      (void)key;
      cancel();
      std::vector<std::string> names;
      std::map<std::string, std::size_t> by_name;
      for (auto index : members) {
        const auto value = name(nodes[index].actual.filename()).display;
        names.push_back(value);
        by_name[value] = index;
      }
      const auto grouping = archive::group_volumes(names, config);
      for (const auto& set : grouping.volume_sets) {
        Unit unit;
        unit.value = {{"id", numbered('v', sets.size() + 1, 4)},
                      {"scheme", archive::to_string(set.scheme)},
                      {"base", set.base},
                      {"members", J::array()},
                      {"missing_indices", set.missing_indices},
                      {"confirmed", false}};
        for (const auto& member : set.members) {
          const auto index = by_name.at(member);
          unit.members.push_back(index);
          unit.value["members"].push_back(nodes[index].value["id"]);
          node_set[index] = sets.size();
        }
        sets.push_back(std::move(unit));
      }
    }
  }
  std::vector<std::string> components(std::string_view path) const {
    std::vector<std::string> result;
    std::size_t at = 0;
    while (at < path.size()) {
      auto end = path.find('/', at);
      if (end == std::string_view::npos) end = path.size();
      result.emplace_back(path.substr(at, end - at));
      at = end + 1;
    }
    return result;
  }
  std::string boundary(std::size_t index) const {
    const auto& n = nodes[index];
    const auto root = n.value["root"].get<std::size_t>();
    if (request.single_candidate) return "single";
    if (settings.value("boundaries", J::object()).value("candidate", std::string("root")) == "root")
      return "root:" + std::to_string(root);
    const auto parts = components(n.value["path"].get<std::string>());
    const auto context = settings.value("boundaries", J::object()).value("context_levels", std::uint64_t{0});
    const auto depth = n.value["kind"] == "directory" ? parts.size() : parts.empty() ? 0 : parts.size() - 1;
    if (depth > context) {
      std::string base;
      for (std::size_t k = 0; k <= context; ++k) {
        if (k) base += '/';
        base += parts[k];
      }
      return std::to_string(root) + ":" + base;
    }
    if (n.value["kind"] == "directory") return {};
    return "loose:" + std::to_string(root) + ":" + n.value["id"].get<std::string>();
  }
  std::vector<Candidate> candidates();
  void matching(std::vector<Candidate>&);
};
void reason(J& candidate, std::string value) {
  auto& reasons = candidate["reasons"];
  if (std::find(reasons.begin(), reasons.end(), value) == reasons.end()) reasons.push_back(std::move(value));
}
std::vector<Candidate> Scanner::candidates() {
  std::map<std::string, Candidate> groups;
  std::set<std::string> separated;
  for (std::size_t i = 0; i < nodes.size(); ++i)
    if (nodes[i].ready) separated.insert(boundary(i));
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    cancel();
    const auto& n = nodes[i];
    auto key = n.ready ? "ready:" + n.value["id"].get<std::string>() : boundary(i);
    const auto set = node_set.find(i);
    bool kept = false;
    if (set != node_set.end()) {
      const auto first = sets[set->second].members.front();
      key = boundary(first);
      for (auto member : sets[set->second].members) kept = kept || boundary(member) != key;
    }
    if (key.empty()) continue;
    auto [it, added] = groups.try_emplace(key);
    auto& c = it->second;
    if (added) {
      c.order = i;
      const auto root = n.value["root"].get<std::size_t>();
      std::string base;
      const auto leaf = settings.value("boundaries", J::object()).value("candidate", std::string("root")) ==
                            "leaf_dir_with_payload" &&
                        !request.single_candidate;
      if (leaf && !key.starts_with("loose:") && !n.ready) {
        const auto parts = components(n.value["path"].get<std::string>());
        const auto count = settings.value("boundaries", J::object()).value("context_levels", std::uint64_t{0}) + 1;
        for (std::size_t k = 0; k < std::min<std::uint64_t>(parts.size(), count); ++k) {
          if (k) base += '/';
          base += parts[k];
        }
      }
      std::string title = n.value["name"].get<std::string>();
      if (request.single_candidate) title = "Selected sources";
      if (!base.empty()) {
        auto parts = components(base);
        title = parts.back();
      }
      c.value = {{"kind", n.ready ? "ready_asset" : "new_asset"},
                 {"root", root},
                 {"base_path", base},
                 {"title_hint", title},
                 {"context", J::object()},
                 {"confidence", key.starts_with("loose:") ? "low"
                                : leaf                    ? "medium"
                                                          : "high"},
                 {"reasons", J::array()},
                 {"inputs", J::array()},
                 {"entries", J::array()},
                 {"directories", J::array()},
                 {"volume_sets", J::array()},
                 {"suggested_links", J::array()},
                 {"matches", J::array()},
                 {"warnings", J::array()},
                 {"action", "import"}};
      c.value["filtered_links"] = J::array();
      const auto initial = n.ready ? (n.asset.value("valid", false) ? "ready_asset_manifest" : "ready_asset_invalid")
                           : request.single_candidate                                ? "user_single"
                           : key.starts_with("loose:")                               ? "loose_file_in_context"
                           : leaf                                                    ? "depth_rule"
                           : n.value["path"] == "" && n.value["kind"] == "directory" ? "root_directory"
                                                                                     : "root_file";
      reason(c.value, initial);
      if (n.ready) {
        if (!n.asset.value("valid", false)) c.value["action"] = "ask";
        for (const auto* field : {"asset_id", "revision"})
          if (n.asset.contains(field)) c.value[field] = n.asset[field];
        if(n.asset.value("probe_error",std::string{})=="am.zip.duplicate_name") c.value["warnings"].push_back(n.asset["probe_error"]);
        if(n.asset.value("manifest_detected",false)) c.value["warnings"].push_back("am.import.ready_asset_manifest_detected");
      }
      if (leaf && !n.ready) {
        const auto parts = components(n.value["path"].get<std::string>());
        const auto context = settings.value("boundaries", J::object()).value("context_levels", std::uint64_t{0});
        const auto label = settings.value("provenance", J::object()).value("parent_as", std::string{});
        if (!label.empty()) {
          c.value["context"]["sources"] = J::array();
          const auto ancestors = n.value["kind"] == "directory" ? parts.size() : parts.empty() ? 0 : parts.size() - 1;
          for (std::size_t k = 0; k < std::min<std::uint64_t>(context, ancestors); ++k)
            c.value["context"]["sources"].push_back({{"kind", label}, {"name", parts[k]}, {"confirmed", false}});
        }
      }
    }
    c.members.push_back(i);
    if (kept) reason(c.value, "volume_set_kept_together");
  }
  std::vector<Candidate> result;
  for (auto& [key, c] : groups) {
    bool payload = false;
    std::set<std::size_t> included_sets;
    std::map<std::string, std::string> case_names;
    bool collision = false;
    std::vector<std::size_t> free_files;
    for (auto index : c.members) {
      const auto& n = nodes[index];
      const auto id = n.value["id"].get<std::string>();
      if (n.value["status"] == "unavailable") {
        reason(c.value, "has_unavailable");
        c.value["action"] = "ask";
        payload = true;
      }
      if (n.value["kind"] == "directory") {
        c.value["directories"].push_back(id);
        if (n.value["status"] == "unavailable" || n.value.value("empty", false)) free_files.push_back(index);
        continue;
      }
      std::string decision = n.value["status"] == "unavailable"                           ? "ask"
                             : n.value["kind"] == "symlink" || n.value["kind"] == "other" ? "skip"
                             : n.rule.confidence == Confidence::certain                   ? "exclude"
                             : n.rule.confidence == Confidence::probable                  ? "ask"
                                                                                          : "include";
      if (n.ready && n.rule.confidence != Confidence::none) {
        decision = "ask";
        reason(c.value, "ready_asset_exclusion_conflict");
      }
      J entry = {{"node", id}, {"decision", decision}};
      if (n.value["kind"] == "symlink") entry["explanation_id"] = "am.import.link_not_followed";
      if (n.rule.confidence != Confidence::none) {
        static const std::map<std::string, std::string> explanations{
            {"macos.appledouble", "am.import.exclusion.appledouble"},
            {"macos.macosx_dir", "am.import.exclusion.macosx_dir"},
            {"macos.ds_store", "am.import.exclusion.ds_store"},
            {"windows.thumbs_db", "am.import.exclusion.thumbs_db"},
            {"windows.desktop_ini", "am.import.exclusion.desktop_ini"},
            {"os.icon_r", "am.import.exclusion.icon_r"}};
        entry["rule_id"] = n.rule.rule_id;
        entry["confidence"] = n.rule.confidence == Confidence::certain ? "certain" : "probable";
        entry["explanation_id"] =
            n.rule.explanation_id.empty() ? explanations.at(n.rule.rule_id) : n.rule.explanation_id;
      }
      if (decision == "include" || decision == "ask") payload = true;
      if (decision == "ask") c.value["action"] = "ask";
      c.value["entries"].push_back(entry);
      auto relative = n.value["path"].get<std::string>();
      if (relative.empty()) relative = n.value["name"].get<std::string>();
      const auto identity = folded(relative);
      auto [found, unique] = case_names.emplace(identity, relative);
      if (!unique && found->second != relative) collision = true;
      if (n.value["kind"] != "file") {
        if (n.value["status"] == "unavailable") free_files.push_back(index);
        continue;
      }
      const auto volume = node_set.find(index);
      if (volume != node_set.end()) {
        if (included_sets.insert(volume->second).second) {
          const auto& set = sets[volume->second];
          c.value["volume_sets"].push_back(set.value);
          c.value["inputs"].push_back(
              {{"kind", "multivolume_archive"}, {"objects", J::array({{{"volume_set", set.value["id"]}}})}});
          reason(c.value, "volume_set_unconfirmed");
          c.value["action"] = "ask";
        }
      } else if (n.ready || (is_archive(n.kind) && n.value.value("detected", std::string{}) != "project.3mf"))
        c.value["inputs"].push_back({{"kind", "archive"}, {"objects", J::array({{{"node", id}}})}});
      else
        free_files.push_back(index);
      for (const auto& url : n.urls) {
        bool ad = false;
        for (const auto& pattern :
             settings.value("provenance", J::object()).value("url_ad_patterns", std::vector<std::string>{})) {
          auto expression = pattern;
          auto flags = std::regex::ECMAScript;
          if (expression.starts_with("(?i)")) {
            expression.erase(0, 4);
            flags |= std::regex::icase;
          }
          ad = ad || std::regex_search(url, std::regex(expression, flags));
        }
        c.value[ad ? "filtered_links" : "suggested_links"].push_back(
            {{"url", url}, {"confirmed", false}, {"ad", ad}, {"node", id}});
      }
    }
    const auto leaf =
        settings.value("boundaries", J::object()).value("candidate", std::string("root")) == "leaf_dir_with_payload" &&
        !request.single_candidate;
    if (separated.contains(key)) reason(c.value, "ready_asset_separated");
    if (!payload && c.value["kind"] != "ready_asset" && (leaf || separated.contains(key))) {
      for (auto index : c.members)
        if (nodes[index].value["kind"] == "directory" && !nodes[index].value.value("empty", false))
          nodes[index].value["excluded_only"] = true;
      continue;
    }
    if (collision) c.value["warnings"].push_back("case_collision");
    if (!free_files.empty()) {
      J objects = J::array();
      for (auto index : free_files) objects.push_back({{"node", nodes[index].value["id"]}});
      c.value["inputs"].push_back(
          {{"kind", free_files.size() == 1 && c.value["directories"].empty() ? "file" : "directory"},
           {"objects", objects}});
    }
    if (c.value["inputs"].empty() && !c.value["directories"].empty())
      c.value["inputs"].push_back(
          {{"kind", "directory"}, {"objects", J::array({{{"node", c.value["directories"][0]}}})}});
    result.push_back(std::move(c));
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
  std::size_t input_id = 0;
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i].value["id"] = numbered('c', i + 1, 4);
    for (auto& input : result[i].value["inputs"]) input["id"] = numbered('i', ++input_id, 6);
  }
  return result;
}
void Scanner::matching(std::vector<Candidate>& candidates) {
  if (!request.library) return;
  library::OpenOptions options;
  options.mode = library::Mode::read_only;
  catalog::IndexerHooks hooks;
  hooks.before_read = [&](const fs::path& path) -> Result<void> {
    if (request.hooks.on_library_asset_read) request.hooks.on_library_asset_read(path);
    return {};
  };
  auto catalog = take(catalog::Catalog::open(*request.library, options, hooks));
  auto& library = catalog.library();
  auto* database = library.database(library::DatabaseKind::index);
  if (!database) throw error("am.import.library_unavailable", {}, Errc::io_error);
  struct Match {
    std::string asset, id, kind;
  };
  std::map<std::pair<std::string, std::string>, std::vector<Match>> weak;
  std::map<std::string, std::vector<Match>> hashes;
  std::set<std::pair<std::string, std::string>> wanted_names;
  std::set<std::string> wanted_sizes, wanted_hashes, wanted_ids;
  for (const auto& n : nodes) {
    if (n.value["kind"] != "file") continue;
    if (n.value.contains("size")) {
      const auto size = n.value["size"].get<std::string>();
      wanted_sizes.insert(size);
      wanted_names.emplace(folded(n.value["name"].get<std::string>()), size);
    }
    if (n.value.contains("sha256")) wanted_hashes.insert(n.value["sha256"].get<std::string>());
  }
  for (const auto& c : candidates)
    if (c.value.contains("asset_id")) wanted_ids.insert(c.value["asset_id"].get<std::string>());
  for (const bool input : {true, false}) {
    auto query = take(db::Connection::borrow(database).prepare(
        input ? "SELECT asset_id,object_id,original_name,size,sha256 FROM input_objects"
              : "SELECT asset_id,file_id,name,size,sha256 FROM files"));
    while (take(query.step()) == db::Step::row) {
      cancel();
      std::optional<std::pair<std::string, std::string>> name_key;
      if (!query.is_null(3)) {
        const auto size = take(query.text(3));
        if (wanted_sizes.contains(size)) {
          auto key = std::pair(folded(take(query.text(2))), size);
          if (wanted_names.contains(key)) name_key = std::move(key);
        }
      }
      std::string sha;
      if (!wanted_hashes.empty() && !query.is_null(4)) sha = take(query.text(4));
      const bool hash_match = wanted_hashes.contains(sha);
      if (!name_key && !hash_match) continue;
      Match match{take(query.text(0)), take(query.text(1)), input ? "sha256_input" : "sha256_file"};
      if (name_key) weak[*name_key].push_back(match);
      if (hash_match) hashes[sha].push_back(match);
    }
  }
  std::map<std::string, std::uint64_t> revisions;
  auto query = take(db::Connection::borrow(database).prepare("SELECT asset_id,revision FROM assets"));
  while (take(query.step()) == db::Step::row) {
    cancel();
    const auto id = take(query.text(0));
    if (wanted_ids.contains(id)) revisions[id] = static_cast<std::uint64_t>(query.integer(1));
  }
  for (auto& c : candidates) {
    cancel();
    for (auto index : c.members) {
      const auto& n = nodes[index].value;
      if (n["kind"] != "file") continue;
      auto emit = [&](const Match& match, const std::string& kind) {
        c.value["matches"].push_back(
            {{"kind", kind}, {"node", n["id"]}, {"asset_id", match.asset}, {"object_id", match.id}});
      };
      if (n.contains("size")) {
        const auto found = weak.find({folded(n["name"].get<std::string>()), n["size"].get<std::string>()});
        if (found != weak.end())
          for (const auto& match : found->second) emit(match, "name_size");
      }
      if (n.contains("sha256")) {
        const auto found = hashes.find(n["sha256"].get<std::string>());
        if (found != hashes.end())
          for (const auto& match : found->second) emit(match, match.kind);
      }
    }
    if (c.value["kind"] == "ready_asset" && c.value.contains("asset_id") && c.value.contains("revision")) {
      const auto id = c.value["asset_id"].get<std::string>();
      const auto found = revisions.find(id);
      if (found != revisions.end()) {
        const auto rev = c.value["revision"].get<std::uint64_t>();
        c.value["matches"].push_back({{"kind", "same_asset_id"},
                                      {"asset_id", id},
                                      {"relation", rev == found->second  ? "same_revision"
                                                   : rev > found->second ? "newer"
                                                                         : "older"}});
      }
    }
    if (!c.value["matches"].empty()) c.value["action"] = "ask";
    if (request.skip_identical) {
      std::set<std::string> exact;
      for (const auto& match : c.value["matches"])
        if (match["kind"] == "sha256_input" && match.contains("node")) exact.insert(match["node"].get<std::string>());
      bool all = true, any = false;
      for (const auto& input : c.value["inputs"])
        for (const auto& object : input["objects"]) {
          std::vector<std::string> ids;
          if (object.contains("node"))
            ids.push_back(object["node"].get<std::string>());
          else
            for (const auto& set : c.value["volume_sets"])
              if (set["id"] == object["volume_set"])
                for (const auto& id : set["members"]) ids.push_back(id.get<std::string>());
          for (const auto& id : ids) {
            any = true;
            all = all && exact.contains(id);
          }
        }
      const auto& reasons = c.value["reasons"];
      const bool exclusion_conflict =
          std::find(reasons.begin(), reasons.end(), "ready_asset_exclusion_conflict") != reasons.end();
      if (all && any && !exclusion_conflict) c.value["action"] = "skip";
    }
  }
}
}  // namespace
Result<void> validate_scan_paths(const ScanRequest& request, const std::optional<fs::path>& output) {
  try {
    for (const auto& root : request.roots) {
      if (request.library && (within(*request.library, root) || within(root, *request.library)))
        return error("am.import.library_overlaps_source");
      if (output && within(*output, root)) return error("am.import.output_inside_source");
    }
    return {};
  } catch (const std::exception&) {
    return error("am.import.root_unavailable", {}, Errc::io_error);
  } catch (...) {
    return error("am.import.root_unavailable", {}, Errc::io_error);
  }
}
Result<ImportPlan> scan(const ScanRequest& request) {
  try {
    take(validate_profile(request.profile));
    if (request.roots.empty() || (request.skip_identical && !request.hash_inputs))
      return error("am.import.arguments_invalid");
    take(validate_scan_paths(request));
    const auto start = std::chrono::steady_clock::now();
    Scanner scanner(request);
    struct ScanEnd {
      Scanner& scanner;
      const ScanRequest& request;
      std::chrono::steady_clock::time_point start;
      std::uint64_t candidates = 0;
      bool finished = false;
      ~ScanEnd() {
        AM_LOG(info, "import", "import.scan.finished", "Import scan finished",
               {{"nodes", scanner.progress.nodes},
                {"candidates", candidates},
                {"profile", request.profile.settings["id"].get<std::string>()},
                {"completeness", finished && !scanner.incomplete ? std::string("complete") : std::string("incomplete")},
                {"outcome", finished ? std::string("finished") : std::string("stopped")},
                {"duration_ms", static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              std::chrono::steady_clock::now() - start)
                                                              .count())}});
      }
    } end{scanner, request, start};
    AM_LOG(info, "import", "import.scan.started", "Import scan started",
           {{"profile", request.profile.settings["id"].get<std::string>()}});
    scanner.walk();
    scanner.volumes();
    auto candidates = scanner.candidates();
    end.candidates = candidates.size();
    scanner.matching(candidates);
    scanner.cancel();
    J nodes = J::array(), output = J::array();
    std::uint64_t files = 0, unavailable = 0;
    for (auto& node : scanner.nodes) {
      files += node.value["kind"] == "file";
      unavailable += node.value["status"] == "unavailable";
      nodes.push_back(std::move(node.value));
    }
    for (auto& c : candidates) output.push_back(std::move(c.value));
    std::vector<std::string> not_applied;
    for (const auto* key : {"nested", "previews", "groups"})
      if (request.profile.settings.contains(key)) not_applied.push_back(key);
    not_applied.push_back("limits.max_temp_bytes");
    const auto limits=snapshot_import_limits(request.profile);
    J value = {
        {"format", "am-import-plan"},
        {"version", 2},
        {"created_at", time_text(request.hooks.clock ? request.hooks.clock() : std::chrono::system_clock::now())},
        {"generator", {{"name", "am"}, {"version", kAppVersion}}},
        {"profile",
         {{"id", request.profile.settings["id"]},
          {"version", 1},
          {"sha256", request.profile.sha256},
          {"not_applied", not_applied}}},
        {"mode", request.profile.settings.value("mode", J::object()).value("strict", true) ? "strict" : "partial"},
        {"roots", scanner.roots},
        {"scan",
         {{"completeness", scanner.incomplete ? "incomplete" : "complete"},
          {"limits_hit", scanner.limited ? J::array({"max_entries"}) : J::array()},
          {"counts", {{"nodes", nodes.size()}, {"files", files}, {"unavailable", unavailable}}}}},
        {"nodes", std::move(nodes)},
        {"candidates", std::move(output)}};
    value["profile"]["limits"]={{"max_entries",std::to_string(limits.max_entries)},{"max_unpacked_bytes",std::to_string(limits.max_unpacked_bytes)},
      {"max_ratio",limits.max_ratio},{"max_seconds_per_container",std::to_string(limits.max_seconds_per_container)},{"max_temp_bytes",std::to_string(limits.max_temp_bytes)}};
    value["archives"]=J::array(); value["archive_entries"]=J::array();
    ImportPlan plan{std::move(value)};
    take(inventory_zip_entries(plan,request,scanner.zip_job));
    end.finished = true;
    return plan;
  } catch (const Error& e) {
    return e;
  } catch (const std::bad_alloc&) {
    return error("am.import.out_of_memory", {}, Errc::resource_exhausted);
  } catch (const std::exception& e) {
    return error("am.import.scan_failed", e.what(), Errc::io_error);
  } catch (...) {
    return error("am.import.scan_failed", {}, Errc::io_error);
  }
}
}  // namespace am::import
