#include "am/domain/manifest/semantics.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include "am/foundation/paths.hpp"
#include "am/foundation/unicode.hpp"

namespace am::manifest::semantic_detail {
bool uri(std::string_view);
bool spdx(std::string_view);
}  // namespace am::manifest::semantic_detail
namespace am::manifest {
namespace {
using J = nlohmann::ordered_json;
using U = std::uint64_t;
const J& at(const J& j, std::string_view key) {
  static const J empty;
  auto i = j.find(std::string(key));
  return i == j.end() ? empty : *i;
}
std::string_view text(const J& j) {
  return j.is_string() ? std::string_view(j.get_ref<const J::string_t&>()) : std::string_view{};
}
std::string_view str(const J& j, std::string_view key) { return text(at(j, key)); }
U num(const J& j) {
  if (j.is_number_unsigned()) return j.get<U>();
  if (j.is_number_integer() && j.get<std::int64_t>() >= 0) return static_cast<U>(j.get<std::int64_t>());
  return 0;
}
std::string escape(std::string_view s) {
  std::string out;
  for (auto c : s) {
    if (c == '~')
      out += "~0";
    else if (c == '/')
      out += "~1";
    else
      out += c;
  }
  return out;
}
struct Row {
  const J* node;
  std::string pointer;
  const J& j() const { return *node; }
};
std::vector<Row> rows(const J& j, std::string_view key, const std::string& path = "") {
  std::vector<Row> out;
  const auto& a = at(j, key);
  if (a.is_array()) {
    out.reserve(a.size());
    for (std::size_t i = 0; i < a.size(); ++i)
      out.push_back({&a[i], path + "/" + std::string(key) + "/" + std::to_string(i)});
  }
  return out;
}
using Index = std::map<std::string_view, const Row*>;
Index index(const std::vector<Row>& list, std::string_view key = "id") {
  Index out;
  for (const auto& r : list) out.emplace(str(r.j(), key), &r);
  return out;
}
bool decimal(std::string_view s) {
  if (s.starts_with('-')) s.remove_prefix(1);
  if (s.empty()) return false;
  auto dot = s.find('.');
  auto whole = s.substr(0, dot);
  auto digits = [](std::string_view p) {
    return !p.empty() && std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; });
  };
  return digits(whole) && (whole.size() == 1 || whole[0] != '0') && (dot == s.npos || digits(s.substr(dot + 1)));
}
bool integer(std::string_view s) {
  std::int64_t value = 0;
  auto r = std::from_chars(s.data(), s.data() + s.size(), value);
  return decimal(s) && s.find('.') == s.npos && r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
int compare_decimal(std::string_view a, std::string_view b) {
  // Compare canonical decimal strings without narrowing either magnitude.
  auto parts = [](std::string_view s) {
    bool neg = s.starts_with('-');
    if (neg) s.remove_prefix(1);
    auto dot = s.find('.');
    auto whole = s.substr(0, dot), fraction = dot == s.npos ? std::string_view{} : s.substr(dot + 1);
    while (!fraction.empty() && fraction.back() == '0') fraction.remove_suffix(1);
    if (whole == "0" && fraction.empty()) neg = false;
    return std::tuple{neg, whole, fraction};
  };
  auto [an, aw, af] = parts(a);
  auto [bn, bw, bf] = parts(b);
  if (an != bn) return an ? -1 : 1;
  int c = 0;
  if (aw.size() != bw.size())
    c = aw.size() < bw.size() ? -1 : 1;
  else if (aw != bw)
    c = aw < bw ? -1 : 1;
  else
    for (std::size_t i = 0; i < std::max(af.size(), bf.size()); ++i) {
      auto x = i < af.size() ? af[i] : '0', y = i < bf.size() ? bf[i] : '0';
      if (x != y) {
        c = x < y ? -1 : 1;
        break;
      }
    }
  return an ? -c : c;
}
bool calendar(std::string_view s, bool datetime) {
  const auto required = datetime ? 20u : 10u;
  if (s.size() < required || (!datetime && s.size() != 10) || s[4] != '-' || s[7] != '-') return false;
  auto number = [&](std::size_t p, std::size_t n) {
    unsigned out = 0;
    auto r = std::from_chars(s.data() + p, s.data() + p + n, out);
    return r.ec == std::errc{} && r.ptr == s.data() + p + n ? out : 99999u;
  };
  auto y = number(0, 4), m = number(5, 2), d = number(8, 2);
  constexpr unsigned days[]{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y > 9999 || m == 0 || m > 12 || d == 0) return false;
  auto max = days[m] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0) ? 1u : 0u);
  if (d > max) return false;
  if (!datetime) return true;
  if (s[10] != 'T' || s[13] != ':' || s[16] != ':' || s.back() != 'Z' || number(11, 2) > 23 || number(14, 2) > 59 ||
      number(17, 2) > 60)
    return false;
  if (s.size() == 20) return true;
  return s.size() > 21 && s[19] == '.' &&
         std::all_of(s.begin() + 20, s.end() - 1, [](char c) { return c >= '0' && c <= '9'; });
}
int compare_time(std::string_view a, std::string_view b, bool datetime) {
  if (!datetime) return a == b ? 0 : a < b ? -1 : 1;
  auto left = a.substr(0, 19), right = b.substr(0, 19);
  if (left != right) return left < right ? -1 : 1;
  auto fraction = [](std::string_view s) { return s.size() == 20 ? std::string_view{} : s.substr(20, s.size() - 21); };
  auto x = fraction(a), y = fraction(b);
  for (std::size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
    auto p = i < x.size() ? x[i] : '0', q = i < y.size() ? y[i] : '0';
    if (p != q) return p < q ? -1 : 1;
  }
  return 0;
}
struct Owner {
  Row row;
  std::string_view scope, id;
};
struct Scoped {
  Row row;
  std::size_t owner;
};
struct Stop {};
class Validator {
 public:
  const J& root;
  const SemanticOptions& options;
  SemanticReport report;
  bool limited = false;
  std::vector<Row> files, historical, inputs, objects, entries, groups, previews, sources, attachments, fields, history,
      extensions;
  Index file_ids, all_files, input_ids, object_ids, entry_ids, group_ids, preview_ids, source_ids, field_ids;
  std::map<const J*, const J*> input_of;
  std::vector<Owner> owners;
  std::vector<Scoped> values, claims;
  std::map<std::tuple<std::string_view, std::string_view, std::string_view>, const J*> custom;
  std::map<const J*, std::set<std::string_view>> option_ids;
  std::vector<std::set<std::string_view>> author_ids;
  Validator(const J& j, const SemanticOptions& o)
      : root(j),
        options(o),
        files(rows(j, "files")),
        historical(rows(j, "historical_files")),
        inputs(rows(j, "inputs")),
        groups(rows(j, "groups")),
        previews(rows(at(j, "previews"), "items", "/previews")),
        sources(rows(at(j, "origin"), "sources", "/origin")),
        fields(rows(j, "field_definitions")),
        history(rows(at(j, "history"), "entries", "/history")) {
    for (const auto& i : inputs) {
      auto a = rows(i.j(), "objects", i.pointer), b = rows(i.j(), "entries", i.pointer);
      for (const auto& r : a) input_of[r.node] = i.node;
      for (const auto& r : b) input_of[r.node] = i.node;
      objects.insert(objects.end(), a.begin(), a.end());
      entries.insert(entries.end(), b.begin(), b.end());
    }
    const auto& ext = at(j, "extensions");
    if (ext.is_object())
      for (auto i = ext.begin(); i != ext.end(); ++i) {
        Row row{&i.value(), "/extensions/" + escape(i.key())};
        extensions.push_back(row);
        auto list = rows(row.j(), "attachments", row.pointer);
        attachments.insert(attachments.end(), list.begin(), list.end());
        if (i.value().contains("data")) report.has_plugin_data = true;
      }
    // Row vectors are complete before indexes retain pointers into them.
    file_ids = index(files);
    all_files = file_ids;
    auto dead = index(historical);
    all_files.insert(dead.begin(), dead.end());
    input_ids = index(inputs);
    object_ids = index(objects);
    entry_ids = index(entries);
    group_ids = index(groups);
    preview_ids = index(previews);
    source_ids = index(sources);
    field_ids = index(fields);
    auto owner = [&](const Row& r, std::string_view scope, bool source = false) {
      const auto& meta = source ? r.j() : at(r.j(), "metadata");
      if (meta.is_object())
        owners.push_back({{&meta, r.pointer + (source ? "" : "/metadata")}, scope, str(r.j(), "id")});
    };
    owner({&at(j, "asset"), "/asset"}, "asset");
    for (const auto& r : files) owner(r, "file");
    for (const auto& r : groups) owner(r, "group");
    for (const auto& r : sources) owner(r, "source", true);
    author_ids.resize(owners.size());
    for (std::size_t i = 0; i < owners.size(); ++i) {
      for (const auto& a : rows(owners[i].row.j(), "authors", owners[i].row.pointer))
        if (a.j().contains("id")) author_ids[i].insert(str(a.j(), "id"));
      for (const auto& r : rows(owners[i].row.j(), "custom_values", owners[i].row.pointer)) {
        values.push_back({r, i});
        custom.emplace(std::tuple{owners[i].scope, owners[i].id, str(r.j(), "field_id")}, r.node);
      }
      for (const auto& r : rows(owners[i].row.j(), "claims", owners[i].row.pointer)) claims.push_back({r, i});
    }
    for (const auto& f : fields) {
      auto& ids = option_ids[f.node];
      for (const auto& o2 : rows(f.j(), "options", f.pointer)) ids.insert(str(o2.j(), "id"));
      if (str(f.j(), "type").starts_with("plugin:")) report.has_plugin_data = true;
    }
    report.deferred_rules.push_back({"tags_search_key", "O-08"});
  }
  void finding(std::string rule, std::string pointer, std::string detail,
               FindingSeverity severity = FindingSeverity::error) {
    if (report.findings.size() < options.max_findings && !limited) {
      report.findings.push_back({rule, severity, std::move(pointer), "am.manifest." + rule, std::move(detail)});
      return;
    }
    if (!limited) {
      limited = true;
      report.findings.push_back({"limit_exceeded", FindingSeverity::error, std::move(pointer),
                                 "am.manifest.limit_exceeded", "Finding count exceeded hypothesis limit"});
    }
    if (options.stop_after_limit) throw Stop{};
  }
  void expect(bool good, std::string_view rule, const std::string& p, std::string detail = "Semantic rule violated") {
    if (!good) finding(std::string(rule), p, std::move(detail));
  }
  template <class F>
  void stage(std::initializer_list<std::string_view> ids, F&& f) {
    f();
    for (auto id : ids) report.checked_rules.emplace_back(id);
  }
  void unique(const std::vector<Row>& list, std::string_view key, std::string_view rule = "unique_ids") {
    std::set<std::string_view> seen;
    for (const auto& r : list)
      if (r.j().contains(std::string(key)))
        expect(seen.insert(str(r.j(), key)).second, rule, r.pointer + "/" + std::string(key), "Duplicate identifier");
  }
  void sizes(const Row& r, std::string_view name = "size_bytes") {
    const auto& v = at(r.j(), name);
    if (v.is_null()) return;
    auto s = text(v);
    U n = 0;
    auto p = std::from_chars(s.data(), s.data() + s.size(), n);
    expect(p.ec == std::errc{} && p.ptr == s.data() + s.size(), "byte_sizes_uint64",
           r.pointer + "/" + std::string(name), "Byte size exceeds uint64");
  }
  void graph(const std::vector<Row>& list, const Index& ids, std::string_view parent, std::string_view rule) {
    // Functional graph coloring avoids recursion for arbitrarily deep chains.
    std::map<const J*, unsigned> color;
    for (const auto& r : list) {
      std::vector<const Row*> path;
      const Row* current = &r;
      while (current && color[current->node] == 0) {
        color[current->node] = 1;
        path.push_back(current);
        auto p = str(current->j(), parent);
        auto found = ids.find(p);
        current = found == ids.end() ? nullptr : found->second;
      }
      if (current && color[current->node] == 1)
        finding(std::string(rule), current->pointer + "/" + std::string(parent), "Reference cycle");
      for (const auto* p : path) color[p->node] = 2;
    }
  }
  void identities_paths() {
    stage({"unique_ids"}, [&] {
      std::vector<Row> all = files;
      all.insert(all.end(), historical.begin(), historical.end());
      for (const auto* list : {&all, &groups, &previews, &inputs, &objects, &entries, &sources, &attachments, &fields})
        unique(*list, "id");
      std::vector<Row> cs;
      for (const auto& c : claims) cs.push_back(c.row);
      unique(cs, "id");
      for (const auto& o : owners) {
        unique(rows(o.row.j(), "authors", o.row.pointer), "id");
        unique(rows(o.row.j(), "custom_values", o.row.pointer), "field_id");
      }
      for (const auto& r : objects) {
        std::set<U> indices;
        for (const auto& v : rows(r.j(), "volumes", r.pointer))
          expect(indices.insert(num(at(v.j(), "index"))).second, "unique_ids", v.pointer + "/index",
                 "Duplicate volume index");
      }
    });
    stage({"storage_paths"}, [&] {
      std::set<std::string> paths;
      auto check = [&](const Row& r, std::string_view key, std::string_view prefix) {
        auto s = str(r.j(), key);
        bool valid = s.size() <= 200 && paths::check_container_path(s).ok() &&
                     paths.insert(unicode::path_collision_key(s)).second;
        if (key == "storage_path") {
          if (s.starts_with("content/")) {
            auto end = s.find('/', 8);
            valid = valid && end != s.npos && input_ids.contains(s.substr(8, end - 8));
          } else
            valid = valid && s.starts_with("generated/");
        } else
          valid = valid && s.starts_with(prefix);
        expect(valid, "storage_paths", r.pointer + "/" + std::string(key),
               "Unsafe, colliding, or unresolved storage path");
      };
      for (const auto& r : files) check(r, "storage_path", "");
      const auto& ext = at(root, "extensions");
      if (ext.is_object())
        for (auto i = ext.begin(); i != ext.end(); ++i)
          for (const auto& r : rows(i.value(), "attachments", "/extensions/" + escape(i.key())))
            check(r, "path", "extensions/" + i.key() + "/");
    });
    stage({"history_operation_id_unique"}, [&] { unique(history, "operation_id", "history_operation_id_unique"); });
    stage({"byte_sizes_uint64"}, [&] {
      for (const auto* list : {&files, &objects, &entries, &attachments})
        for (const auto& r : *list) sizes(r);
      for (const auto& r : historical) sizes(r, "last_known_size_bytes");
      for (const auto& r : objects)
        for (const auto& v : rows(r.j(), "volumes", r.pointer)) sizes(v);
    });
  }
  void provenance_groups() {
    stage({"historical_files"}, [&] {
      auto rev = num(at(at(root, "asset"), "revision"));
      for (const auto& r : historical) {
        auto n = num(at(r.j(), "removed_at_revision"));
        expect(n >= 2 && n <= rev, "historical_files", r.pointer + "/removed_at_revision");
      }
    });
    stage({"derived_from"}, [&] {
      for (const auto& r : files)
        for (const auto& d : rows(r.j(), "derived_from", r.pointer))
          expect(all_files.contains(str(d.j(), "file_id")), "derived_from", d.pointer + "/file_id");
    });
    stage({"input_source_refs"}, [&] {
      for (const auto& r : inputs) {
        const auto& a = at(r.j(), "source_refs");
        if (a.is_array())
          for (std::size_t i = 0; i < a.size(); ++i)
            expect(source_ids.contains(text(a[i])), "input_source_refs",
                   r.pointer + "/source_refs/" + std::to_string(i));
      }
    });
    stage({"input_entry_object"}, [&] {
      for (const auto& r : entries) {
        auto o = object_ids.find(str(r.j(), "object_id"));
        expect(o != object_ids.end() && input_of[o->second->node] == input_of[r.node], "input_entry_object",
               r.pointer + "/object_id");
      }
    });
    stage({"input_entry_file"}, [&] {
      for (const auto& r : entries) {
        auto kind = str(r.j(), "entry_kind"), result = str(r.j(), "result");
        bool required = kind == "file" && (result == "included" || result == "included_renamed");
        auto id = str(r.j(), "file_id");
        expect(required ? (!id.empty() && all_files.contains(id)) : !r.j().contains("file_id"), "input_entry_file",
               r.pointer + "/file_id");
      }
    });
    stage({"input_directory_consistency"}, [&] {
      std::map<const J*, std::multimap<std::string_view, const Row*>> paths_by_input;
      for (const auto& r : entries) paths_by_input[input_of[r.node]].emplace(str(r.j(), "path"), &r);
      for (const auto& input : paths_by_input) {
        const auto& paths = input.second;
        for (const auto& [path, directory] : paths) {
          if (str(directory->j(), "entry_kind") != "directory" || path.empty() ||
              str(directory->j(), "result") != "excluded")
            continue;
          auto prefix = std::string(path);
          if (!prefix.ends_with('/')) prefix += '/';
          for (auto child = paths.lower_bound(prefix); child != paths.end() && child->first.starts_with(prefix); ++child)
            if (child->first.size() > prefix.size())
              expect(str(child->second->j(), "result") == "excluded", "input_directory_consistency",
                     child->second->pointer + "/result", "Descendant of an excluded directory must be excluded");
        }
        // Directory entries persist no destination prefix. Descendant paths alone
        // do not disambiguate directory renames from independent child renames.
      }
    });
    stage({"input_nesting"}, [&] {
      for (const auto& r : objects) {
        auto po = str(r.j(), "parent_object_id"), pe = str(r.j(), "parent_entry_id");
        if (po.empty() && pe.empty()) continue;
        auto o = object_ids.find(po);
        auto e = entry_ids.find(pe);
        bool valid = !po.empty() && !pe.empty() && o != object_ids.end() && e != entry_ids.end();
        if (valid)
          valid = input_of[o->second->node] == input_of[r.node] && input_of[e->second->node] == input_of[r.node] &&
                  str(e->second->j(), "object_id") == po && str(e->second->j(), "entry_kind") == "file" &&
                  (str(r.j(), "nested_action") != "exclude" || !e->second->j().contains("file_id"));
        expect(valid, "input_nesting", r.pointer + "/parent_object_id");
      }
      graph(objects, object_ids, "parent_object_id", "input_nesting");
    });
    stage({"group_refs"}, [&] {
      for (const auto& r : groups) {
        auto p = str(r.j(), "parent_id");
        if (!p.empty()) expect(group_ids.contains(p), "group_refs", r.pointer + "/parent_id");
        const auto& a = at(r.j(), "file_refs");
        if (a.is_array())
          for (std::size_t i = 0; i < a.size(); ++i)
            expect(file_ids.contains(text(a[i])), "group_refs", r.pointer + "/file_refs/" + std::to_string(i));
      }
      graph(groups, group_ids, "parent_id", "group_refs");
    });
    stage({"preview_refs"}, [&] {
      auto primary = str(at(root, "previews"), "primary_id");
      if (!primary.empty()) expect(preview_ids.contains(primary), "preview_refs", "/previews/primary_id");
      for (const auto& r : previews) {
        expect(file_ids.contains(str(r.j(), "file_id")), "preview_refs", r.pointer + "/file_id");
        const auto& a = at(r.j(), "group_refs");
        if (a.is_array())
          for (std::size_t i = 0; i < a.size(); ++i)
            expect(group_ids.contains(text(a[i])), "preview_refs", r.pointer + "/group_refs/" + std::to_string(i));
      }
    });
  }
  bool physical(const J& field) {
    auto t = str(field, "type"), d = str(field, "dimension");
    return (t == "integer" || t == "decimal") && !d.empty() && d != "dimensionless";
  }
  void date(const J& value, const std::string& p, bool datetime) {
    expect(value.is_string() && calendar(text(value), datetime), "timestamps", p, "Invalid calendar value");
  }
  void url(const J& value, const std::string& p) {
    expect(value.is_string() && semantic_detail::uri(text(value)), "urls", p, "Invalid absolute URI");
  }
  void value(const J& field, const J& v, const std::string& p, std::string_view unit, bool default_value = false,
             bool claim = false) {
    auto type = str(field, "type");
    if (type.starts_with("plugin:")) return;
    const auto repr = default_value ? "field_default" : claim ? "claim_value_type" : "value_representation";
    const auto opt = default_value ? "field_default" : claim ? "claim_value_type" : "value_options";
    bool valid = true;
    auto s = text(v);
    if (type == "integer")
      valid = v.is_string() && integer(s);
    else if (type == "decimal")
      valid = v.is_string() && decimal(s);
    else if (type == "boolean")
      valid = v.is_boolean();
    else if (type == "multi_enum") {
      valid = v.is_array();
      if (valid) {
        std::set<std::string_view> ids;
        for (const auto& i : v)
          if (!i.is_string() || !ids.insert(text(i)).second) valid = false;
      }
    } else
      valid = v.is_string();
    if (!valid) {
      finding(repr, p, "Value does not match field representation");
      return;
    }
    if (type == "date" || type == "datetime") {
      date(v, p, type == "datetime");
      if (!calendar(s, type == "datetime")) return;
    }
    if (type == "url") url(v, p);
    if (type == "enum") expect(option_ids[&field].contains(s), opt, p, "Unknown OptionID");
    if (type == "multi_enum")
      for (std::size_t i = 0; i < v.size(); ++i)
        expect(option_ids[&field].contains(text(v[i])), opt, p + "/" + std::to_string(i), "Unknown OptionID");
    if (type == "file_ref" && !claim)
      expect(file_ids.contains(s), default_value ? "field_default" : "value_file_ref", p,
             "File reference is not current");
    if (physical(field) && !claim) {
      expect(!unit.empty(), default_value ? "field_default" : "value_unit_required", p.substr(0, p.size() - 5) + "unit",
             "Physical value requires a unit");
    }
    if (claim && !physical(field)) return;
    const auto& constraints = at(field, "constraints");
    const auto rule = default_value ? "field_default" : "value_constraints";
    for (const auto* name : {"minimum", "maximum"}) {
      const auto& bound = at(constraints, name);
      if (!bound.is_object()) continue;
      const auto& b = at(bound, "value");
      bool comparable = true;
      int comparison = 0;
      if (type == "integer" || type == "decimal") {
        if (!b.is_string() || !(type == "integer" ? integer(text(b)) : decimal(text(b)))) continue;
        if (physical(field)) {
          auto bu = str(bound, "unit");
          if (unit.empty() || bu.empty()) continue;
          if (unit != bu) {
            finding("x02_unit_conversion_open", p, "Unit conversion is not implemented",
                    FindingSeverity::unverifiable);
            comparable = false;
          }
        }
        comparison = compare_decimal(s, text(b));
      } else if (type == "date" || type == "datetime") {
        if (!calendar(text(b), type == "datetime")) continue;
        comparison = compare_time(s, text(b), type == "datetime");
      } else
        continue;
      if (comparable)
        expect(std::string_view(name) == "minimum" ? comparison >= 0 : comparison <= 0, rule, p,
               "Value outside inclusive bounds");
    }
    if (type == "string" || type == "text") {
      std::size_t count = 0;
      for (auto c : s)
        if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) ++count;
      if (constraints.contains("min_length"))
        expect(count >= num(at(constraints, "min_length")), rule, p, "Value too short");
      if (constraints.contains("max_length"))
        expect(count <= num(at(constraints, "max_length")), rule, p, "Value too long");
    }
    if (type == "multi_enum") {
      if (constraints.contains("min_items"))
        expect(v.size() >= num(at(constraints, "min_items")), rule, p, "Too few options");
      if (constraints.contains("max_items"))
        expect(v.size() <= num(at(constraints, "max_items")), rule, p, "Too many options");
    }
  }
  void field_values() {
    stage({"field_options"}, [&] {
      for (const auto& f : fields)
        if (str(f.j(), "type") == "enum" || str(f.j(), "type") == "multi_enum")
          unique(rows(f.j(), "options", f.pointer), "id", "field_options");
    });
    stage({"field_constraints", "field_default", "custom_value_field", "value_representation", "value_options",
           "value_file_ref", "value_constraints", "value_unit_required", "value_dependencies_scope",
           "x02_unit_conversion_open"},
          [&] {
            for (const auto& f : fields) {
              auto t = str(f.j(), "type");
              if (t.starts_with("plugin:")) continue;
              for (const auto* name : {"minimum", "maximum"}) {
                const auto& b = at(at(f.j(), "constraints"), name);
                if (!b.is_object()) continue;
                auto p = f.pointer + "/constraints/" + name;
                if (t == "integer" || t == "decimal") {
                  expect(at(b, "value").is_string() &&
                             (t == "integer" ? integer(str(b, "value")) : decimal(str(b, "value"))),
                         "field_constraints", p + "/value", "Invalid bound representation");
                  if (physical(f.j()))
                    expect(!str(b, "unit").empty(), "field_constraints", p + "/unit", "Physical bound requires a unit");
                }
                if (t == "date" || t == "datetime") date(at(b, "value"), p + "/value", t == "datetime");
              }
              if (f.j().contains("default")) {
                const auto& d = at(f.j(), "default");
                auto p = f.pointer + "/default";
                if (physical(f.j())) {
                  if (!d.is_object())
                    finding("field_default", p, "Physical default requires value and unit");
                  else
                    value(f.j(), at(d, "value"), p + "/value", str(d, "unit"), true);
                } else
                  value(f.j(), d, p, "", true);
              }
            }
            for (const auto& v : values) {
              auto found = field_ids.find(str(v.row.j(), "field_id"));
              const auto& owner = owners[v.owner];
              bool resolved = found != field_ids.end();
              if (resolved) {
                const auto& scopes = at(found->second->j(), "scopes");
                resolved = scopes.is_array() && std::any_of(scopes.begin(), scopes.end(),
                                                            [&](const J& scope) { return text(scope) == owner.scope; });
              }
              expect(resolved, "custom_value_field", v.row.pointer + "/field_id",
                     "Unknown field or invalid owner scope");
              if (!resolved) continue;
              const auto& f = found->second->j();
              bool plugin = str(f, "type").starts_with("plugin:");
              expect(plugin || !v.row.j().contains("dependencies"), "value_dependencies_scope",
                     v.row.pointer + "/dependencies", "Dependencies require a plugin type");
              value(f, at(v.row.j(), "value"), v.row.pointer + "/value", str(v.row.j(), "unit"));
            }
          });
  }
  void standard_field(std::string_view name, const J& v, const std::string& p) {
    if (name == "publisher" && v.contains("url")) url(at(v, "url"), p + "/url");
    if (name == "license") {
      if (v.contains("file_ref")) expect(file_ids.contains(str(v, "file_ref")), "license_file_ref", p + "/file_ref");
      if (v.contains("spdx"))
        expect(semantic_detail::spdx(str(v, "spdx")), "license_spdx", p + "/spdx", "Invalid SPDX expression");
      if (v.contains("url")) url(at(v, "url"), p + "/url");
    }
    if (name == "links" && v.is_array())
      for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].contains("url")) url(at(v[i], "url"), p + "/" + std::to_string(i) + "/url");
    if (name == "dates")
      for (const auto* key : {"published", "acquired"})
        if (v.contains(key)) date(at(v, key), p + "/" + key, false);
  }
  void standard(const J& meta, const std::string& p) {
    for (const auto* name : {"publisher", "license", "links", "dates"})
      if (meta.contains(name)) standard_field(name, at(meta, name), p + "/" + name);
  }
  void metadata_claims() {
    stage(
        {"claim_target_standard", "claim_target_item", "claim_target_custom", "claim_value_type",
         "claim_value_unit", "claim_evidence", "license_file_ref", "license_spdx", "urls", "timestamps"},
        [&] {
          const auto& asset = at(root, "asset");
          for (const auto* key : {"created_at", "modified_at"})
            if (asset.contains(key)) date(at(asset, key), std::string("/asset/") + key, true);
          for (const auto& r : inputs)
            if (r.j().contains("imported_at")) date(at(r.j(), "imported_at"), r.pointer + "/imported_at", true);
          for (const auto& r : history)
            if (r.j().contains("timestamp")) date(at(r.j(), "timestamp"), r.pointer + "/timestamp", true);
          for (const auto& o : owners) {
            if (o.scope != "source")
              standard(o.row.j(), o.row.pointer);
            else {
              if (o.row.j().contains("url")) url(at(o.row.j(), "url"), o.row.pointer + "/url");
              for (const auto* key : {"published_at", "retrieved_at"})
                if (o.row.j().contains(key)) date(at(o.row.j(), key), o.row.pointer + "/" + key, true);
            }
          }
          for (const auto& c : claims) {
            const auto& j = c.row.j();
            const auto& o = owners[c.owner];
            const auto& target = at(j, "target");
            auto kind = str(target, "kind"), field = str(target, "field"), relation = str(j, "relation");
            bool physical_value = false;
            if (kind == "standard" && relation == "supports_selected")
              expect(o.row.j().contains(std::string(field)), "claim_target_standard", c.row.pointer + "/target/field",
                     "Selected field is absent");
            if (kind == "standard_item")
              expect(author_ids[c.owner].contains(str(target, "item_id")), "claim_target_item",
                     c.row.pointer + "/target/item_id");
            if (kind == "custom") {
              auto f = field_ids.find(str(target, "field_id"));
              bool valid = f != field_ids.end();
              if (valid) {
                const auto& scope = at(f->second->j(), "scopes");
                valid = scope.is_array() &&
                        std::any_of(scope.begin(), scope.end(), [&](const J& item) { return text(item) == o.scope; });
              }
              if (relation == "supports_selected")
                valid = valid && custom.contains({o.scope, o.id, str(target, "field_id")});
              expect(valid, "claim_target_custom", c.row.pointer + "/target/field_id");
              physical_value = valid && o.scope != "source" && physical(f->second->j()) && j.contains("value");
              if (valid && (relation == "alternative" || relation == "rejected"))
                value(f->second->j(), at(j, "value"), c.row.pointer + "/value", str(j, "unit"), false, true);
            }
            expect(physical_value ? !str(j, "unit").empty() : !j.contains("unit"), "claim_value_unit",
                   c.row.pointer + "/unit", "Unit is required only for a physical custom claim value");
            if (kind == "standard" && j.contains("value")) {
              if (field == "url") url(at(j, "value"), c.row.pointer + "/value");
              if (field == "published_at" || field == "retrieved_at")
                date(at(j, "value"), c.row.pointer + "/value", true);
              standard_field(field, at(j, "value"), c.row.pointer + "/value");
            }
            for (const auto& e : rows(j, "evidence", c.row.pointer))
              for (const auto& [name, ids] :
                   std::initializer_list<std::pair<std::string_view, const Index*>>{{"file_id", &all_files},
                                                                                    {"source_entry_id", &entry_ids},
                                                                                    {"input_object_id", &object_ids},
                                                                                    {"source_id", &source_ids}})
                if (e.j().contains(std::string(name)))
                  expect(ids->contains(str(e.j(), name)), "claim_evidence", e.pointer + "/" + std::string(name));
            if (j.contains("created_at")) date(at(j, "created_at"), c.row.pointer + "/created_at", true);
            const auto& review = at(j, "review");
            if (review.contains("at")) date(at(review, "at"), c.row.pointer + "/review/at", true);
          }
        });
  }
  void dependencies() {
    stage({"dependency_refs"}, [&] {
      auto check = [&](const Row& row) {
        const auto& deps = at(row.j(), "dependencies");
        for (const auto& r : rows(deps, "refs", row.pointer + "/dependencies")) {
          auto k = str(r.j(), "kind");
          bool good = true;
          if (k == "file") good = all_files.contains(str(r.j(), "file_id"));
          if (k == "group") good = group_ids.contains(str(r.j(), "group_id"));
          if (k == "field_value")
            good = custom.contains({str(r.j(), "owner_scope"), str(r.j(), "owner_id"), str(r.j(), "field_id")});
          expect(good, "dependency_refs", r.pointer, "Unresolved dependency");
        }
      };
      for (const auto& e : extensions) check(e);
      for (const auto& v : values) check(v.row);
    });
  }
  void histories() {
    stage({"change_kind_fields"}, [&] {
      const std::set<std::string_view> kinds{
          "file_add", "file_remove", "file_replace", "file_move", "metadata_set", "group_add", "group_remove",
          "group_change", "preview_add", "preview_remove", "claim_add", "claim_remove", "claim_change",
          "field_definition_change", "extension_change"};
      auto sha256 = [](std::string_view s) {
        return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
      };
      for (const auto& h : history)
        for (const auto& change : rows(h.j(), "changes", h.pointer)) {
          const auto kind = str(change.j(), "kind");
          if (!kinds.contains(kind)) continue;
          expect(change.j().contains("target"), "change_kind_fields", change.pointer + "/target",
                 "Base change kind requires a target");
          if (kind == "file_replace")
            for (const auto* name : {"old_sha256", "new_sha256"})
              expect(sha256(str(change.j(), name)), "change_kind_fields", change.pointer + "/" + name,
                     "File replacement requires a lowercase SHA-256 hex digest");
          if (kind == "file_move")
            expect(paths::check_container_path(str(change.j(), "old_path")).ok(), "change_kind_fields",
                   change.pointer + "/old_path", "File move requires a safe nonempty original path");
        }
    });
    stage({"asset_times"}, [&] {
      if (!history.empty()) {
        const auto& a = at(root, "asset");
        expect(str(a, "created_at") == str(history.front().j(), "timestamp"), "asset_times", "/asset/created_at");
        expect(str(a, "modified_at") == str(history.back().j(), "timestamp"), "asset_times", "/asset/modified_at");
      }
    });
    stage({"history_coverage"}, [&] {
      // Merge revision intervals; never materialize the revisions in a range.
      std::vector<std::pair<U, U>> spans;
      U previous = 0;
      bool valid = !history.empty();
      auto current = num(at(at(root, "asset"), "revision"));
      for (const auto& r : history) {
        auto n = num(at(r.j(), "revision"));
        valid = valid && n > previous && n <= current;
        previous = n;
        spans.emplace_back(n, n);
      }
      const auto ranges = rows(at(root, "history"), "compacted_ranges", "/history");
      previous = 0;
      for (const auto& r : ranges) {
        auto from = num(at(r.j(), "from_revision")), to = num(at(r.j(), "to_revision"));
        valid = valid && from > previous && from > 1 && to >= from && to < current;
        previous = from;
        spans.emplace_back(from, to);
      }
      if (!history.empty())
        valid = valid && num(at(history.front().j(), "revision")) == 1 &&
                str(history.front().j(), "operation") == "asset_created" &&
                num(at(history.back().j(), "revision")) == current;
      std::sort(spans.begin(), spans.end());
      U next = 1;
      for (auto [from, to] : spans) {
        valid = valid && from == next && to != UINT64_MAX;
        next = to + 1;
      }
      valid = valid && current > 0 && current != UINT64_MAX && next == current + 1;
      expect(valid, "history_coverage", "/history", "Revision coverage is not exact");
    });
    stage({"history_compacted_summary"}, [&] {
      const std::set<std::string> protected_ops{"asset_created", "format_migrated", "provenance_changed",
                                                "history_compacted"};
      for (const auto& r : rows(at(root, "history"), "compacted_ranges", "/history")) {
        auto from = num(at(r.j(), "from_revision")), to = num(at(r.j(), "to_revision"));
        const auto& summary = at(r.j(), "summary");
        bool good = to >= from && to - from != UINT64_MAX && num(at(summary, "revision_count")) == to - from + 1;
        const auto& counts = at(summary, "operation_counts");
        if (counts.is_object()) {
          U sum = 0;
          for (auto i = counts.begin(); i != counts.end(); ++i) {
            auto n = num(i.value());
            if (protected_ops.contains(i.key()) ||
                (i.value().is_number_integer() && i.value().get<std::int64_t>() < 0) || n > UINT64_MAX - sum)
              good = false;
            else
              sum += n;
          }
          good = good && sum == num(at(summary, "revision_count"));
        }
        expect(good, "history_compacted_summary", r.pointer + "/summary");
      }
    });
  }
  SemanticReport run() {
    try {
      identities_paths();
      provenance_groups();
      field_values();
      metadata_claims();
      dependencies();
      histories();
      report.complete = !limited;
    } catch (const Stop&) {
      report.complete = false;
    }
    std::sort(report.checked_rules.begin(), report.checked_rules.end());
    report.checked_rules.erase(std::unique(report.checked_rules.begin(), report.checked_rules.end()),
                               report.checked_rules.end());
    return std::move(report);
  }
};
}  // namespace
Result<SemanticReport> validate_semantics(const ManifestDocument& document, const SemanticOptions& options) {
  try {
    return Validator(document.known(), options).run();
  } catch (const std::bad_alloc&) {
    return Error::make(Errc::resource_exhausted, "Semantic validation allocation failed")
        .in("domain.manifest")
        .id("am.manifest.out_of_memory");
  } catch (const std::exception& e) {
    return Error::make(Errc::internal, "Semantic validator failed", e.what())
        .in("domain.manifest")
        .id("am.manifest.internal");
  } catch (...) {
    return Error::make(Errc::internal, "Semantic validator failed", "Unknown exception")
        .in("domain.manifest")
        .id("am.manifest.internal");
  }
}
}  // namespace am::manifest
