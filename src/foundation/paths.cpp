#include "am/foundation/paths.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <new>
#include <vector>

#include "am/foundation/unicode.hpp"

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

// NFC + Unicode simple case folding for collision comparison (ADR-0035 p.8).
std::string fold(std::string_view s) {
  return unicode::path_collision_key(s);
}

bool forbidden_filename_byte(char ch) {
  return static_cast<unsigned char>(ch) < 0x20 || ch == '<' || ch == '>' || ch == ':' ||
         ch == '"' || ch == '|' || ch == '?' || ch == '*' || ch == '\\';
}

bool sanitize_component(std::string& c) {
  bool changed = false;
  for (auto& ch : c) {
    if (forbidden_filename_byte(ch)) {
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

// create() guarantees room for the tag and at least one byte of stem.
std::size_t stem_budget(std::size_t root_bytes, std::string& stem, std::string& ext) {
  const auto available = ZipPathMapper::kMaxZipPathBytes - root_bytes - ZipPathMapper::kCollisionTagBytes;
  if (ext.size() >= available) {
    stem += ext;
    ext.clear();
  }
  return available - ext.size();
}

void shorten_stem(std::string& stem, std::size_t budget) {
  stem = truncate_utf8(stem, budget);
  while (!stem.empty() && (stem.back() == '/' || stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
  if (stem.empty()) stem = "_";
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

Result<void> check_container_path(std::string_view path) {
  try {
    auto invalid = [](std::string detail) -> Result<void> {
      return Error::make(Errc::invalid_argument, "Invalid container path", std::move(detail))
          .in("foundation.paths").id("am.paths.invalid_container_path");
    };
    if (path.empty() || path.size() > ZipPathMapper::kMaxZipPathBytes) return invalid("Empty path or path exceeds 200 bytes");
    if (!unicode::is_valid_utf8(path)) return invalid("Path is not valid UTF-8");
    auto normalized = unicode::nfc(path);
    if (!normalized) return normalized.error();
    if (*normalized != path) return invalid("Path is not NFC");
    for (std::size_t start = 0; start <= path.size();) {
      const auto slash = path.find('/', start);
      const auto stop = slash == std::string_view::npos ? path.size() : slash;
      const auto part = path.substr(start, stop - start);
      if (part.empty() || part == "." || part == "..") return invalid("Empty, dot or parent component");
      for (std::size_t i = 0; i < part.size(); ++i) {
        // The mapper's byte policy is shared. The strict reader additionally
        // rejects DEL/C1 controls without changing historical import mapping.
        if (forbidden_filename_byte(part[i]) || part[i] == '\x7f' ||
            (static_cast<unsigned char>(part[i]) == 0xc2 && i + 1 < part.size() &&
             static_cast<unsigned char>(part[i + 1]) >= 0x80 && static_cast<unsigned char>(part[i + 1]) <= 0x9f))
          return invalid("Forbidden filename character or control");
      }
      if (part.back() == '.' || part.back() == ' ') return invalid("Trailing dot or space");
      if (is_reserved_windows_name(part)) return invalid("Reserved Windows name");
      if (slash == std::string_view::npos) break;
      start = slash + 1;
    }
    return {};
  } catch (const std::bad_alloc&) {
    Error error;
    error.code = Errc::resource_exhausted;
    error.component = "foundation.paths";
    error.message_id = "am.paths.out_of_memory";
    return error;
  }
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

ZipPathMapper::ZipPathMapper(std::string root) : root_(std::move(root)) {}

Result<ZipPathMapper> ZipPathMapper::create(std::string root) {
  try {
    if (root.empty() || root.size() > kMaxRootBytes ||
        (root.size() == kMaxRootBytes && root.back() != '/')) {
      return Error::make(Errc::invalid_argument, "Invalid ZIP path root")
          .in("foundation.paths").id("paths.invalid_root");
    }
    if (root.back() != '/') root.push_back('/');
    return ZipPathMapper(std::move(root));
  } catch (const std::bad_alloc&) {
    Error error;
    error.code = Errc::resource_exhausted;
    return error;
  }
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
  if (rel.size() > kMaxZipPathBytes - root_.size()) {
    // Keep the file extension and as much of the leading path as fits, then a
    // stable hash of the ORIGINAL path so different long paths stay distinct.
    auto [stem, ext] = split_ext(parts.back());
    stem = rel.substr(0, rel.size() - parts.back().size()) + stem;
    const auto budget = stem_budget(root_.size(), stem, ext);
    shorten_stem(stem, budget);
    rel = stem + tag + ext;
    reason = RenameReason::path_too_long;
  }

  std::string full = root_ + rel;
  if (!used_folded_.insert(fold(full)).second) {
    auto [stem, ext] = split_ext(rel);
    const auto budget = stem_budget(root_.size(), stem, ext);
    if (stem.size() > budget) {
      shorten_stem(stem, budget);
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
