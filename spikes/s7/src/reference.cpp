#include "reference.hpp"
#include "candidates.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace s7 {
namespace {
bool digit(char c) { return c >= '0' && c <= '9'; }
bool consume_number(std::string_view s, std::size_t& p) {
  if (p < s.size() && s[p] == '-') ++p;
  if (p == s.size()) return false;
  if (s[p] == '0') ++p;
  else if (s[p] >= '1' && s[p] <= '9') { while (p < s.size() && digit(s[p])) ++p; }
  else return false;
  if (p < s.size() && s[p] == '.') {
    ++p;
    const auto start = p;
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
std::string escape_pointer(std::string_view s) {
  std::string out;
  for (char c : s) { if (c == '~') out += "~0"; else if (c == '/') out += "~1"; else out += c; }
  return out;
}
struct Parser {
  Document& doc;
  std::size_t p = 0, limit;
  void ws() { while (p < doc.text.size() && (doc.text[p] == ' ' || doc.text[p] == '\n' || doc.text[p] == '\r' || doc.text[p] == '\t')) ++p; }
  [[noreturn]] void fail(std::string_view reason) const {
    throw std::runtime_error(std::string(reason) + " at byte " + std::to_string(p));
  }
  std::string string() {
    const auto start = p++;
    bool escaped = false;
    while (p < doc.text.size()) {
      const auto c = static_cast<unsigned char>(doc.text[p++]);
      if (!escaped && c == '"') {
        // Only string decoding is delegated; no number enters the DOM.
        return nlohmann::json::parse(doc.text.substr(start, p - start)).get<std::string>();
      }
      if (c < 0x20) fail("control character in string");
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
    }
    fail("unterminated string");
  }
  std::size_t value(std::size_t parent, std::string edge, std::size_t depth) {
    if (depth > limit) fail("hypothesis_max_depth exceeded");
    doc.max_depth = std::max(doc.max_depth, depth);
    ws();
    if (p == doc.text.size()) fail("missing value");
    const auto id = doc.tokens.size();
    doc.tokens.push_back({Kind::literal, p, p, parent, std::move(edge), {}, {}});
    const char c = doc.text[p];
    if (c == '{' || c == '[') {
      const bool object = c == '{';
      doc.tokens[id].kind = object ? Kind::object : Kind::array;
      ++p;
      ws();
      const char end = object ? '}' : ']';
      std::unordered_set<std::string> keys;
      if (p < doc.text.size() && doc.text[p] == end) ++p;
      else {
        std::size_t index = 0;
        for (;;) {
          ws();
          std::string key;
          if (object) {
            if (p == doc.text.size() || doc.text[p] != '"') fail("object key required");
            key = string();
            if (!keys.insert(key).second) doc.duplicates = true;
            ws();
            if (p == doc.text.size() || doc.text[p++] != ':') fail("colon required");
          } else key = std::to_string(index++);
          const auto item = value(id, std::move(key), depth + 1);
          doc.tokens[id].children.push_back(item);
          ws();
          if (p == doc.text.size()) fail("unterminated container");
          if (doc.text[p] == end) { ++p; break; }
          if (doc.text[p++] != ',') fail("comma required");
        }
      }
    } else if (c == '"') {
      doc.tokens[id].kind = Kind::string;
      doc.tokens[id].decoded = string();
    } else if (c == '-' || digit(c)) {
      doc.tokens[id].kind = Kind::number;
      if (!consume_number(doc.text, p)) fail("invalid number");
    } else {
      bool found = false;
      for (const auto literal : {"true", "false", "null"}) {
        const std::string_view token(literal);
        if (doc.text.substr(p, token.size()) == token) { p += token.size(); found = true; break; }
      }
      if (!found) fail("invalid literal");
    }
    doc.tokens[id].end = p;
    return id;
  }
};
}  // namespace
bool number(std::string_view text) { std::size_t p = 0; return consume_number(text, p) && p == text.size(); }
std::string checked_number(std::string_view text) {
  if (!number(text)) throw std::runtime_error("unsafe numeric lexeme");
  return std::string(text);
}
Document::Document(std::string_view source, std::size_t depth) : text(source) {
  Parser parser{*this, 0, depth};
  parser.value(missing, {}, 0);
  parser.ws();
  if (parser.p != text.size()) throw std::runtime_error("trailing JSON bytes");
}
std::size_t Document::child(std::size_t parent, std::string_view key) const {
  if (parent == missing) return missing;
  for (auto id : tokens[parent].children) if (tokens[id].edge == key) return id;
  return missing;
}
std::string Document::path(std::size_t id, bool typed, bool stable) const {
  std::vector<std::size_t> chain;
  for (; id != missing && tokens[id].parent != missing; id = tokens[id].parent) chain.push_back(id);
  std::string out;
  const auto groups = stable ? opaque_groups() : std::vector<int>{};
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    out += '/';
    if (typed) out += tokens[tokens[*it].parent].kind == Kind::array ? "a:" : "o:";
    std::string edge = tokens[*it].edge;
    const auto parent = tokens[*it].parent;
    if (stable && groups[*it] < 0 && tokens[parent].kind == Kind::array) {
      const auto& array = tokens[parent].edge;
      const auto grandparent = tokens[parent].parent;
      const auto name = grandparent == missing ? std::string{} : tokens[grandparent].edge;
      std::string key;
      if ((array == "files" || array == "groups" || array == "field_definitions") && grandparent == 0) key = "id";
      else if (array == "custom_values" && name == "metadata") key = "field_id";
      else if (array == "claims" && name == "metadata") key = "id";
      else if (array == "entries" && name == "history") key = "operation_id";
      if (!key.empty()) {
        const auto identity = child(*it, key);
        if (identity == missing || tokens[identity].kind != Kind::string) throw std::runtime_error("missing stable owner ID");
        edge = key + ':' + tokens[identity].decoded;
        // Ambiguous owners must never silently overwrite a fingerprint map entry.
        for (const auto sibling : tokens[parent].children) {
          const auto other = child(sibling, key);
          if (sibling != *it && other != missing && tokens[other].decoded == tokens[identity].decoded)
            throw std::runtime_error("ambiguous stable owner ID");
        }
      }
    }
    out += escape_pointer(edge);
  }
  return out;
}
std::vector<int> Document::opaque_groups() const {
  std::vector<int> groups(tokens.size(), -1);
  auto mark = [&](std::size_t id, int group) { if (id != missing) groups[id] = group; };
  const auto fields = child(0, "field_definitions");
  std::unordered_set<std::string> plugin_fields;
  if (fields != missing) for (auto field : tokens[fields].children) {
    const auto type = child(field, "type"), fid = child(field, "id");
    if (type != missing && fid != missing && tokens[type].decoded.rfind("plugin:", 0) == 0) {
      plugin_fields.insert(tokens[fid].decoded);
      mark(child(field, "default"), 4); mark(child(field, "constraints"), 4);
    }
  }
  const auto extensions = child(0, "extensions");
  if (extensions != missing) for (auto ext : tokens[extensions].children) mark(child(ext, "data"), 0);
  auto metadata = [&](std::size_t owner) {
    const auto meta = child(owner, "metadata");
    const auto values = child(meta, "custom_values");
    if (values != missing) for (auto item : tokens[values].children) {
      const auto fid = child(item, "field_id");
      if (fid != missing && plugin_fields.count(tokens[fid].decoded)) mark(child(item, "value"), 1);
    }
    const auto claims = child(meta, "claims");
    if (claims != missing) for (auto item : tokens[claims].children) {
      const auto fid = child(child(item, "target"), "field_id");
      if (fid != missing && plugin_fields.count(tokens[fid].decoded)) mark(child(item, "value"), 3);
    }
  };
  metadata(child(0, "asset"));
  for (const auto name : {"files", "groups"}) {
    const auto array = child(0, name);
    if (array != missing) for (auto item : tokens[array].children) {
      metadata(item);
      if (std::string_view(name) == "files") mark(child(child(item, "generation"), "params"), 2);
    }
  }
  const auto entries = child(child(0, "history"), "entries");
  if (entries != missing) for (auto entry : tokens[entries].children) {
    mark(child(entry, "details"), 5);
    const auto changes = child(entry, "changes");
    if (changes != missing) for (auto change : tokens[changes].children)
      for (auto item : tokens[change].children)
        if (tokens[item].edge != "kind" && tokens[item].edge != "target") mark(item, 5);
  }
  for (std::size_t i = 1; i < tokens.size(); ++i)
    if (groups[i] == -1) groups[i] = groups[tokens[i].parent];
  return groups;
}
std::vector<std::size_t> Document::opaque_roots() const {
  std::vector<std::size_t> roots;
  const auto groups = opaque_groups();
  for (std::size_t i = 0; i < tokens.size(); ++i)
    if (groups[i] >= 0 && (tokens[i].parent == missing || groups[tokens[i].parent] < 0)) roots.push_back(i);
  return roots;
}
std::array<std::map<std::string, std::string>, 6> Document::fingerprints(bool stable) const {
  std::array<std::map<std::string, std::string>, 6> result;
  const auto groups = opaque_groups();
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto& t = tokens[i];
    if (groups[i] < 0 || !t.children.empty()) continue;
    const auto body = t.kind == Kind::object ? "{}" : t.kind == Kind::array ? "[]" :
        t.kind == Kind::string ? t.decoded : std::string(text.substr(t.begin, t.end - t.begin));
    result[static_cast<std::size_t>(groups[i])][path(i, true, stable)] = std::to_string(static_cast<int>(t.kind)) + ':' + body;
  }
  return result;
}
std::string Document::shadow() const {
  std::string out(text);
  const auto groups = opaque_groups();
  for (std::size_t i = tokens.size(); i-- > 0;) {
    const auto& t = tokens[i];
    if (groups[i] < 0 || (t.parent != missing && groups[t.parent] >= 0)) continue;
    std::string replacement;
    switch (t.kind) {
      case Kind::object: replacement = "{}"; break;
      case Kind::array: replacement = "[]"; break;
      case Kind::number: replacement = "0"; break;
      case Kind::string: replacement = "\"\""; break;
      case Kind::literal: replacement = text.substr(t.begin, t.end - t.begin); break;
    }
    out.replace(t.begin, t.end - t.begin, replacement);
  }
  return out;
}
std::array<std::map<std::string, std::string_view>, 6> Document::numeric_lexemes(bool stable) const {
  std::array<std::map<std::string, std::string_view>, 6> result;
  const auto groups = opaque_groups();
  for (std::size_t i = 0; i < tokens.size(); ++i) if (groups[i] >= 0 && tokens[i].kind == Kind::number) {
    const auto& t = tokens[i];
    result[static_cast<std::size_t>(groups[i])][path(i, false, stable)] = text.substr(t.begin, t.end - t.begin);
  }
  return result;
}
std::string Document::edit_title(std::string_view title) const {
  auto known = nlohmann::ordered_json::parse(shadow());
  known["asset"]["metadata"]["title"] = title;
  const auto quoted = known["asset"]["metadata"]["title"].dump();
  const auto id = child(child(child(0, "asset"), "metadata"), "title");
  if (id == missing) throw std::runtime_error("fixture title missing");
  std::string out(text);
  out.replace(tokens[id].begin, tokens[id].end - tokens[id].begin, quoted);
  return out;
}
std::string Document::edit_known(bool structural) const {
  if (!structural) return edit_title("S7 edited title");
  auto known = nlohmann::ordered_json::parse(shadow());
  edit_dom(known, true);
  struct Edit { std::size_t begin, end; std::string value; };
  std::vector<Edit> edits;
  const auto asset = child(0, "asset"), meta = child(asset, "metadata");
  auto replace = [&](std::size_t node, std::string value) {
    if (node == missing) throw std::runtime_error("required edit field missing");
    edits.push_back({tokens[node].begin, tokens[node].end, std::move(value)});
  };
  replace(child(meta, "title"), known["asset"]["metadata"]["title"].dump());
  replace(child(asset, "revision"), known["asset"]["revision"].dump());
  replace(child(asset, "modified_at"), known["asset"]["modified_at"].dump());
  const auto values = child(meta, "custom_values");
  if (values == missing || tokens[values].children.size() < 2) throw std::runtime_error("structural fixture lacks preceding core value");
  const auto first = tokens[values].children[0], second = tokens[values].children[1];
  edits.push_back({tokens[first].begin, tokens[second].begin, {}});
  const auto entries = child(child(0, "history"), "entries");
  if (entries == missing) throw std::runtime_error("history missing");
  const auto end = tokens[entries].end - 1;
  edits.push_back({end, end, (tokens[entries].children.empty() ? "" : ",") + known["history"]["entries"].back().dump()});
  std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.begin > b.begin; });
  std::string output(text);
  for (const auto& edit : edits) output.replace(edit.begin, edit.end - edit.begin, edit.value);
  return output;
}
}  // namespace s7
