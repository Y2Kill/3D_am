#include <algorithm>
#include <charconv>
#include <string_view>

#include "am/foundation/unicode.hpp"

namespace am::manifest::semantic_detail {
namespace {
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool idstring(std::string_view s) {
  return !s.empty() &&
         std::all_of(s.begin(), s.end(), [](char c) { return alpha(c) || digit(c) || c == '-' || c == '.'; });
}
bool simple(std::string_view s) {
  if (s.starts_with("DocumentRef-")) {
    auto colon = s.find(':');
    return colon != s.npos && idstring(s.substr(12, colon - 12)) && s.substr(colon + 1).starts_with("LicenseRef-") &&
           idstring(s.substr(colon + 12));
  }
  if (s.starts_with("LicenseRef-")) return idstring(s.substr(11));
  if (s.ends_with('+')) s.remove_suffix(1);
  return idstring(s) && s != "AND" && s != "OR" && s != "WITH";
}
bool unreserved(char c) { return alpha(c) || digit(c) || c == '-' || c == '.' || c == '_' || c == '~'; }
bool sub(char c) { return std::string_view("!$&'()*+,;=").find(c) != std::string_view::npos; }
bool component(std::string_view s, std::string_view extra) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    auto c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) continue;  // The caller validated UTF-8 and rejected controls.
    if (c == '%') {
      if (i + 2 >= s.size() || !hex(s[i + 1]) || !hex(s[i + 2])) return false;
      i += 2;
    } else if (!unreserved(static_cast<char>(c)) && !sub(static_cast<char>(c)) &&
               extra.find(static_cast<char>(c)) == extra.npos)
      return false;
  }
  return true;
}
bool ipv4(std::string_view s) {
  unsigned parts = 0;
  for (std::size_t start = 0; start < s.size();) {
    auto end = s.find('.', start);
    if (end == s.npos) end = s.size();
    auto p = s.substr(start, end - start);
    unsigned n = 0;
    auto r = std::from_chars(p.data(), p.data() + p.size(), n);
    if (p.empty() || p.size() > 3 || (p.size() > 1 && p[0] == '0') || r.ec != std::errc{} ||
        r.ptr != p.data() + p.size() || n > 255)
      return false;
    ++parts;
    if (end == s.size()) return parts == 4;
    start = end + 1;
    if (start == s.size()) return false;
  }
  return false;
}
bool ipv6(std::string_view s) {
  auto compress = s.find("::");
  if (compress != s.npos && s.find("::", compress + 2) != s.npos) return false;
  unsigned count = 0;
  auto side = [&](std::string_view part, bool final) {
    if (part.empty()) return true;
    for (std::size_t start = 0; start < part.size();) {
      auto end = part.find(':', start);
      if (end == part.npos) end = part.size();
      auto item = part.substr(start, end - start);
      if (item.find('.') != item.npos) {
        if (!final || end != part.size() || !ipv4(item)) return false;
        count += 2;
      } else {
        if (item.empty() || item.size() > 4 || !std::all_of(item.begin(), item.end(), hex)) return false;
        ++count;
      }
      if (end == part.size()) return true;
      start = end + 1;
      if (start == part.size()) return false;
    }
    return true;
  };
  if (compress == s.npos) return side(s, true) && count == 8;
  return side(s.substr(0, compress), false) && side(s.substr(compress + 2), true) && count < 8;
}
bool literal(std::string_view s) {
  if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) {
    auto dot = s.find('.');
    return dot > 1 && dot != s.npos && dot + 1 < s.size() &&
           std::all_of(s.begin() + 1, s.begin() + static_cast<std::ptrdiff_t>(dot), hex) &&
           component(s.substr(dot + 1), ":") && s.find('%') == s.npos;
  }
  return ipv6(s);
}
}  // namespace
bool spdx(std::string_view text) {
  // Iterative expression grammar; WITH can follow a simple expression only.
  bool operand = true, may_with = false;
  std::size_t depth = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') {
      ++i;
      continue;
    }
    if (text[i] == '(') {
      if (!operand) return false;
      ++depth;
      ++i;
      may_with = false;
      continue;
    }
    if (text[i] == ')') {
      if (operand || depth == 0) return false;
      --depth;
      ++i;
      may_with = false;
      continue;
    }
    auto start = i;
    while (i < text.size() && text[i] != '(' && text[i] != ')' && text[i] != ' ' && text[i] != '\t' &&
           text[i] != '\n' && text[i] != '\r')
      ++i;
    auto token = text.substr(start, i - start);
    if (operand) {
      if (!simple(token)) return false;
      operand = false;
      may_with = true;
    } else if (token == "WITH") {
      if (!may_with || start == 0 || (text[start - 1] != ' ' && text[start - 1] != '\t')) return false;
      auto whitespace = i;
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
      if (i == whitespace) return false;
      auto begin = i;
      while (i < text.size() && (alpha(text[i]) || digit(text[i]) || text[i] == '-' || text[i] == '.')) ++i;
      auto exception = text.substr(begin, i - begin);
      if (!idstring(exception) || exception == "AND" || exception == "OR" || exception == "WITH") return false;
      may_with = false;
    } else if (token == "AND" || token == "OR") {
      if (start == 0 || (text[start - 1] != ' ' && text[start - 1] != '\t') || i == text.size() ||
          (text[i] != ' ' && text[i] != '\t'))
        return false;
      operand = true;
      may_with = false;
    } else
      return false;
  }
  return !operand && depth == 0;
}
bool uri(std::string_view text) {
  if (!unicode::is_valid_utf8(text)) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    auto c = static_cast<unsigned char>(text[i]);
    if (c <= 0x20 || c == 0x7f ||
        (c == 0xc2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
         static_cast<unsigned char>(text[i + 1]) <= 0x9f))
      return false;
  }
  auto colon = text.find(':');
  if (colon == text.npos || colon == 0 || !alpha(text[0])) return false;
  for (auto c : text.substr(1, colon - 1))
    if (!alpha(c) && !digit(c) && c != '+' && c != '-' && c != '.') return false;
  auto rest = text.substr(colon + 1);
  auto hash = rest.find('#');
  if (hash != rest.npos) {
    if (!component(rest.substr(hash + 1), ":@/?")) return false;
    rest = rest.substr(0, hash);
  }
  auto query = rest.find('?');
  if (query != rest.npos) {
    if (!component(rest.substr(query + 1), ":@/?")) return false;
    rest = rest.substr(0, query);
  }
  if (rest.starts_with("//")) {
    rest.remove_prefix(2);
    auto slash = rest.find('/');
    auto authority = rest.substr(0, slash);
    rest = slash == rest.npos ? std::string_view{} : rest.substr(slash);
    auto at = authority.find('@');
    if (at != authority.npos) {
      if (!component(authority.substr(0, at), ":")) return false;
      authority.remove_prefix(at + 1);
    }
    std::string_view port;
    if (authority.starts_with('[')) {
      auto end = authority.find(']');
      if (end == authority.npos || !literal(authority.substr(1, end - 1))) return false;
      auto tail = authority.substr(end + 1);
      if (!tail.empty()) {
        if (tail[0] != ':') return false;
        port = tail.substr(1);
      }
    } else {
      auto p = authority.find(':');
      auto host = authority.substr(0, p);
      if (!component(host, "")) return false;
      if (p != authority.npos) port = authority.substr(p + 1);
    }
    if (!std::all_of(port.begin(), port.end(), digit)) return false;
  } else if (rest.starts_with('/')) {
    if (rest.starts_with("//")) return false;
  }
  return component(rest, ":@/");
}
}  // namespace am::manifest::semantic_detail
