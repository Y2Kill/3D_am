#include "am/foundation/log.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/services/catalog/verify_job.hpp"
#include "am/storage/db/job_store.hpp"
#include "am/storage/db/operation_log.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "am/foundation/json_text.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

namespace am::catalog {
namespace {
namespace fs = std::filesystem;
using Json = repository::Json;
Error error(Errc c, std::string id, std::string detail = {}) {
  return Error::make(c, "Catalog operation failed", std::move(detail)).in("services.catalog").id(std::move(id));
}
Error warning_with_cause(std::string id, const Error& cause, std::string detail = {}, Errc code = Errc::io_error) {
  auto warning = error(code, std::move(id), std::move(detail))
                     .with_severity(Severity::warning).caused_by(cause);
  if (cause.root_cause().code == Errc::insufficient_space)
    warning.actions = {ErrorAction::free_space, ErrorAction::abort};
  return warning;
}
template <class T>
T take(Result<T> r) {
  if (!r) throw r.error();
  return std::move(r).value();
}
void take(Result<void> r) {
  if (!r) throw r.error();
}
template <class T, class F>
Result<T> guard(F&& f) {
  try {
    return f();
  } catch (const Error& e) {
    return e;
  } catch (const std::bad_alloc&) {
    return error(Errc::resource_exhausted, "am.catalog.out_of_memory");
  } catch (const std::exception& e) {
    return error(Errc::io_error, "am.catalog.error", e.what());
  } catch (...) {
    return error(Errc::internal, "am.catalog.error");
  }
}
const Json& member(const Json& j, std::string_view key) {
  static const Json empty;
  if (!j.is_object()) return empty;
  const auto i = j.find(std::string(key));
  return i == j.end() ? empty : *i;
}
const Json& array(const Json& j, std::string_view key) {
  static const Json empty = Json::array();
  const auto& a = member(j, key);
  return a.is_array() ? a : empty;
}
std::optional<std::string> text(const Json& j, std::string_view key) {
  const auto& s = member(j, key);
  return s.is_string() ? std::optional(s.get<std::string>()) : std::nullopt;
}
std::string string(const Json& j, std::string_view key) { return text(j, key).value_or(""); }
std::int64_t integer(const Json& j, std::string_view key, std::int64_t fallback = 0) {
  const auto& n = member(j, key);
  if (n.is_number_unsigned() && n.get<std::uint64_t>() <= std::uint64_t(INT64_MAX))
    return static_cast<std::int64_t>(n.get<std::uint64_t>());
  return n.is_number_integer() && !n.is_number_unsigned() ? n.get<std::int64_t>() : fallback;
}
std::optional<std::uint64_t> size_value(const std::string& s) {
  if (s.empty() || (s.size() > 1 && s[0] == '0')) return {};
  std::uint64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return {};
  return v;
}
std::optional<std::string> size_text(const Json& j) {
  auto s = text(j, "size_bytes");
  return s && size_value(*s) ? s : std::nullopt;
}
std::string basename(std::string s) {
  const auto p = s.find_last_of("/\\");
  return p == std::string::npos ? s : s.substr(p + 1);
}
std::string utf8(const fs::path& p) {
  const auto s = p.generic_u8string();
  return {s.begin(), s.end()};
}
std::int64_t modified_time(const fs::path& path) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(paths::native(path).c_str(), GetFileExInfoStandard, &data))
    throw error(Errc::io_error, "am.catalog.error", "File stat failed");
  const auto ticks = (std::uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
  return (static_cast<std::int64_t>(ticks) - 116444736000000000LL) * 100;
#else
  const auto time = fs::file_time_type::clock::to_sys(fs::last_write_time(path));
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
#endif
}
std::string utc() {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}
std::string sha256(std::string_view s) {
  auto hash = take(asset_zip::Sha256::create());
  take(hash.update(std::as_bytes(std::span(s))));
  const auto bytes = take(hash.finish());
  std::string out;
  constexpr char hex[] = "0123456789abcdef";
  for (auto b : bytes) {
    const auto v = std::to_integer<unsigned>(b);
    out += hex[v >> 4];
    out += hex[v & 15];
  }
  return out;
}
void append(std::string& to, std::string_view s) {
  if (s.empty()) return;
  if (!to.empty()) to += ' ';
  to += s;
}
struct Raw {
  std::string_view source;
  json::Tree tree;
  std::size_t member(std::size_t n, std::string_view key) const {
    if (n == json::no_node) return n;
    for (auto c : tree.nodes[n].children)
      if (tree.nodes[c].edge == key) return c;
    return json::no_node;
  }
  std::size_t at(std::size_t n, std::size_t index) const {
    return n != json::no_node && index < tree.nodes[n].children.size() ? tree.nodes[n].children[index] : json::no_node;
  }
  std::string value(std::size_t n, bool decoded) const {
    if (n == json::no_node) throw error(Errc::corrupt_data, "am.catalog.manifest_invalid", "Missing raw field value");
    const auto& v = tree.nodes[n];
    return decoded && v.kind == json::Kind::string ? v.decoded : std::string(source.substr(v.begin, v.end - v.begin));
  }
};
void writable(const library::Library& l) {
  if (l.mode() != library::Mode::read_write) throw error(Errc::read_only, "am.catalog.read_only");
}
struct Candidate {
  fs::path path;
  std::string relative, id;
  bool trash = false;
};
struct Candidates {
  std::vector<Candidate> winners;
  std::vector<std::string> ignored;
  std::vector<SkippedAsset> duplicates;
};
Candidates list_candidates(const fs::path& root, bool include_trash = true) {
  Candidates out;
  std::vector<Candidate> all;
  auto consider = [&](const fs::directory_entry& entry, const std::string& relative, bool trash) {
    const auto filename = utf8(entry.path().filename());
    const auto stem = filename.ends_with(".zip") ? filename.substr(0, filename.size() - 4) : std::string{};
    const auto id = Uuid::parse_canonical(stem);
    if (!entry.is_regular_file() || entry.is_symlink() || !id || id->version() != 4 ||
        (id->bytes()[8] & 0xc0) != 0x80) {
      out.ignored.push_back(relative);
      return;
    }
    all.push_back({entry.path(), relative, stem, trash});
  };
  // Enumerate both complete roots before a write transaction. Failure is not
  // an empty library and must never cause missing marks.
  for (const auto& shard : fs::directory_iterator(paths::native(root / "assets"))) {
    const auto relative = "assets/" + utf8(shard.path().filename());
    if (!shard.is_directory() || shard.is_symlink()) {
      out.ignored.push_back(relative);
      continue;
    }
    for (const auto& entry : fs::directory_iterator(shard.path()))
      consider(entry, relative + "/" + utf8(entry.path().filename()), false);
  }
  if (include_trash)
    for (const auto& entry : fs::directory_iterator(paths::native(root / "trash")))
      consider(entry, "trash/" + utf8(entry.path().filename()), true);
  std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.relative < b.relative; });
  std::set<std::string> seen;
  for (auto& candidate : all) {
    if (seen.insert(candidate.id).second)
      out.winners.push_back(std::move(candidate));
    else
      out.duplicates.push_back({candidate.relative, "duplicate_id", "am.catalog.duplicate_id"});
  }
  std::sort(out.ignored.begin(), out.ignored.end());
  return out;
}
Location stat_location(const Candidate& c) {
  return {c.relative, std::to_string(fs::file_size(paths::native(c.path))), modified_time(c.path), c.trash};
}
SkippedAsset skipped(const std::string& path, const Error& e) {
  const auto reason = e.message_id == "am.catalog.id_mismatch" ? "id_mismatch"
                      : e.code == Errc::unsupported            ? "unsupported"
                                                               : "unreadable";
  return {path, reason, e.message_id};
}
void sort_skipped(std::vector<SkippedAsset>& skipped) {
  std::sort(skipped.begin(), skipped.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
}
bool validated(const repository::VerificationReport& report) {
  switch (report.v1) {
    case repository::CheckStatus::passed:
      break;
    case repository::CheckStatus::not_performed:
    case repository::CheckStatus::partial:
    case repository::CheckStatus::failed:
    case repository::CheckStatus::not_applicable_newer_minor:
      return false;
    default:
      return false;
  }
  switch (report.v2) {
    case repository::CheckStatus::passed:
      return true;
    case repository::CheckStatus::not_applicable_newer_minor:
      return report.format_support == repository::FormatSupport::newer_minor;
    case repository::CheckStatus::not_performed:
    case repository::CheckStatus::partial:
    case repository::CheckStatus::failed:
      return false;
    default:
      return false;
  }
}
}  // namespace
std::string tag_search_key(std::string_view s) {
  // O-08, identity until decided.
  return std::string(s);
}
Result<db::IndexRecord> Indexer::map(const manifest::ManifestDocument& doc, std::string_view original,
                                     const Location& location, const repository::VerificationReport& report) {
  return guard<db::IndexRecord>([&]() -> Result<db::IndexRecord> {
    if (!validated(report)) {
      if (report.error) return *report.error;
      return error(Errc::unsupported, "am.catalog.schema_unverified", "Indexing requires passed V1 and V2");
    }
    const bool newer = report.format_support == repository::FormatSupport::newer_minor;
    const auto& j = doc.known();
    const auto& asset = member(j, "asset");
    const auto& m = member(asset, "metadata");
    const auto& version = member(member(j, "format"), "version");
    if (newer && (!member(asset, "revision").is_number_integer() || !member(version, "major").is_number_integer() ||
                  !member(version, "minor").is_number_integer() || !member(j, "files").is_array() ||
                  integer(version, "major", -1) < 0 || integer(version, "minor", -1) < 0))
      return error(Errc::corrupt_data, "am.catalog.manifest_invalid",
                   "Invalid required field type in newer minor manifest");
    db::IndexRecord r;
    auto& a = r.summary;
    a.asset_id = string(asset, "id");
    a.revision = integer(asset, "revision");
    r.created_at = string(asset, "created_at");
    r.modified_at = string(asset, "modified_at");
    if (a.asset_id.empty() || a.revision < 1 || r.created_at.empty() || r.modified_at.empty())
      return error(Errc::corrupt_data, "am.catalog.manifest_invalid", "Required asset fields are unavailable");
    a.in_trash = location.in_trash;
    r.rel_path = location.rel_path;
    r.zip_size = location.zip_size;
    r.zip_mtime_ns = location.zip_mtime_ns;
    r.manifest_json = original;
    r.manifest_sha256 = sha256(original);
    r.format_major = integer(version, "major");
    r.format_minor = integer(version, "minor");
    a.title = text(m, "title");
    r.description = text(m, "description");
    a.health = "ok";
    if (report.error) r.health_detail = report.error->message_id;
    const bool corrupt = !newer && report.v3 == repository::CheckStatus::failed;
    const bool no_children = corrupt || newer;
    if (corrupt)
      a.health = "corrupt";
    else if (report.format_support == repository::FormatSupport::newer_minor ||
             report.v3 != repository::CheckStatus::passed)
      a.health = "warn";
    if (corrupt && !report.v3_findings.empty()) r.health_detail = report.v3_findings.front().message_id;
    for (const auto& f : report.v3_findings) {
      if (!r.health_detail) r.health_detail = f.message_id;
      if (a.health == "ok") a.health = "warn";
    }
    if (!no_children && report.error && report.error->code == Errc::unsupported) a.health = "unsupported";
    if (newer) {
      a.health = "warn";
      r.health_detail = "am.catalog.newer_minor";
    }
    std::set<std::string> formats, names, tags, categories;
    std::uint64_t total = 0;
    bool overflow = false, unknown = false;
    const auto& files = array(j, "files");
    a.file_count = static_cast<std::int64_t>(files.size());
    for (const auto& f : files) {
      const auto type = text(member(f, "type"), "id");
      if (type) formats.insert(*type);
      const auto name = basename(string(f, "storage_path"));
      if (!name.empty()) names.insert(name);
      const auto size = size_text(f);
      if (!size) {
        unknown = true;
        continue;
      }
      const auto n = *size_value(*size);
      if (n > UINT64_MAX - total)
        overflow = true;
      else if (!overflow)
        total += n;
      if (no_children) continue;
      db::IndexedFile file{string(f, "id"),     string(f, "storage_path"), basename(string(f, "storage_path")), *size,
                           string(f, "sha256"), string(f, "origin_kind"),  text(member(f, "type"), "id")};
      if (file.id.empty() || file.storage_path.empty()) continue;
      r.files.push_back(std::move(file));
    }
    if (!overflow && !unknown) a.content_bytes = std::to_string(total);
    if (overflow) {
      if (a.health == "ok") a.health = "warn";
      if (!newer)
        r.health_detail = r.health_detail ? *r.health_detail + ";content_bytes_overflow" : "content_bytes_overflow";
    }
    r.formats = Json(std::vector<std::string>(formats.begin(), formats.end())).dump();
    const auto& previews = member(j, "previews");
    if (!no_children) {
      for (const auto& p : array(previews, "items"))
        if (string(p, "id") == string(previews, "primary_id")) r.primary_preview = text(p, "file_id");
    }
    for (const auto& input : array(j, "inputs")) {
      const auto completeness = string(input, "completeness");
      if (completeness == "incomplete" || completeness == "unknown") r.incomplete = true;
      if (no_children) continue;
      const auto input_id = string(input, "id");
      for (const auto& e : array(input, "entries")) {
        db::IndexedEntry entry{string(e, "id"),
                               input_id,
                               string(e, "object_id"),
                               string(e, "path"),
                               basename(string(e, "path")),
                               string(e, "entry_kind"),
                               string(e, "result"),
                               size_text(e),
                               text(e, "crc32"),
                               text(e, "file_id")};
        names.insert(entry.name);
        r.entries.push_back(std::move(entry));
      }
      for (const auto& o : array(input, "objects")) {
        if (string(o, "kind") == "multivolume_archive") {
          for (const auto& v : array(o, "volumes")) {
            db::IndexedObject object{
                string(o, "id"),         input_id,     string(o, "kind"), basename(string(v, "name")),
                integer(v, "index", -1), size_text(v), text(v, "sha256")};
            names.insert(object.name);
            r.objects.push_back(std::move(object));
          }
        } else {
          db::IndexedObject object{
              string(o, "id"), input_id,         string(o, "kind"), basename(string(o, "name")), -1,
              size_text(o),    text(o, "sha256")};
          names.insert(object.name);
          r.objects.push_back(std::move(object));
        }
      }
    }
    for (const auto& s : array(member(j, "origin"), "sources")) {
      if (!no_children) r.sources.push_back({string(s, "id"), string(s, "kind"), text(s, "name"), text(s, "url")});
      append(r.sources_text, string(s, "name"));
    }
    for (const auto& author : array(m, "authors")) append(r.authors_text, string(author, "name"));
    for (const auto& tag : array(m, "tags"))
      if (tag.is_string()) tags.insert(tag_search_key(tag.get_ref<const std::string&>()));
    for (const auto& t : tags) append(r.tags_text, t);
    // Violated or unverified graphs are not projected into keyed children.
    // The original manifest and safe summary/FTS fields remain available.
    if (no_children) return r;
    for (const auto& category : array(m, "categories"))
      if (member(category, "path").is_array()) categories.insert(member(category, "path").dump());
    r.tags.assign(tags.begin(), tags.end());
    r.categories.assign(categories.begin(), categories.end());
    r.names.assign(names.begin(), names.end());
    Raw raw{original, take(json::parse_strict(original))};
    std::map<std::string, std::string> types;
    for (const auto& d : array(j, "field_definitions")) types.emplace(string(d, "id"), string(d, "type"));
    auto fields = [&](const Json& metadata, std::size_t raw_metadata, const std::string& scope,
                      const std::string& owner) {
      const auto& values = array(metadata, "custom_values");
      for (std::size_t i = 0; i < values.size(); ++i) {
        const auto& v = values[i];
        db::IndexedField field;
        field.scope = scope;
        field.owner_id = owner;
        field.field_id = string(v, "field_id");
        field.unit = text(v, "unit");
        const auto type = types.find(field.field_id);
        if (type != types.end()) field.type_id = type->second;
        const bool plugin = field.type_id && field.type_id->starts_with("plugin:");
        const auto node = raw.member(raw.at(raw.member(raw_metadata, "custom_values"), i), "value");
        field.txt = raw.value(node, !plugin);
        if (field.type_id && (*field.type_id == "integer" || *field.type_id == "decimal")) {
          double number = 0;
          const auto& s = *field.txt;
          const auto converted = std::from_chars(s.data(), s.data() + s.size(), number);
          if (converted.ec == std::errc{} && converted.ptr == s.data() + s.size() && std::isfinite(number))
            field.num = number;
        }
        r.fields.push_back(std::move(field));
      }
    };
    fields(m, raw.member(raw.member(0, "asset"), "metadata"), "asset", a.asset_id);
    for (const auto* key : {"groups", "files"}) {
      const auto& owners = array(j, key);
      for (std::size_t i = 0; i < owners.size(); ++i)
        fields(member(owners[i], "metadata"), raw.member(raw.at(raw.member(0, key), i), "metadata"),
               std::string(key) == "files" ? "file" : "group", string(owners[i], "id"));
    }
    return r;
  });
}
Indexer::Indexer(fs::path root, library::Library& library, IndexerHooks hooks) noexcept
    : root_(std::move(root)),
      library_(library),
      store_(library.database(library::DatabaseKind::index)),
      hooks_(std::move(hooks)) {}
Result<db::IndexRecord> Indexer::read(const fs::path& path, const std::string& relative, bool trash) const {
  return guard<db::IndexRecord>([&]() -> Result<db::IndexRecord> {
    if (hooks_.before_read) take(hooks_.before_read(path));
    repository::ContainerVerifyOptions options;
    options.streams = false;
    repository::ContainerContents contents;
    const auto report = take(repository::verify_container(path, options, &contents));
    if (!contents.document) return report.error.value_or(error(Errc::corrupt_data, "am.catalog.manifest_invalid"));
    Location location{relative, std::to_string(fs::file_size(paths::native(path))), modified_time(path), trash};
    auto record = take(map(*contents.document, contents.manifest_text, location, report));
    if (record.summary.asset_id != utf8(path.stem()))
      return error(Errc::corrupt_data, "am.catalog.id_mismatch", relative);
    return record;
  });
}
Result<RebuildReport> Indexer::rebuild() {
  return guard<RebuildReport>([&]() -> Result<RebuildReport> {
    writable(library_);
    const auto start = std::chrono::steady_clock::now();
    RebuildReport out;
    auto candidates = list_candidates(root_);
    out.ignored = std::move(candidates.ignored);
    out.skipped = std::move(candidates.duplicates);
    auto transaction = take(store_.begin_rebuild());
    for (const auto& c : candidates.winners) {
      auto record = read(c.path, c.relative, c.trash);
      if (!record) {
        if (record.error().code == Errc::resource_exhausted || record.error().code == Errc::internal)
          throw record.error();
        out.skipped.push_back(skipped(c.relative, record.error()));
        continue;
      }
      take(transaction.insert(*record));
      ++out.indexed;
      if (hooks_.after_rebuild_insert) take(hooks_.after_rebuild_insert(out.indexed));
    }
    take(transaction.commit(utc()));
    sort_skipped(out.skipped);
    out.duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    return out;
  });
}
Result<SyncReport> Indexer::sync(const SyncOptions& options) {
  return guard<SyncReport>([&]() -> Result<SyncReport> {
    writable(library_);
    if (take(store_.rebuild_required())) return error(Errc::incomplete, "am.catalog.rebuild_required");
    const auto start = std::chrono::steady_clock::now();
    auto candidates = list_candidates(root_);
    SyncReport out;
    out.full = options.full;
    out.ignored = std::move(candidates.ignored);
    out.skipped = std::move(candidates.duplicates);
    auto transaction = take(store_.begin_sync());
    std::map<std::string, db::IndexState> previous;
    for (auto& row : take(store_.sync_state())) previous.emplace(row.asset_id, std::move(row));
    std::set<std::string> present;
    std::size_t changes = 0;
    auto changed = [&]() {
      ++changes;
      if (hooks_.after_sync_change) take(hooks_.after_sync_change(changes));
    };
    for (const auto& c : candidates.winners) {
      present.insert(c.id);
      const auto observed = stat_location(c);
      const auto old = previous.find(c.id);
      if (!options.full && old != previous.end() && old->second.health != "missing" &&
          old->second.rel_path == observed.rel_path && old->second.zip_size == observed.zip_size &&
          old->second.zip_mtime_ns == observed.zip_mtime_ns) {
        ++out.unchanged;
        continue;
      }
      auto record = read(c.path, c.relative, c.trash);
      if (!record) {
        if (record.error().code == Errc::resource_exhausted || record.error().code == Errc::internal)
          throw record.error();
        out.skipped.push_back(skipped(c.relative, record.error()));
        if (old != previous.end()) {
          take(transaction.mark({c.id, record.error().code == Errc::unsupported ? "unsupported" : "corrupt",
                                 record.error().message_id, db::IndexStat{observed.zip_size, observed.zip_mtime_ns}}));
          ++out.updated;
          changed();
        }
        continue;
      }
      take(transaction.insert(*record));
      if (old == previous.end())
        ++out.added;
      else
        ++out.updated;
      changed();
    }
    for (const auto& [id, row] : previous)
      if (!present.contains(id) && row.health != "missing") {
        take(transaction.mark({id, "missing", "am.catalog.asset_missing", std::nullopt}));
        out.missing.push_back(id);
        changed();
      }
    take(transaction.commit());
    sort_skipped(out.skipped);
    out.duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    return out;
  });
}
Result<void> Indexer::update(const repository::AssetRef& ref, bool in_trash) {
  return guard<void>([&]() -> Result<void> {
    writable(library_);
    if (ref.library_id != library_.config().library_id)
      return error(Errc::invalid_argument, "am.catalog.manifest_invalid", "Library ID mismatch");
    const auto id = Uuid::parse_canonical(ref.asset_id);
    if (!id || id->version() != 4 || (id->bytes()[8] & 0xc0) != 0x80)
      return error(Errc::invalid_argument, "am.catalog.manifest_invalid", "Asset ID is not UUIDv4");
    if (hooks_.before_update) take(hooks_.before_update(ref.asset_id));
    const auto relative = (in_trash ? "trash/" : "assets/" + ref.asset_id.substr(34) + "/") + ref.asset_id + ".zip";
    auto record = take(read(root_ / fs::path(std::u8string(relative.begin(), relative.end())), relative, in_trash));
    if (record.summary.asset_id != ref.asset_id) return error(Errc::corrupt_data, "am.catalog.id_mismatch", relative);
    take(store_.replace(record));
    return {};
  });
}
Result<void> Indexer::remove(std::string_view id) {
  return guard<void>([&]() -> Result<void> {
    writable(library_);
    return store_.remove(id);
  });
}
Result<void> Indexer::refresh(std::string_view id) {
  return guard<void>([&]() -> Result<void> {
    writable(library_);
    const repository::AssetRef ref{library_.config().library_id,std::string(id)};
    const auto uuid = Uuid::parse_canonical(id);
    if (!uuid || uuid->version() != 4 || (uuid->bytes()[8] & 0xc0) != 0x80)
      return error(Errc::invalid_argument,"am.catalog.manifest_invalid","Asset ID is not UUIDv4");
    if (fs::exists(paths::native(root_/"assets"/std::string(id.substr(34))/(std::string(id)+".zip")))) return update(ref,false);
    if (fs::exists(paths::native(root_/"trash"/(std::string(id)+".zip")))) return update(ref,true);
    return remove(id);
  });
}
Result<LibraryVerifyReport> Indexer::verify(const LibraryVerifyOptions& options) {
  return guard<LibraryVerifyReport>([&]() -> Result<LibraryVerifyReport> {
    const auto start = std::chrono::steady_clock::now();
    auto candidates = list_candidates(root_,options.include_trash);
    LibraryVerifyReport out; out.level = options.level; out.total = candidates.winners.size();
    out.skipped = std::move(candidates.duplicates); out.ignored = std::move(candidates.ignored);
    bool database_index_ok = true;
    for (const auto kind : {library::DatabaseKind::index, library::DatabaseKind::userdata, library::DatabaseKind::state}) {
      auto check = take(library_.check_database(kind, options.level == LibraryVerifyLevel::full
          ? library::DatabaseCheckLevel::full : library::DatabaseCheckLevel::quick));
      if (check.status != "ok") {
        if (kind == library::DatabaseKind::index) database_index_ok = false;
        else out.database_failed = true;
      }
      out.databases.push_back(std::move(check));
    }
    std::map<std::string,db::IndexState> previous;
    bool index_ready = library_.database_info(library::DatabaseKind::index).state == library::DatabaseState::ok &&
                       library_.database(library::DatabaseKind::index);
    index_ready = index_ready && database_index_ok;
    if (index_ready) {
      auto rows = store_.sync_state();
      if (rows) for (auto& row : *rows) previous.emplace(row.asset_id,std::move(row));
      else index_ready = false;
    }
    if (index_ready) {
      auto required = store_.rebuild_required(); index_ready = required && !*required;
    }
    if (!index_ready) out.warnings.push_back(error(Errc::incomplete,"am.catalog.rebuild_required").with_severity(Severity::warning));
    struct Repair { std::string id; std::optional<db::IndexRecord> record; std::optional<db::IndexMark> mark; };
    std::vector<Repair> repairs;
    if (options.progress) options.progress(0,out.total);
    for (const auto& c : candidates.winners) {
      if (options.cancelled && options.cancelled()) { out.cancelled = true; break; }
      LibraryVerifyItem item; item.asset_id = c.id; item.path = c.relative; item.in_trash = c.trash;
      item.indexed = previous.contains(c.id);
      std::optional<db::IndexStat> observed;
      if (index_ready && item.indexed && library_.mode() == library::Mode::read_write) {
        auto location = guard<Location>([&]() -> Result<Location> { return stat_location(c); });
        if (location) observed = db::IndexStat{location->zip_size,location->zip_mtime_ns};
      }
      repository::ContainerVerifyOptions check;
      check.streams = options.level == LibraryVerifyLevel::full; check.level = repository::VerifyLevel::full;
      repository::ContainerContents contents;
      auto verified = repository::verify_container(c.path,check,&contents);
      if (!verified) {
        if (verified.error().code == Errc::resource_exhausted || verified.error().code == Errc::internal) throw verified.error();
        item.status = verified.error().code == Errc::unsupported ? "unsupported" :
                      verified.error().code == Errc::corrupt_data ? "corrupt" : "unreadable";
        item.message_id = verified.error().message_id;
      } else {
        const auto& report = *verified;
        if (contents.document) {
          auto mapped = map(*contents.document,contents.manifest_text,Location{c.relative,"0",0,c.trash},report);
          if (mapped) { item.status = mapped->summary.health; item.message_id = mapped->health_detail.value_or(""); }
          else { item.status = mapped.error().code == Errc::unsupported ? "unsupported" : "corrupt"; item.message_id = mapped.error().message_id; }
          if (string(member(contents.document->known(),"asset"),"id") != c.id) {
            item.status = "corrupt"; item.message_id = "am.catalog.id_mismatch"; item.pointer = "/asset/id";
          }
        } else item.status = report.error && report.error->code == Errc::unsupported ? "unsupported" : "corrupt";
        if (report.error) {
          if (item.message_id.empty()) item.message_id = report.error->message_id;
          if (report.error->code == Errc::io_error || report.error->code == Errc::not_found) item.status = "unreadable";
        }
        for (const auto& finding : report.v3_findings)
          if (!item.pointer && finding.message_id == item.message_id && !finding.pointer.empty()) item.pointer = finding.pointer;
        for (const auto& stream : report.v4)
          if (stream.status == repository::StreamStatus::failed || stream.status == repository::StreamStatus::unsupported) {
            if (stream.status == repository::StreamStatus::failed) item.status = "corrupt";
            else if (item.status != "corrupt") item.status = "unsupported";
            item.stream = LibraryVerifyStream{stream.id,stream.path};
            if (stream.error) {
              item.message_id = stream.error->message_id;
              if (stream.error->code == Errc::io_error || stream.error->code == Errc::not_found) item.status = "unreadable";
            }
            break;
          }
        if (item.status.empty()) item.status = "corrupt";
      }
      if (item.message_id.empty() && item.status != "ok") item.message_id = "am.catalog.manifest_invalid";
      if (index_ready && item.indexed && library_.mode() == library::Mode::read_write) {
        if (item.status == "corrupt" || item.status == "unreadable")
          repairs.push_back({c.id,{},db::IndexMark{c.id,"corrupt",item.message_id,observed}});
        else if ((item.status == "ok" || item.status == "warn") && previous.at(c.id).health == "corrupt")
          repairs.push_back({c.id,take(read(c.path,c.relative,c.trash)),{}});
      }
      if (item.status == "ok") ++out.ok;
      else if (item.status == "warn") ++out.warn;
      else if (item.status == "corrupt") ++out.corrupt;
      else if (item.status == "unsupported") ++out.unsupported;
      else ++out.unreadable;
      out.items.push_back(std::move(item)); ++out.checked;
      if (options.progress) options.progress(out.checked,out.total);
    }
    if (index_ready && library_.mode() == library::Mode::read_write) {
      auto applied = guard<void>([&]() -> Result<void> {
        auto tx = take(store_.begin_sync());
        for (const auto& repair : repairs) {
          if (repair.mark) take(tx.mark(*repair.mark));
          else if (repair.record) {
            if (hooks_.before_update) take(hooks_.before_update(repair.id));
            take(tx.insert(*repair.record));
          }
        }
        return tx.commit();
      });
      out.index_updated = applied.ok();
      if (!applied) {
        auto marked = store_.mark_rebuild_required();
        out.warnings.push_back(warning_with_cause("am.catalog.index_update_failed",applied.error()));
        if (!marked) { auto warning = marked.error(); warning.severity = Severity::warning; out.warnings.push_back(std::move(warning)); }
      }
    }
    out.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    return out;
  });
}
Result<std::vector<db::IndexSummary>> Indexer::list(bool trash) const { return store_.list(trash); }
Result<bool> Indexer::rebuild_required() const { return store_.rebuild_required(); }
Result<std::int64_t> Indexer::count() const { return store_.count(); }
Result<void> Indexer::mark_rebuild_required() {
  return guard<void>([&]() -> Result<void> {
    writable(library_);
    return store_.mark_rebuild_required();
  });
}

struct Catalog::Impl {
  std::recursive_mutex mutex;
  std::optional<library::Library> library;
  std::unique_ptr<Indexer> indexer;
  std::unique_ptr<db::StateJobStore> jobs;
  std::unique_ptr<db::OperationLog> operations;
  std::vector<Error> warnings;
  OpenReport opened;
  std::function<Result<void>(std::string_view)> before_log;
  void log(std::string code, std::string level, std::optional<std::string> asset,
           const repository::Json& detail, std::vector<Error>& output, std::optional<std::string> job = {}) {
    if (library->mode() == library::Mode::read_only) return;
    auto written = guard<void>([&]() -> Result<void> {
      if (before_log) take(before_log(code));
      db::OperationLog log(library->database(library::DatabaseKind::state));
      db::LogEntry entry; entry.code=std::move(code); entry.level=std::move(level);
      entry.asset_id=std::move(asset); entry.job_id=std::move(job); entry.message="Library operation completed";
      entry.detail_json=detail.dump(); take(log.append(entry)); return {};
    });
    if (!written) {
      AM_LOG(warning,"catalog","library.warning","Library operation log failed",{},&written.error());
      output.push_back(warning_with_cause("am.catalog.log_failed",written.error()));
    }
  }
  bool opening = true, session_marked = false;
  void lifecycle_log(const repository::AssetRef& ref, std::string kind, const Result<repository::LifecycleResult>& result) {
    repository::Json detail=repository::Json::object();
    if (result) detail["op_id"]=result->op_id;
    else detail["message_id"]=result.error().message_id;
    log("library."+kind,result ? warnings.empty() ? "info" : "warn" : "error",ref.asset_id,detail,warnings);
  }
  ~Impl() noexcept {
    if (session_marked && library) {
      try { db::IndexStore store(library->database(library::DatabaseKind::index)); (void)store.set_meta("writer_session","closed"); }
      catch (...) {}
    }
  }
  void index_warning(const std::string& id, const Error& cause, std::vector<Error>& output) {
    AM_LOG(warning,"catalog","library.warning","Library index update failed",{},&cause);
    auto marked = indexer->mark_rebuild_required();
    output.push_back(warning_with_cause("am.catalog.index_update_failed",cause,id));
    if (!marked) { auto warning = marked.error(); warning.severity = Severity::warning; output.push_back(std::move(warning)); }
  }
  void observe(const repository::IndexedEvent& event) {
    const std::lock_guard lock(mutex);
    // Recovery notifications are deferred to the explicit refresh pass after
    // recover, including kinds whose repository observer is not replayed.
    if (opening) return;
    auto indexed = event.kind == "remove" || event.kind == "purge" ? indexer->remove(event.ref.asset_id)
                                                                   : indexer->update(event.ref, event.kind == "trash");
    if (!indexed) {
      AM_LOG(warning,"catalog","library.warning","Library index update failed",{},&indexed.error());
      auto marked = indexer->mark_rebuild_required();
      auto warning = warning_with_cause("am.catalog.index_update_failed",indexed.error(),event.ref.asset_id);
      warnings.push_back(std::move(warning));
      if (!marked) {
        auto e = marked.error();
        e.severity = Severity::warning;
        warnings.push_back(std::move(e));
      }
    }
  }
};
Catalog::Catalog(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
Catalog::Catalog(Catalog&&) noexcept = default;
Catalog& Catalog::operator=(Catalog&&) noexcept = default;
Catalog::~Catalog() = default;
Result<Catalog> Catalog::open(const fs::path& root, library::OpenOptions options, IndexerHooks hooks) {
  auto opened=guard<Catalog>([&]() -> Result<Catalog> {
    auto p = std::make_unique<Impl>();
    bool checked_session = false, previous_unclosed = true, damaged_index = false, journal_may_be_lost = false;
    if (options.mode == library::Mode::read_write) {
      const auto wal = root / ".am/state.sqlite-wal";
      const bool has_wal = fs::exists(paths::native(wal)) && fs::file_size(paths::native(wal)) != 0;
      auto read_options = options;
      read_options.mode = library::Mode::read_only;
      auto before = take(library::Library::open(root,read_options));
      if (before.capabilities().mode == repository::StorageMode::read_only_default)
        return error(Errc::read_only,"am.library.storage_read_only",before.capabilities().filesystem).allow(ErrorAction::open_read_only);
      if (!before.capabilities().writable)
        return error(Errc::read_only,"am.library.not_writable").allow(ErrorAction::open_read_only);
      if (before.owner().locked)
        return error(Errc::locked,"am.library.locked",before.owner().detail).allow(ErrorAction::open_read_only);
      if (before.database_info(library::DatabaseKind::index).state == library::DatabaseState::ok) {
        db::IndexStore store(before.database(library::DatabaseKind::index));
        auto session = store.get_meta("writer_session");
        if (session) previous_unclosed = !*session || **session != "closed";
      }
      checked_session = true;
      journal_may_be_lost = previous_unclosed && !has_wal;
      if (previous_unclosed) {
        // Authoritative checks run on read-only connections before any writer
        // diagnostics, WAL setup, or disposable-index repair can change files.
        for (const auto kind : {library::DatabaseKind::state,library::DatabaseKind::userdata,library::DatabaseKind::index}) {
          if (!before.database(kind)) continue;
          auto check = take(before.check_database(kind,library::DatabaseCheckLevel::full));
          if (check.status != "ok") {
            if (kind != library::DatabaseKind::index) return error(Errc::corrupt_data,"am.library.db_error",check.detail);
            damaged_index = true;
          }
        }
      }
    }
    auto* observer = p.get();
    auto downstream = std::move(options.on_indexed);
    options.on_indexed = [observer, downstream](const repository::IndexedEvent& e) {
      observer->observe(e);
      if (downstream) downstream(e);
    };
    p->library.emplace(take(library::Library::open(root, options)));
    if (damaged_index) take(p->library->reset_index());
    log::Scope scope({{},{},p->library->config().library_id,{}});
    const bool read_only=options.mode==library::Mode::read_only;
    p->jobs=std::make_unique<db::StateJobStore>(p->library->database(library::DatabaseKind::state),read_only,db::StateJobStore::Clock{},options.max_attempts);
    p->operations=std::make_unique<db::OperationLog>(p->library->database(library::DatabaseKind::state),read_only);
    p->before_log=hooks.before_log;
    const auto index_kind = library::DatabaseKind::index;
    const bool index_available = p->library->database_info(index_kind).state == library::DatabaseState::ok && p->library->database(index_kind);
    p->opened.previous_session_unclosed = checked_session ? previous_unclosed : true;
    if (index_available) {
      db::IndexStore store(p->library->database(index_kind));
      auto session = store.get_meta("writer_session");
      if (session) p->opened.previous_session_unclosed = !*session || **session != "closed";
    }
    p->indexer = std::make_unique<Indexer>(fs::absolute(root).lexically_normal(), *p->library, std::move(hooks));
    if (options.mode == library::Mode::read_only) {
      if (!index_available)
        p->opened.warnings.push_back(error(Errc::incomplete,"am.catalog.rebuild_required").with_severity(Severity::warning));
      if (p->opened.previous_session_unclosed)
        p->opened.warnings.push_back(error(Errc::incomplete,"am.library.recovery_pending","writer_session").with_severity(Severity::warning));
      if (p->library->database_info(library::DatabaseKind::state).state == library::DatabaseState::ok) {
        auto state_connection = db::Connection::borrow(p->library->database(library::DatabaseKind::state));
        auto query = state_connection.prepare("SELECT count(*) FROM pending_ops");
        if (query) {
          auto row = query->step();
          if (row && *row == db::Step::row) p->opened.pending_read_only = static_cast<std::size_t>(query->integer(0));
        }
      }
      if (p->opened.pending_read_only)
        p->opened.warnings.push_back(error(Errc::incomplete,"am.library.recovery_pending",std::to_string(p->opened.pending_read_only)).with_severity(Severity::warning));
      auto state=db::Connection::borrow(p->library->database(library::DatabaseKind::state));
      auto running=state.prepare("SELECT count(*) FROM jobs WHERE state='running'");
      if(running) {
        auto row=running->step();
        if(row && *row==db::Step::row && running->integer(0)>0)
          p->opened.warnings.push_back(error(Errc::incomplete,"am.jobs.recovery_pending",std::to_string(running->integer(0))).with_severity(Severity::warning));
      }
    } else {
      const bool state_recreated = p->library->database_info(library::DatabaseKind::state).reason == "recreated";
      if (state_recreated) {
        auto warning = error(Errc::incomplete,"am.library.state_recreated").with_severity(Severity::warning);
        p->opened.warnings.push_back(warning);
      }
      auto journal = db::StateJournal::open(root);
      if (!journal) return error(journal.error().code,"am.library.recovery_failed").caused_by(journal.error());
      auto before = (*journal)->list();
      if (!before) return error(before.error().code,"am.library.recovery_failed").caused_by(before.error());
      auto recovered = p->library->assets().recover(state_recreated || journal_may_be_lost,
                                                   state_recreated ? "journal_recreated" : "journal_may_be_lost");
      if (!recovered) return error(recovered.error().code,"am.library.recovery_failed").caused_by(recovered.error());
      p->opened.recovered = std::move(*recovered);
      for (const auto& item : p->opened.recovered)
        if (item.outcome == commit::RecoveryOutcome::failed || item.outcome == commit::RecoveryOutcome::skipped ||
            item.outcome == commit::RecoveryOutcome::skipped_locked) {
          auto warning = error(Errc::incomplete,"am.library.recovery_incomplete",item.op_id+" "+utf8(item.path)+" "+item.detail).with_severity(Severity::warning);
          if (item.error) warning = std::move(warning).caused_by(*item.error);
          if (item.error && item.error->root_cause().code == Errc::insufficient_space)
            warning.actions = {ErrorAction::free_space,ErrorAction::abort};
          p->opened.warnings.push_back(std::move(warning));
        }
      if (p->library->database_info(index_kind).reason == "recreated") take(p->indexer->rebuild());
      const bool available = p->library->database_info(library::DatabaseKind::index).state == library::DatabaseState::ok && p->library->database(library::DatabaseKind::index);
      bool ready = available && !take(p->indexer->rebuild_required());
      std::set<std::string> touched;
      for (const auto& row : *before) touched.insert(row.asset_id);
      for (const auto& id : touched) {
        if (!ready) break;
        auto updated = p->indexer->refresh(id);
        if (updated) p->opened.reindexed.push_back(id);
        else { p->index_warning(id,updated.error(),p->opened.warnings); ready = false; }
      }
      for (const auto& item:p->opened.recovered) {
        const auto row=std::find_if(before->begin(),before->end(),[&](const auto& r) { return r.op_id==item.op_id; });
        const auto asset=row==before->end() ? std::optional<std::string>{} : std::optional(row->asset_id);
        const bool failed=item.outcome==commit::RecoveryOutcome::failed;
        const bool skipped=item.outcome==commit::RecoveryOutcome::skipped || item.outcome==commit::RecoveryOutcome::skipped_locked;
        p->log("library.recovery",failed ? "error" : skipped ? "warn" : "info",asset,
               repository::Json{{"op_id",item.op_id},{"outcome",commit::outcome_name(item.outcome)}},p->opened.warnings);
      }
      auto recovered_jobs=p->jobs->recover_running();
      if (!recovered_jobs) p->opened.warnings.push_back(warning_with_cause("am.jobs.recovery_failed",recovered_jobs.error(),{},Errc::incomplete));
      else {
        p->opened.jobs_requeued=std::move(recovered_jobs->requeued); p->opened.jobs_failed=std::move(recovered_jobs->failed);
        for (const auto& id:p->opened.jobs_requeued) p->log("library.recovery","info",{},repository::Json{{"job_id",id},{"outcome","queued"}},p->opened.warnings,id);
        for (const auto& id:p->opened.jobs_failed) p->log("library.recovery","error",{},repository::Json{{"job_id",id},{"outcome","failed"}},p->opened.warnings,id);
      }
      if (available) {
        db::IndexStore store(p->library->database(library::DatabaseKind::index));
        auto session = store.get_meta("writer_session");
        if (session) {
          p->opened.previous_session_unclosed = !*session || **session != "closed";
          if (ready && p->opened.previous_session_unclosed) {
            auto synced = p->indexer->sync();
            if (synced) p->opened.sync = std::move(*synced);
            else { p->index_warning("startup sync",synced.error(),p->opened.warnings); ready = false; }
          }
        } else { p->index_warning("writer_session",session.error(),p->opened.warnings); ready = false; }
        auto marked = store.set_meta("writer_session","open");
        if (marked) p->session_marked = true;
        else { p->index_warning("writer_session",marked.error(),p->opened.warnings); ready = false; }
      }
      if (!ready) p->opened.warnings.push_back(error(Errc::incomplete,"am.catalog.rebuild_required").with_severity(Severity::warning));
    }
    AM_LOG(info,"catalog","library.open","Library opened",{{"mode",std::string(options.mode==library::Mode::read_only?"read_only":"read_write")},{"recovered",static_cast<std::uint64_t>(p->opened.recovered.size())},{"jobs_requeued",static_cast<std::uint64_t>(p->opened.jobs_requeued.size())},{"jobs_failed",static_cast<std::uint64_t>(p->opened.jobs_failed.size())},{"unclosed_session",p->opened.previous_session_unclosed}});
    for(const auto& warning:p->opened.warnings)AM_LOG(warning,"catalog","library.warning","Library warning",{},&warning);
    p->opening = false;
    return Catalog(std::move(p));
  });
  if(!opened)AM_LOG(error,"catalog","library.open_failed","Library opening failed",{},&opened.error());
  return opened;
}
library::Library& Catalog::library() noexcept { return *impl_->library; }
Indexer& Catalog::indexer() noexcept { return *impl_->indexer; }
const OpenReport& Catalog::open_report() const noexcept { return impl_->opened; }
const std::vector<Error>& Catalog::operation_warnings() const noexcept { return impl_->warnings; }
db::StateJobStore& Catalog::job_store() noexcept { return *impl_->jobs; }
db::OperationLog& Catalog::operation_log() noexcept { return *impl_->operations; }
Result<LibraryVerifyReport> Catalog::verify(const LibraryVerifyOptions& options) {
  return guard<LibraryVerifyReport>([&]() -> Result<LibraryVerifyReport> {
    const std::lock_guard lock(impl_->mutex);
    log::Scope scope({{},{},impl_->library->config().library_id,{}});
    auto result=impl_->indexer->verify(options);
    if (result) {
      const auto& r=*result;
      impl_->log("library.verify",r.database_failed || r.corrupt || r.unreadable ? "error" : r.warn || r.unsupported || r.cancelled ? "warn" : "info",{},
        repository::Json{{"level",r.level==LibraryVerifyLevel::full ? "full" : "quick"},{"total",r.total},{"checked",r.checked},
          {"ok",r.ok},{"warn",r.warn},{"corrupt",r.corrupt},{"unsupported",r.unsupported},{"unreadable",r.unreadable},{"cancelled",r.cancelled}},result->warnings);
    }
    return result;
  });
}
Result<SyncReport> Catalog::sync(const SyncOptions& options) {
  return guard<SyncReport>([&]() -> Result<SyncReport> {
    const std::lock_guard lock(impl_->mutex);
    log::Scope scope({{},{},impl_->library->config().library_id,{}});
    return impl_->indexer->sync(options);
  });
}
Result<CatalogChange> Catalog::create(const repository::CreateRequest& request) {
  return guard<CatalogChange>([&]() -> Result<CatalogChange> {
    const std::lock_guard lock(impl_->mutex);
    log::Scope scope({{},{},impl_->library->config().library_id,{}});
    impl_->warnings.clear();
    auto changed = take(impl_->library->assets().create(request));
    return CatalogChange{std::move(changed), std::move(impl_->warnings)};
  });
}
Result<CatalogChange> Catalog::change(const repository::ChangeRequest& request) {
  return guard<CatalogChange>([&]() -> Result<CatalogChange> {
    const std::lock_guard lock(impl_->mutex);
    log::Scope scope({{},{},impl_->library->config().library_id,{}});
    impl_->warnings.clear();
    auto changed = take(impl_->library->assets().change(request));
    return CatalogChange{std::move(changed), std::move(impl_->warnings)};
  });
}
Result<CatalogLifecycle> Catalog::trash(const repository::AssetRef& ref) {
  return guard<CatalogLifecycle>([&]() -> Result<CatalogLifecycle> {
    const std::lock_guard lock(impl_->mutex);
    log::Scope scope({{},{},impl_->library->config().library_id,{}});
    impl_->warnings.clear();
    auto changed = impl_->library->assets().trash(ref);
    impl_->lifecycle_log(ref,"trash",changed);
    if (!changed) return changed.error();
    return CatalogLifecycle{std::move(*changed), std::move(impl_->warnings)};
  });
}
Result<CatalogCopy> Catalog::copy_independent(const repository::CopyRequest& request) {
  return guard<CatalogCopy>([&]() -> Result<CatalogCopy> {
    const std::lock_guard lock(impl_->mutex);
    log::Scope scope({{},{},impl_->library->config().library_id,{}});
    impl_->warnings.clear();
    auto copied = impl_->library->assets().copy_independent(request);
    repository::Json detail{{"source_asset",{{"asset_id",request.source.asset_id}}}};
    if (request.op_id) detail["op_id"]=*request.op_id;
    if (copied) { detail["source_asset"]["revision"]=copied->source_revision; detail["op_id"]=copied->change.op_id; }
    else detail["message_id"]=copied.error().message_id;
    impl_->log("library.copy",copied ? impl_->warnings.empty() ? "info" : "warn" : "error",
               copied ? copied->ref.asset_id : request.source.asset_id,detail,impl_->warnings);
    if (!copied) return copied.error();
    return CatalogCopy{std::move(*copied),std::move(impl_->warnings)};
  });
}
Result<CatalogLifecycle> Catalog::restore(const repository::AssetRef& ref) {
  return guard<CatalogLifecycle>([&]() -> Result<CatalogLifecycle> {
    const std::lock_guard lock(impl_->mutex);
    log::Scope scope({{},{},impl_->library->config().library_id,{}});
    impl_->warnings.clear();
    auto changed = impl_->library->assets().restore(ref);
    impl_->lifecycle_log(ref,"restore",changed);
    if (!changed) return changed.error();
    return CatalogLifecycle{std::move(*changed), std::move(impl_->warnings)};
  });
}
Result<CatalogLifecycle> Catalog::purge(const repository::AssetRef& ref) {
  return guard<CatalogLifecycle>([&]() -> Result<CatalogLifecycle> {
    const std::lock_guard lock(impl_->mutex);
    log::Scope scope({{},{},impl_->library->config().library_id,{}});
    impl_->warnings.clear();
    auto changed = impl_->library->assets().purge(ref);
    impl_->lifecycle_log(ref,"purge",changed);
    if (!changed) return changed.error();
    return CatalogLifecycle{std::move(*changed), std::move(impl_->warnings)};
  });
}
Result<repository::ImportReadyResult> Catalog::import_ready(const repository::ImportReadyRequest& request,
                                                            repository::ImportReadyResult* failure) {
  return guard<repository::ImportReadyResult>([&]() -> Result<repository::ImportReadyResult> {
    const std::lock_guard lock(impl_->mutex);
    log::Scope scope({{},{},impl_->library->config().library_id,{}});
    impl_->warnings.clear();
    repository::ImportReadyResult failed;
    auto imported = impl_->library->assets().import_ready(request, &failed);
    auto& result=imported ? *imported : failed;
    repository::Json detail{{"classification",result.classification},{"status",result.status}};
    if (result.op_id) detail["op_id"]=*result.op_id;
    if (result.incoming && result.incoming->revision) detail["revision"]=*result.incoming->revision;
    if (result.status=="replaced" && request.replace_expected) detail["replaced_sha256"]=*request.replace_expected;
    const auto asset=result.ref.asset_id.empty() ? std::optional<std::string>{} : std::optional(result.ref.asset_id);
    const auto level=!imported || result.status=="failed" ? "error" :
      result.status=="identical" || result.status=="conflict" || !result.warnings.empty() || !impl_->warnings.empty() ? "warn" : "info";
    impl_->log("library.import_ready",level,asset,detail,impl_->warnings);
    if (!imported) {
      for (auto& warning:impl_->warnings) failed.warnings.push_back(std::move(warning));
      if (failure) *failure=std::move(failed);
      return imported.error();
    }
    for (auto& warning : impl_->warnings) imported->warnings.push_back(std::move(warning));
    return imported;
  });
}
}  // namespace am::catalog

namespace am::catalog {
namespace {
class VerifyJob final : public jobs::JobHandler {
 public:
  VerifyJob(Catalog& catalog, std::string device, std::function<void(Error)> warning)
      : catalog_(catalog), device_(std::move(device)), warning_(std::move(warning)) {}
  jobs::JobOutcome run(jobs::JobContext& context) override {
    LibraryVerifyOptions options;
    try {
      const auto params = repository::Json::parse(context.record().params_json);
      if (!params.is_object()) return invalid();
      for (const auto& [key, value] : params.items()) {
        if (key == "resource_profile") continue;
        if (key == "level") {
          if (!value.is_string() || (value != "quick" && value != "full")) return invalid();
          options.level = value == "full" ? LibraryVerifyLevel::full : LibraryVerifyLevel::quick;
        } else if (key == "include_trash") {
          if (!value.is_boolean()) return invalid();
          options.include_trash = value.get<bool>();
        } else
          return invalid();
      }
    } catch (const repository::Json::exception&) {
      return invalid();
    }
    const std::array<jobs::ResourceRequest, 1> request{{{jobs::ResourceKind::read, device_}}};
    auto lease = context.acquire(request);
    if (!lease)
      return context.stop().stop_requested() ? jobs::JobOutcome::interrupted()
                                             : jobs::JobOutcome::failure(lease.error());
    options.progress = [&](std::size_t done, std::size_t total) {
      context.report({static_cast<std::uint64_t>(done), static_cast<std::uint64_t>(total), "assets", "verify"});
    };
    options.cancelled = [&] { return context.stop().stop_requested(); };
    auto report = catalog_.verify(options);
    if (!report) return jobs::JobOutcome::failure(report.error());
    if (warning_)
      for (const auto& warning : report->warnings) {
        try {
          warning_(warning);
        } catch (...) {
        }
      }
    if (report->cancelled) return jobs::JobOutcome::interrupted();
    return jobs::JobOutcome::success(repository::Json{
        {"total", report->total},
        {"checked", report->checked},
        {"ok", report->ok},
        {"warn", report->warn},
        {"corrupt", report->corrupt},
        {"unsupported", report->unsupported},
        {"unreadable",
         report->unreadable}}.dump());
  }

 private:
  Catalog& catalog_;
  std::string device_;
  std::function<void(Error)> warning_;
  static jobs::JobOutcome invalid() {
    return jobs::JobOutcome::failure(
        Error::make(Errc::invalid_argument, "Invalid verification job parameters").id("am.jobs.params_invalid"));
  }
};
}  // namespace
Result<void> register_verify_job(jobs::HandlerRegistry& registry, Catalog& catalog, std::string device,
                                 std::function<void(Error)> warning) {
  return registry.add("library.verify",
                      [&catalog, device = std::move(device), warning = std::move(warning)](const auto&) {
                        return std::make_unique<VerifyJob>(catalog, device, warning);
                      });
}
}  // namespace am::catalog
