#include "am/storage/archive/volume_detect.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <tuple>

namespace am::archive {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
  return s;
}

struct Key {
  VolumeScheme scheme;
  std::string base_lower;
  std::string ext_lower;
  auto operator<=>(const Key&) const = default;
};

struct Group {
  std::string base;
  std::map<int, std::string> by_index;
};

}  // namespace

std::string to_string(VolumeScheme s) {
  switch (s) {
    case VolumeScheme::rar_part: return "rar_part";
    case VolumeScheme::rar_old: return "rar_old";
    case VolumeScheme::split_numeric: return "split_numeric";
    case VolumeScheme::zip_split: return "zip_split";
    case VolumeScheme::copy_suffix: return "copy_suffix";
  }
  return "?";
}

GroupingResult group_volumes(const std::vector<std::string>& names, const VolumeNamingRules& rules) {
  static const std::regex kRarPart(R"(^(.*)\.part(\d{1,4})\.rar$)", std::regex::icase);
  static const std::regex kSplit(R"(^(.*)\.(\d{3})$)", std::regex::icase);
  static const std::regex kRarOld(R"(^(.*)\.r(\d{2})$)", std::regex::icase);
  static const std::regex kZipSplit(R"(^(.*)\.z(\d{2})$)", std::regex::icase);
  static const std::regex kCopySuffix(R"(^(.*) \((\d{1,3})\)\.([A-Za-z0-9]{1,8})$)");

  std::map<Key, Group> groups;
  std::set<std::string> claimed;
  std::map<std::string, std::string> by_lower;  // lower name -> original
  for (const auto& n : names) by_lower.emplace(lower(n), n);

  auto add = [&](VolumeScheme s, const std::string& base, const std::string& ext, int idx, const std::string& name) {
    auto& g = groups[Key{s, lower(base), lower(ext)}];
    g.base = base;
    g.by_index.emplace(idx, name);
  };

  auto copy_ext_ok = [&](const std::string& ext) {
    auto e = lower(ext);
    for (const auto& x : rules.copy_suffix_extensions)
      if (lower(x) == e) return true;
    return false;
  };

  std::smatch m;
  for (const auto& n : names) {
    if (rules.rar_part && std::regex_match(n, m, kRarPart)) {
      add(VolumeScheme::rar_part, m[1], "rar", std::stoi(m[2]), n);
    } else if (rules.split_numeric && std::regex_match(n, m, kSplit)) {
      add(VolumeScheme::split_numeric, m[1], "", std::stoi(m[2]), n);
    } else if (rules.rar_old && std::regex_match(n, m, kRarOld)) {
      add(VolumeScheme::rar_old, m[1], "rar", std::stoi(m[2]) + 2, n);  // X.rar = 1, X.r00 = 2
    } else if (rules.zip_split && std::regex_match(n, m, kZipSplit)) {
      add(VolumeScheme::zip_split, m[1], "zip", std::stoi(m[2]), n);
    } else if (rules.copy_suffix && std::regex_match(n, m, kCopySuffix) && copy_ext_ok(m[3])) {
      add(VolumeScheme::copy_suffix, m[1], m[3], std::stoi(m[2]), n);
    }
  }

  // Attach the "head" file that carries no number for schemes that need it.
  for (auto& [key, g] : groups) {
    auto head_lower = key.base_lower + "." + key.ext_lower;
    auto it = by_lower.find(head_lower);
    if (key.scheme == VolumeScheme::rar_old && it != by_lower.end()) {
      g.by_index.emplace(1, it->second);
    } else if (key.scheme == VolumeScheme::zip_split && it != by_lower.end()) {
      int last = g.by_index.empty() ? 1 : g.by_index.rbegin()->first + 1;
      g.by_index.emplace(last, it->second);  // X.zip is the LAST piece
    } else if (key.scheme == VolumeScheme::copy_suffix && it != by_lower.end()) {
      g.by_index.emplace(1, it->second);
    }
  }

  GroupingResult result;
  for (auto& [key, g] : groups) {
    if (g.by_index.size() < 2) continue;  // a lone "X (1).zip" is a duplicate download, not a set
    VolumeCandidate c;
    c.scheme = key.scheme;
    c.base = g.base;
    c.needs_header_confirmation = key.scheme == VolumeScheme::copy_suffix;
    int expect = 1;  // every scheme numbers its first piece 1 (see index mapping above)
    for (auto& [idx, name] : g.by_index) {
      while (expect < idx) c.missing_indices.push_back(expect++);
      c.members.push_back(name);
      claimed.insert(name);
      expect = idx + 1;
    }
    result.volume_sets.push_back(std::move(c));
  }
  for (const auto& n : names)
    if (!claimed.count(n)) result.singles.push_back(n);
  return result;
}

}  // namespace am::archive
