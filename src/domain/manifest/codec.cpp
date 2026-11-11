#include "am/domain/manifest/codec.hpp"
#include "am/foundation/json_text.hpp"
#include "am/foundation/unicode.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace am::manifest {
namespace {
using Json = nlohmann::ordered_json;
using Id = std::size_t;
using Kind = am::json::Kind;
constexpr auto absent = am::json::no_node;
Error error(Errc code, std::string_view id, std::string detail) {
  return Error::make(code, "Manifest codec rejected input or write", std::move(detail))
      .in("domain.manifest").op("manifest.codec").id(std::string(id));
}
[[noreturn]] void fail(Errc code, std::string_view id, std::string detail) { throw error(code, id, std::move(detail)); }
[[noreturn]] void blocked(std::string detail) { fail(Errc::unsupported, "am.manifest.write_blocked", std::move(detail)); }
Error oom() { return error(Errc::resource_exhausted, "am.manifest.out_of_memory", "Allocation failed"); }
am::json::Tree strict(std::string_view text, const CodecLimits& limits) {
  auto result = am::json::parse_strict(text, {limits.max_depth, limits.max_bytes});
  if (!result) {
    const auto& cause = result.error();
    std::string id = cause.message_id;
    if (id.starts_with("am.json.")) id.replace(0, 8, "am.manifest.");
    throw error(cause.code, id, cause.ctx.detail).caused_by(cause);
  }
  return std::move(result).value();
}
std::string part(std::string_view s) {
  std::string out = "/";
  for (char c : s) { if (c == '~') out += "~0"; else if (c == '/') out += "~1"; else out += c; }
  return out;
}
using Key = std::vector<Id>;
// Intern owner components rather than copying a FileID/GroupID scope into every
// descendant key. Verification shares this pool across both texts and consent.
// This also bounds work for syntactically valid inputs with very long IDs.
struct KeyPool {
  std::map<std::string, Id, std::less<>> index;
  std::vector<std::string> values;
  Id intern(std::string_view value) {
    const auto found = index.find(value);
    if (found != index.end()) return found->second;
    const auto id = values.size();
    values.emplace_back(value);
    index.emplace(values.back(), id);
    return id;
  }
  OpaqueKey format(const Key& key) const {
    OpaqueKey out;
    for (auto id : key) out.value += part(values[id]);
    return out;
  }
  Key parse(const OpaqueKey& key) {
    if (key.value.empty() || key.value.front() != '/') blocked("Malformed opaque owner key");
    Key out;
    Id cursor = 1;
    for (;;) {
      const auto end = key.value.find('/', cursor);
      const auto segment = std::string_view(key.value).substr(cursor, end == std::string::npos ? end : end - cursor);
      std::string decoded;
      for (Id i = 0; i < segment.size(); ++i) {
        if (segment[i] != '~') decoded += segment[i];
        else {
          if (++i == segment.size() || (segment[i] != '0' && segment[i] != '1')) blocked("Malformed opaque owner escape");
          decoded += segment[i] == '0' ? '~' : '/';
        }
      }
      out.push_back(intern(decoded));
      if (end == std::string::npos) break;
      cursor = end + 1;
    }
    return out;
  }
};
struct Root { Id node; Key key; };
// One object-member index, one owner index and one root index per text.
// No leaf path causes a rescan of owners or siblings.
struct Analysis {
  std::string_view text;
  std::shared_ptr<KeyPool> pool;
  am::json::Tree tree;
  std::vector<std::vector<Id>> members;
  std::vector<Root> roots;
  std::vector<Id> root_at;
  std::map<Key, Id> owners;
  std::set<std::pair<Key, Id>> identities;
  std::string ambiguity;
  Analysis(std::string_view input, const CodecLimits& limits, std::shared_ptr<KeyPool> shared = std::make_shared<KeyPool>())
      : text(input), pool(std::move(shared)), tree(strict(input, limits)), members(tree.nodes.size()), root_at(tree.nodes.size(), absent) {
    for (Id i = 0; i < tree.nodes.size(); ++i) if (tree.nodes[i].kind == Kind::object) {
      members[i] = tree.nodes[i].children;
      std::sort(members[i].begin(), members[i].end(), [&](Id a, Id b) { return tree.nodes[a].edge < tree.nodes[b].edge; });
    }
    identify();
  }
  Id child(Id node, std::string_view key) const {
    if (node == absent) return absent;
    const auto& list = members[node];
    auto it = std::lower_bound(list.begin(), list.end(), key, [&](Id i, std::string_view k) { return tree.nodes[i].edge < k; });
    return it != list.end() && tree.nodes[*it].edge == key ? *it : absent;
  }
  std::string_view string(Id node) const { return node != absent && tree.nodes[node].kind == Kind::string ? tree.nodes[node].decoded : std::string_view{}; }
  const std::vector<Id>& children(Id node) const {
    static const std::vector<Id> empty;
    return node == absent ? empty : tree.nodes[node].children;
  }
  Key named(std::string_view name) { return {pool->intern(name)}; }
  Key append(Key key, std::string_view segment) { key.push_back(pool->intern(segment)); return key; }
  void identity(Key kind, std::string_view id) {
    if (id.empty() || !identities.emplace(std::move(kind), pool->intern(id)).second)
      if (ambiguity.empty()) ambiguity = "Missing or duplicate owner ID: " + std::string(id);
  }
  void mark(Id node, Key key) {
    if (node == absent) return;
    root_at[node] = roots.size();
    roots.push_back({node, std::move(key)});
    if (!owners.emplace(roots.back().key, node).second && ambiguity.empty())
      ambiguity = "Duplicate opaque owner: " + pool->format(roots.back().key).value;
  }
  void identify() {
    std::map<std::string_view, bool> fields;
    for (Id field : children(child(0, "field_definitions"))) {
      const auto id = string(child(field, "id"));
      identity(named("field"), id);
      const bool plugin = string(child(field, "type")).starts_with("plugin:");
      fields.emplace(id, plugin);
      if (plugin) {
        const auto key = append(named("field_definitions"), id);
        mark(child(field, "default"), append(key, "default")); mark(child(field, "constraints"), append(key, "constraints"));
      }
    }
    auto unknown = [&](std::string_view id) { const auto it = fields.find(id); return it == fields.end() || it->second; };
    auto metadata = [&](Id node, const Key& scope) {
      const auto meta = child(node, "metadata");
      for (Id value : children(child(meta, "custom_values"))) {
        const auto id = string(child(value, "field_id"));
        identity(append(scope, "custom_values"), id);
        if (unknown(id)) mark(child(value, "value"), append(append(append(scope, "custom_values"), id), "value"));
      }
      for (Id claim : children(child(meta, "claims"))) {
        const auto id = string(child(claim, "id"));
        identity(named("claim"), id);
        const auto target = child(claim, "target");
        if (string(child(target, "kind")) == "custom" && unknown(string(child(target, "field_id"))))
          mark(child(claim, "value"), append(append(append(scope, "claims"), id), "value"));
      }
    };
    metadata(child(0, "asset"), named("asset"));
    for (std::string_view array : {"files", "groups"}) for (Id item : children(child(0, array))) {
      const auto id = string(child(item, "id"));
      identity(named(array), id);
      const auto scope = append(named(array), id);
      metadata(item, scope);
      if (array == "files") mark(child(child(item, "generation"), "params"), append(append(scope, "generation"), "params"));
    }
    for (Id extension : children(child(0, "extensions"))) {
      const auto& id = tree.nodes[extension].edge;
      identity(named("extension"), id);
      mark(child(extension, "data"), append(append(named("extensions"), id), "data"));
    }
    for (Id entry : children(child(child(0, "history"), "entries"))) {
      const auto id = string(child(entry, "operation_id"));
      identity(named("operation"), id);
      const auto scope = append(named("history"), id);
      mark(child(entry, "details"), append(scope, "details"));
      for (Id change : children(child(entry, "changes"))) for (Id value : children(change)) {
        const auto& name = tree.nodes[value].edge;
        if (name != "kind" && name != "target")
          mark(value, append(append(append(scope, "changes"), std::to_string(tree.nodes[change].index)), name));
      }
    }
  }
  std::string_view raw(Id id) const { const auto& n = tree.nodes[id]; return text.substr(n.begin, n.end - n.begin); }
};
// Decimal arithmetic on digits only. Saturating exponent parsing is bounded by
// the input length: a larger exponent cannot affect any nonzero in-range integer.
std::string integer(std::string_view lexeme) {
  auto reject = [&]() -> void { fail(Errc::corrupt_data, "am.manifest.known_number_out_of_range", "Known number is not an int64/uint64 value"); };
  if (!am::json::is_json_number(lexeme)) { reject(); }
  const bool negative = lexeme.front() == '-';
  std::size_t p = negative ? 1 : 0, fraction = 0;
  std::string digits;
  bool decimal = false;
  for (; p < lexeme.size() && lexeme[p] != 'e' && lexeme[p] != 'E'; ++p) {
    if (lexeme[p] == '.') decimal = true;
    else { digits += lexeme[p]; if (decimal) ++fraction; }
  }
  const auto first = digits.find_first_not_of('0');
  if (first == std::string::npos) return "0";
  digits.erase(0, first);
  bool exponent_negative = false;
  std::size_t exponent = 0;
  if (p < lexeme.size()) {
    ++p;
    if (lexeme[p] == '+' || lexeme[p] == '-') exponent_negative = lexeme[p++] == '-';
    const auto cap = lexeme.size() + 21;
    for (; p < lexeme.size(); ++p) {
      const auto digit = static_cast<std::size_t>(lexeme[p] - '0');
      if (exponent > (cap - digit) / 10) { exponent = cap; break; }
      exponent = exponent * 10 + digit;
    }
  }
  std::size_t remove = 0, append = 0;
  if (exponent_negative) {
    if (exponent > digits.size() || fraction > digits.size() - exponent) { reject(); }
    remove = fraction + exponent;
  } else if (exponent >= fraction) append = exponent - fraction;
  else remove = fraction - exponent;
  if (remove > digits.size()) { reject(); }
  for (Id i = 0; i < remove; ++i) if (digits[digits.size() - 1 - i] != '0') { reject(); }
  digits.resize(digits.size() - remove);
  if (digits.empty() || digits.size() > 20 || append > 20 - digits.size()) { reject(); }
  digits.append(append, '0');
  const std::string_view bound = negative ? "9223372036854775808" : "18446744073709551615";
  if (digits.size() > bound.size() || (digits.size() == bound.size() && digits > bound)) { reject(); }
  return negative ? '-' + digits : digits;
}
Json handle(Id id) {
  const auto value = static_cast<std::uint64_t>(id);
  std::vector<std::uint8_t> bytes(8);
  for (Id i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
  return Json::binary(std::move(bytes));
}
struct Fragment { std::string text; };
std::string empty_value(Kind kind, std::string_view raw) {
  switch (kind) {
    case Kind::object: return "{}";
    case Kind::array: return "[]";
    case Kind::number: return "0";
    case Kind::string: return "\"\"";
    case Kind::literal: return std::string(raw);
  }
  blocked("Unknown fragment kind");
}
// Streaming traversal with explicit frames; raw subtrees never enter a DOM.
template <class Raw>
std::string emit(const Json& root, const CodecLimits& limits, Raw raw) {
  struct Frame { const Json* node; Json::const_iterator next, end; std::size_t count, depth; };
  std::vector<Frame> stack;
  std::string out;
  auto value = [&](const Json& node, std::size_t depth) {
    if (node.is_binary()) out += raw(node.get_binary(), out.size());
    else if (node.is_object() || node.is_array()) {
      if (depth >= limits.max_known_depth) fail(Errc::resource_exhausted, "am.manifest.limit_exceeded", "Known DOM exceeds hypothesis max_known_depth");
      out += node.is_object() ? '{' : '[';
      stack.push_back({&node, node.begin(), node.end(), 0, depth});
    } else if (node.is_number()) out += integer(node.dump());
    else if (node.is_discarded()) blocked("Discarded DOM node");
    else {
      if (node.is_string() && !unicode::is_valid_utf8(node.get_ref<const Json::string_t&>()))
        fail(Errc::corrupt_data, "am.manifest.invalid_json", "Invalid UTF-8 in known DOM string");
      out += node.dump();
    }
    if (out.size() > limits.max_bytes) fail(Errc::resource_exhausted, "am.manifest.limit_exceeded", "Output exceeds hypothesis max_bytes");
  };
  value(root, 0);
  while (!stack.empty()) {
    auto& frame = stack.back();
    if (frame.next == frame.end) { out += frame.node->is_object() ? '}' : ']'; stack.pop_back(); continue; }
    if (frame.count++) out += ',';
    if (frame.node->is_object()) {
      if (!unicode::is_valid_utf8(frame.next.key()))
        fail(Errc::corrupt_data, "am.manifest.invalid_json", "Invalid UTF-8 in known DOM key");
      out += Json(frame.next.key()).dump(); out += ':';
    }
    const auto* node = &frame.next.value();
    ++frame.next;
    const auto depth = frame.depth + 1;
    value(*node, depth);
  }
  if (out.size() > limits.max_bytes) fail(Errc::resource_exhausted, "am.manifest.limit_exceeded", "Output exceeds hypothesis max_bytes");
  return out;
}
}  // namespace

struct ManifestDocument::Impl {
  std::string original;
  Json dom;
  std::vector<Fragment> fragments;
  CodecLimits limits;
  std::string render(bool shadow, std::vector<OpaqueKey>* keys = nullptr) const {
    std::vector<bool> used(fragments.size(), false);
    std::vector<Id> binary_roots;
    auto text = emit(dom, limits, [&](const Json::binary_t& bytes, Id offset) {
      if (bytes.size() != 8) blocked("Foreign binary size");
      std::uint64_t index = 0;
      for (Id i = 0; i < 8; ++i) index |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
      if (index >= fragments.size() || used[static_cast<Id>(index)]) blocked("Unknown or repeated binary handle");
      const auto id = static_cast<Id>(index);
      used[id] = true;
      const auto& fragment = fragments[id];
      const auto checked = strict(fragment.text, limits);
      binary_roots.push_back(offset + checked.nodes[0].begin);
      return fragment.text;
    });
    const Analysis checked(text, limits);
    if (!checked.ambiguity.empty()) blocked(checked.ambiguity);
    auto roots = checked.roots;
    std::sort(roots.begin(), roots.end(), [&](const Root& a, const Root& b) {
      return checked.tree.nodes[a.node].begin < checked.tree.nodes[b.node].begin;
    });
    std::vector<Id> opaque_roots;
    for (const auto& root : roots) opaque_roots.push_back(checked.tree.nodes[root.node].begin);
    // A valid-looking handle in a known field must never turn that field into
    // an empty shadow value. Conversely opaque roots must use make_opaque.
    if (binary_roots != opaque_roots) blocked("Binary handles must occupy exactly the opaque roots");
    if (keys) {
      for (const auto& [key, node] : checked.owners) { (void)node; keys->push_back(checked.pool->format(key)); }
      std::sort(keys->begin(), keys->end());
    }
    if (!shadow) return text;
    std::string out;
    Id cursor = 0;
    for (const auto& root : roots) {
      const auto& node = checked.tree.nodes[root.node];
      out.append(text, cursor, node.begin - cursor);
      out += empty_value(node.kind, checked.raw(root.node));
      cursor = node.end;
    }
    out.append(text, cursor);
    return out;
  }
};
ManifestDocument::ManifestDocument(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ManifestDocument::ManifestDocument(ManifestDocument&&) noexcept = default;
ManifestDocument& ManifestDocument::operator=(ManifestDocument&&) noexcept = default;
ManifestDocument::~ManifestDocument() = default;
Json& ManifestDocument::known() noexcept { return impl_->dom; }
const Json& ManifestDocument::known() const noexcept { return impl_->dom; }
Result<ManifestDocument> parse_manifest(std::string_view text, const CodecLimits& limits) {
  try {
    Analysis analysis(text, limits);
    auto impl = std::make_unique<ManifestDocument::Impl>();
    impl->original = text; impl->limits = limits;
    // Build JSON for the bounded known part in one forward token pass. Root
    // fragments become null temporarily; their DOM addresses are retained as
    // token IDs and assigned during the subsequent bounded DOM traversal.
    std::string known;
    std::vector<Id> fragment_at(analysis.tree.nodes.size(), absent);
    std::vector<Id> depth(analysis.tree.nodes.size(), 0);
    Id cursor = 0;
    for (Id i = 0; i < analysis.tree.nodes.size();) {
      const auto& node = analysis.tree.nodes[i];
      if (node.parent != absent) depth[i] = depth[node.parent] + 1;
      if (analysis.root_at[i] != absent) {
        known.append(text.substr(cursor, node.begin - cursor));
        known += "null";
        fragment_at[i] = impl->fragments.size();
        impl->fragments.push_back({std::string(analysis.raw(i))});
        cursor = node.end;
        // Preorder node spans allow skipping a raw subtree without recursion.
        ++i;
        while (i < analysis.tree.nodes.size() && analysis.tree.nodes[i].begin < cursor) ++i;
        continue;
      }
      if ((node.kind == Kind::array || node.kind == Kind::object) && depth[i] >= limits.max_known_depth)
        fail(Errc::resource_exhausted, "am.manifest.limit_exceeded", "Known text exceeds hypothesis max_known_depth");
      if (node.kind == Kind::number) {
        known.append(text.substr(cursor, node.begin - cursor)); known += integer(analysis.raw(i)); cursor = node.end;
      }
      ++i;
    }
    known.append(text.substr(cursor));
    impl->dom = Json::parse(known);
    struct Pending { Id node; Json* dom; };
    std::vector<Pending> pending{{0, &impl->dom}};
    while (!pending.empty()) {
      const auto [id, dom] = pending.back(); pending.pop_back();
      if (fragment_at[id] != absent) { *dom = handle(fragment_at[id]); continue; }
      for (Id child : analysis.tree.nodes[id].children) {
        auto* target = analysis.tree.nodes[id].kind == Kind::array ? &(*dom)[analysis.tree.nodes[child].index] : &(*dom)[analysis.tree.nodes[child].edge];
        pending.push_back({child, target});
      }
    }
    return ManifestDocument(std::move(impl));
  } catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return error(Errc::corrupt_data, "am.manifest.invalid_json", e.what()); }
}
Result<Json> ManifestDocument::make_opaque(std::string_view text) {
  try {
    (void)strict(text, impl_->limits);
    const auto id = impl_->fragments.size();
    auto binary = handle(id);
    impl_->fragments.push_back({std::string(text)});
    return binary;
  } catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return error(Errc::unsupported, "am.manifest.write_blocked", e.what()); }
}
Result<std::string> write_manifest(const ManifestDocument& document) {
  try {
    return document.impl_->render(false);
  } catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return error(Errc::unsupported, "am.manifest.write_blocked", e.what()); }
}
Result<std::string> ManifestDocument::shadow() const {
  try { return impl_->render(true); }
  catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return error(Errc::unsupported, "am.manifest.write_blocked", e.what()); }
}
std::vector<OpaqueKey> ManifestDocument::opaque_keys() const {
  try {
    std::vector<OpaqueKey> keys;
    (void)impl_->render(false, &keys);
    return keys;
  } catch (...) { return {}; }
}
Result<void> verify_preserved(std::string_view before, std::string_view after, const IntendedChanges& changes, const CodecLimits& limits) {
  try {
    const auto pool = std::make_shared<KeyPool>();
    const Analysis a(before, limits, pool), b(after, limits, pool);
    if (!a.ambiguity.empty()) blocked(a.ambiguity);
    if (!b.ambiguity.empty()) blocked(b.ambiguity);
    std::set<Key> intended;
    for (const auto& key : changes.owners) intended.insert(pool->parse(key));
    auto mismatch = [&](const Key& key, Id node) -> void {
      // Construct a diagnostic path once, on failure only, including raw depth.
      std::vector<Id> chain;
      for (Id id = node; id != absent; id = a.tree.nodes[id].parent) chain.push_back(id);
      std::string path;
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto& n = a.tree.nodes[*it];
        if (n.parent != absent) path += a.tree.nodes[n.parent].kind == Kind::array ? part("a:" + std::to_string(n.index)) : part("o:" + n.edge);
      }
      fail(Errc::internal, "am.manifest.preservation_failed", "owner=" + pool->format(key).value + " path=" + path);
    };
    for (const auto& [key, root] : a.owners) {
      if (intended.contains(key)) continue;
      const auto found = b.owners.find(key);
      if (found == b.owners.end()) mismatch(key, root);
      std::vector<std::pair<Id, Id>> pending{{root, found->second}};
      while (!pending.empty()) {
        const auto [x, y] = pending.back(); pending.pop_back();
        const auto& left = a.tree.nodes[x]; const auto& right = b.tree.nodes[y];
        if (left.kind != right.kind || left.children.size() != right.children.size()) mismatch(key, x);
        if (left.kind == Kind::string) { if (left.decoded != right.decoded) mismatch(key, x); }
        else if (left.kind == Kind::number || left.kind == Kind::literal) { if (a.raw(x) != b.raw(y)) mismatch(key, x); }
        else if (left.kind == Kind::object) {
          for (Id i = 0; i < a.members[x].size(); ++i) {
            const auto l = a.members[x][i], r = b.members[y][i];
            if (a.tree.nodes[l].edge != b.tree.nodes[r].edge) mismatch(key, l);
            pending.emplace_back(l, r);
          }
        } else for (Id i = 0; i < left.children.size(); ++i) pending.emplace_back(left.children[i], right.children[i]);
      }
    }
    for (const auto& [key, node] : b.owners) {
      (void)node;
      if (!a.owners.contains(key) && !intended.contains(key))
        fail(Errc::internal, "am.manifest.preservation_failed", "Unexpected opaque owner=" + pool->format(key).value);
    }
    return {};
  } catch (const Error& e) { return e; }
  catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return error(Errc::internal, "am.manifest.preservation_failed", e.what()); }
}
}  // namespace am::manifest
