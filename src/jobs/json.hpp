#pragma once
#include "am/foundation/json_text.hpp"
#include "am/foundation/unicode.hpp"

namespace am::jobs::detail {
inline std::string quote(std::string_view value) {
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  const bool utf8 = unicode::is_valid_utf8(value);
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 32 || (!utf8 && c >= 128)) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += static_cast<char>(c);
  }
  return out + '"';
}
inline const json::Node* field(const json::Tree& tree, const json::Node& object, std::string_view name) {
  if (object.kind != json::Kind::object) return nullptr;
  for (auto i : object.children)
    if (tree.nodes[i].edge == name) return &tree.nodes[i];
  return nullptr;
}
inline std::string error_json(const Error& e) {
  return "{\"code\":" + quote(errc_name(e.code)) + ",\"message_id\":" + quote(e.message_id) +
         ",\"detail\":" + quote(e.ctx.detail.empty() ? e.message : e.ctx.detail) + "}";
}
}  // namespace am::jobs::detail
