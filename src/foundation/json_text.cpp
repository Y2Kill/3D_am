#include "am/foundation/json_text.hpp"
#include "am/foundation/unicode.hpp"
#include <cstdint>
#include <new>
#include <stdexcept>
#include <unordered_set>

namespace am::json {
namespace {
bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool consume_number(std::string_view s, std::size_t& p) noexcept {
  if (p < s.size() && s[p] == '-') ++p;
  if (p == s.size()) return false;
  if (s[p] == '0') ++p;
  else if (s[p] >= '1' && s[p] <= '9') { do { ++p; } while (p < s.size() && digit(s[p])); }
  else return false;
  if (p < s.size() && s[p] == '.') {
    const auto start = ++p;
    while (p < s.size() && digit(s[p])) ++p;
    if (p == start) return false;
  }
  if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
    ++p;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
    const auto start = p;
    while (p < s.size() && digit(s[p])) ++p;
    if (p == start) return false;
  }
  return true;
}
Error error(Errc code, std::string_view id, std::string detail) {
  return Error::make(code, "Strict JSON text rejected", std::move(detail))
      .in("foundation.json").op("json.parse").id(std::string(id));
}
struct Rejected { Errc code; const char* id; std::size_t byte; };
struct Parser {
  std::string_view text;
  const ParseLimits& limits;
  Tree tree;
  std::size_t p = 0;
  struct Frame {
    std::size_t node;
    int state = 0;  // 0: first/end, 1: after comma, 2: comma/end
    std::unordered_set<std::string> keys;
  };
  std::vector<Frame> stack;
  [[noreturn]] void fail(const char* id = "am.json.invalid_json", Errc code = Errc::corrupt_data) const {
    throw Rejected{code, id, p};
  }
  void ws() { while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\n' || text[p] == '\r')) ++p; }
  std::uint32_t hex4() {
    std::uint32_t cp = 0;
    for (int i = 0; i < 4; ++i) {
      if (p == text.size()) fail();
      const char c = text[p++];
      const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (v < 0) fail();
      cp = cp * 16 + static_cast<std::uint32_t>(v);
    }
    return cp;
  }
  static void utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
      out += static_cast<char>(0xc0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xe0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63));
    } else {
      out += static_cast<char>(0xf0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63));
      out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63));
    }
  }
  std::string string() {
    if (p == text.size() || text[p++] != '"') fail();
    std::string out;
    while (p < text.size()) {
      const auto c = static_cast<unsigned char>(text[p++]);
      if (c == '"') return out;
      if (c < 32) fail();
      if (c != '\\') { out += static_cast<char>(c); continue; }
      if (p == text.size()) fail();
      switch (text[p++]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          auto cp = hex4();
          if (cp >= 0xd800 && cp <= 0xdbff) {
            if (text.substr(p, 2) != "\\u") fail();
            p += 2;
            const auto low = hex4();
            if (low < 0xdc00 || low > 0xdfff) fail();
            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
          } else if (cp >= 0xdc00 && cp <= 0xdfff) fail();
          utf8(out, cp); break;
        }
        default: fail();
      }
    }
    fail();
  }
  void value(std::size_t parent, std::string edge = {}, std::size_t index = 0) {
    ws();
    if (p == text.size()) fail();
    const auto id = tree.nodes.size();
    tree.nodes.push_back({Kind::literal, p, p, parent, std::move(edge), index, {}, {}});
    if (parent != no_node) tree.nodes[parent].children.push_back(id);
    const char c = text[p];
    if (c == '{' || c == '[') {
      if (stack.size() >= limits.max_depth) fail("am.json.limit_exceeded", Errc::resource_exhausted);
      tree.nodes[id].kind = c == '{' ? Kind::object : Kind::array;
      ++p;
      stack.push_back({id, 0, {}});
      return;
    }
    if (c == '"') { tree.nodes[id].kind = Kind::string; tree.nodes[id].decoded = string(); }
    else if (c == '-' || digit(c)) {
      tree.nodes[id].kind = Kind::number;
      if (!consume_number(text, p)) fail();
    } else {
      bool found = false;
      for (std::string_view literal : {"true", "false", "null"}) {
        if (text.substr(p, literal.size()) == literal) { p += literal.size(); found = true; break; }
      }
      if (!found) fail();
    }
    tree.nodes[id].end = p;
  }
  Tree run() {
    value(no_node);
    while (!stack.empty()) {
      ws();
      if (p == text.size()) fail();
      const auto parent = stack.back().node;
      const bool object = tree.nodes[parent].kind == Kind::object;
      const char closing = object ? '}' : ']';
      if (text[p] == closing && stack.back().state != 1) {
        tree.nodes[parent].end = ++p;
        stack.pop_back();
      } else if (stack.back().state == 2) {
        if (text[p++] != ',') fail();
        stack.back().state = 1;
      } else {
        std::string edge;
        const auto index = tree.nodes[parent].children.size();
        if (object) {
          edge = string();
          if (!stack.back().keys.insert(edge).second) fail("am.json.duplicate_key");
          ws();
          if (p == text.size() || text[p++] != ':') fail();
        }
        stack.back().state = 2;
        value(parent, std::move(edge), index);
      }
    }
    ws();
    if (p != text.size()) fail();
    return std::move(tree);
  }
};
}  // namespace
bool is_json_number(std::string_view text) noexcept {
  std::size_t p = 0;
  return consume_number(text, p) && p == text.size();
}
Result<Tree> parse_strict(std::string_view text, const ParseLimits& limits) {
  try {
    if (text.size() > limits.max_bytes) return error(Errc::resource_exhausted, "am.json.limit_exceeded", "Input size exceeds hypothesis max_bytes");
    if (text.starts_with("\xef\xbb\xbf") || !unicode::is_valid_utf8(text))
      return error(Errc::corrupt_data, "am.json.invalid_json", "Input must be UTF-8 without BOM");
    return Parser{text, limits, {}, 0, {}}.run();
  } catch (const Rejected& e) { return error(e.code, e.id, "byte=" + std::to_string(e.byte)); }
  catch (const std::bad_alloc&) { return error(Errc::resource_exhausted, "am.json.out_of_memory", "Allocation failed"); }
  catch (const std::length_error&) { return error(Errc::resource_exhausted, "am.json.limit_exceeded", "Container capacity exceeded"); }
}
}  // namespace am::json
