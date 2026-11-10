#include "am/foundation/paths.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace am::paths {
namespace {

std::vector<std::string> split(std::string_view s) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '/') {
      if (i > start) parts.emplace_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return parts;
}

// T-002: NFC + Unicode simple case folding (ADR-0035 p.8)
std::string fold(std::string_view s) {
  std::string r(s);
  std::transform(r.begin(), r.end(), r.begin(),
                 [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
  return r;
}

bool sanitize_component(std::string& c) {
  bool changed = false;
  for (auto& ch : c) {
    auto u = static_cast<unsigned char>(ch);
    if (u < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '|' || ch == '?' ||
        ch == '*' || ch == '\\') {
      ch = '_';
      changed = true;
    }
  }
  // Windows silently strips trailing dots/spaces ("STL Files for 3D Printing "
  // exists in the reference collection!) — make them explicit.
  while (!c.empty() && (c.back() == ' ' || c.back() == '.')) {
    c.pop_back();
    changed = true;
  }
  if (c.empty()) {
    c = "_";
    changed = true;
  }
  return changed;
}

std::string hex8(std::uint64_t v) {
  std::array<char, 9> buf{};
  std::snprintf(buf.data(), buf.size(), "%08x", static_cast<unsigned>(v & 0xFFFFFFFFu));
  return std::string(buf.data(), 8);
}

std::pair<std::string, std::string> split_ext(const std::string& name) {
  auto dot = name.rfind('.');
  auto slash = name.rfind('/');
  if (dot == std::string::npos || dot == 0 || name.size() - dot > 16 ||
      (slash != std::string::npos && dot < slash + 2))
    return {name, {}};
  return {name.substr(0, dot), name.substr(dot)};
}

}  // namespace

std::filesystem::path native(const std::filesystem::path& p) {
#ifdef _WIN32
  std::wstring s = p.wstring();
  if (s.rfind(L"\\\\?\\", 0) == 0) return p;
  std::replace(s.begin(), s.end(), L'/', L'\\');
  if (s.rfind(L"\\\\", 0) == 0) return std::filesystem::path(L"\\\\?\\UNC\\" + s.substr(2));
  if (s.size() >= 3 && s[1] == L':' && s[2] == L'\\') return std::filesystem::path(L"\\\\?\\" + s);
  return std::filesystem::path(s);  // relative: caller must absolutize first
#else
  return p;
#endif
}

std::string normalize_source_path(std::string_view original) {
  std::string s(original);
  std::replace(s.begin(), s.end(), '\\', '/');
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '/' && (out.empty() || out.back() == '/')) continue;
    out.push_back(c);
  }
  while (out.rfind("./", 0) == 0) out.erase(0, 2);
  if (!out.empty() && out.back() == '/') out.pop_back();
  return out;
}

std::string_view to_string(RenameReason r) {
  switch (r) {
    case RenameReason::none: return "none";
    case RenameReason::path_too_long: return "path_too_long";
    case RenameReason::invalid_chars: return "invalid_chars";
    case RenameReason::reserved_name: return "reserved_name";
    case RenameReason::collision: return "collision";
    case RenameReason::encoding: return "encoding";
  }
  return "none";
}

bool is_reserved_windows_name(std::string_view component) {
  auto stem = component.substr(0, component.find('.'));
  std::string u = fold(stem);
  static constexpr std::array<std::string_view, 4> kPlain = {"con", "prn", "aux", "nul"};
  if (std::find(kPlain.begin(), kPlain.end(), u) != kPlain.end()) return true;
  if (u.size() == 4 && (u.rfind("com", 0) == 0 || u.rfind("lpt", 0) == 0) && u[3] >= '1' && u[3] <= '9')
    return true;
  return false;
}

std::string truncate_utf8(std::string_view s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return std::string(s);
  std::size_t n = max_bytes;
  // Step back over UTF-8 continuation bytes so we never split a code point.
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return std::string(s.substr(0, n));
}

std::uint64_t fnv1a64(std::string_view s) {
  std::uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

ZipPathMapper::ZipPathMapper(std::string root) : root_(std::move(root)) {
  if (root_.empty() || root_.back() != '/') root_.push_back('/');
}

MappedPath ZipPathMapper::map(std::string_view source_path) {
  const std::string norm = normalize_source_path(source_path);
  RenameReason reason = RenameReason::none;

  std::vector<std::string> parts;
  for (auto& c : split(norm)) {
    if (c == "." ) { continue; }
    if (c == "..") {  // zip-slip: never climb out of the input root
      if (reason == RenameReason::none) reason = RenameReason::invalid_chars;
      parts.emplace_back("__up__");
      continue;
    }
    if (c.size() == 2 && c[1] == ':') {  // "C:" drive prefix inside archive
      if (reason == RenameReason::none) reason = RenameReason::invalid_chars;
      continue;
    }
    if (sanitize_component(c) && reason == RenameReason::none) reason = RenameReason::invalid_chars;
    if (is_reserved_windows_name(c)) {
      c.insert(0, "_");
      if (reason == RenameReason::none) reason = RenameReason::reserved_name;
    }
    parts.push_back(std::move(c));
  }
  if (parts.empty()) {
    parts.emplace_back("_");
    reason = RenameReason::invalid_chars;
  }

  std::string rel;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) rel.push_back('/');
    rel += parts[i];
  }

  const std::string tag = "~" + hex8(fnv1a64(norm));
  if (root_.size() + rel.size() > kMaxZipPathBytes) {
    // Keep the file extension and as much of the leading path as fits, then a
    // stable hash of the ORIGINAL path so different long paths stay distinct.
    auto [stem, ext] = split_ext(parts.back());
    const std::size_t budget = kMaxZipPathBytes - root_.size() - tag.size() - ext.size();
    rel = truncate_utf8(rel.substr(0, rel.size() - parts.back().size()) + stem, budget);
    while (!rel.empty() && (rel.back() == '/' || rel.back() == ' ' || rel.back() == '.')) rel.pop_back();
    rel += tag + ext;
    reason = RenameReason::path_too_long;
  }

  std::string full = root_ + rel;
  if (!used_folded_.insert(fold(full)).second) {
    auto [stem, ext] = split_ext(rel);
    const std::size_t budget = kMaxZipPathBytes - root_.size() - tag.size() - ext.size();
    if (stem.size() > budget) {
      stem = truncate_utf8(stem, budget);
      while (!stem.empty() && (stem.back() == '/' || stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
      reason = RenameReason::path_too_long;
    }
    for (std::uint64_t k = 0;; ++k) {
      const auto suffix = k == 0 ? tag : "~" + hex8(fnv1a64(norm + '\x1f' + std::to_string(k)));
      std::string candidate = root_ + stem + suffix + ext;
      if (used_folded_.insert(fold(candidate)).second) {
        full = std::move(candidate);
        break;
      }
    }
    if (reason == RenameReason::none) reason = RenameReason::collision;
  }
  return {std::move(full), reason};
}

}  // namespace am::paths
