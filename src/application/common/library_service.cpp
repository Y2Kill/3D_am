#include "am/application/library_service.hpp"
#include "am/storage/library/library.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/foundation/version.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include <algorithm>
namespace am::application {
namespace {
template<class F> Result<AssetOutcome> guard(F&& f) {
  try { return f(); }
  catch (const std::bad_alloc&) { return Error::make(Errc::resource_exhausted, "Allocation failed").id("am.app.out_of_memory"); }
  catch (const std::exception& e) { return Error::make(Errc::io_error, "Library presentation failed", e.what()).id("am.app.io_error"); }
  catch (...) { return Error::make(Errc::internal, "Library presentation failed").id("am.app.internal"); }
}
Json config_json(const library::Config& c) {
  return {{"library_id", c.library_id}, {"name", c.name}, {"created_at", c.created_at}, {"schema_version", c.schema_version}};
}
Json optional_bool(const std::optional<bool>& b) { return b ? Json(*b) : Json(nullptr); }
}  // namespace
Result<AssetOutcome> init_library(const std::filesystem::path& root, const std::string& name) {
  return guard([&]() -> Result<AssetOutcome> {
    library::InitOptions options; options.name = name;
    auto result = library::Library::init(root, options);
    if (!result) return result.error();
    AssetOutcome out; out.value = config_json(*result); return out;
  });
}
Result<AssetOutcome> inspect_library(const std::filesystem::path& root) {
  return guard([&]() -> Result<AssetOutcome> {
    library::OpenOptions options; options.mode = library::Mode::read_only;
    auto result = library::Library::open(root, options);
    if (!result) return result.error();
    const auto& lib = *result;
    AssetOutcome out; out.value = config_json(lib.config());
    auto& j = out.value;
    j["mode"] = "read_only";
    const auto& c = lib.capabilities();
    const auto storage_mode = c.mode == repository::StorageMode::full ? "full" : c.mode == repository::StorageMode::write_with_limits ? "write_with_limits" : "read_only_default";
    const auto durability = c.durability == repository::Durability::verified ? "verified" : c.durability == repository::Durability::assumed ? "assumed" : "unknown";
    j["capabilities"] = {{"mode", storage_mode}, {"max_file_size", c.max_file_size}, {"posix_rename", optional_bool(c.posix_rename)},
      {"replace_file", optional_bool(c.replace_file)}, {"locks_reliable", optional_bool(c.locks_reliable)}, {"durability", durability},
      {"filesystem", c.filesystem}, {"remote", c.remote}, {"readable", c.readable}, {"writable", c.writable}};
    j["write_unavailable_reason"] = c.mode == repository::StorageMode::read_only_default ? "storage_read_only" : !c.writable ? "not_writable" : lib.owner().locked ? "locked" : "";
    j["owner"] = lib.owner().locked ? "locked" : "free";
    if (lib.owner().locked) j["owner_detail"] = lib.owner().detail;
    j["databases"] = Json::object();
    db::IndexStore store(lib.database(library::DatabaseKind::index));
    if (lib.database_info(library::DatabaseKind::index).state == library::DatabaseState::ok) {
      auto count = store.count();
      if (!count) return count.error();
      j["indexed_assets"] = *count;
      auto required = store.rebuild_required();
      if (!required) return required.error();
      if (*required) {
        j["trash"] = nullptr;
        out.diagnostics.push_back(Error::make(Errc::incomplete, "Index rebuild required")
                                      .id("am.catalog.rebuild_required")
                                      .with_severity(Severity::warning));
      } else {
        auto summary = store.trash_summary();
        if (!summary) return summary.error();
        j["trash"] = {{"assets", summary->assets}, {"bytes", summary->bytes ? Json(*summary->bytes) : Json(nullptr)}};
        if (!summary->bytes)
          out.diagnostics.push_back(Error::make(Errc::resource_exhausted, "Trash size exceeds uint64")
                                        .id("am.catalog.trash_size_overflow")
                                        .with_severity(Severity::warning));
      }
    } else {
      j["indexed_assets"] = nullptr;
      j["trash"] = nullptr;
    }
    for (const auto& [kind, name] : {std::pair{library::DatabaseKind::index, "index"}, {library::DatabaseKind::userdata, "userdata"}, {library::DatabaseKind::state, "state"}}) {
      const auto& info = lib.database_info(kind);
      std::string state;
      switch (info.state) {
        case library::DatabaseState::ok: state = "ok"; break;
        case library::DatabaseState::missing: state = "missing"; break;
        case library::DatabaseState::newer: state = "newer"; break;
        case library::DatabaseState::incompatible: state = "incompatible"; break;
        default: state = "unavailable"; break;
      }
      Json db{{"state", state}, {"user_version", info.user_version ? Json(*info.user_version) : Json(nullptr)}};
      if (!info.read_mode.empty()) db["read_mode"] = info.read_mode;
      if (!info.reason.empty()) db["reason"] = info.reason;
      if (!info.detail.empty()) db["detail"] = info.detail;
      if (info.rebuild_required) db["rebuild_required"] = *info.rebuild_required;
      j["databases"][name] = std::move(db);
    }
    return out;
  });
}
Result<AssetOutcome> rebuild_library(const std::filesystem::path& root) {
  return guard([&]() -> Result<AssetOutcome> {
    library::OpenOptions options; options.application_version = kAppVersion;
    auto opened = catalog::Catalog::open(root,options); if (!opened) return opened.error();
    auto report = opened->indexer().rebuild(); if (!report) return report.error();
    AssetOutcome out;
    out.value = {{"indexed",report->indexed},{"duration_ms",report->duration_ms},{"skipped",Json::array()},{"ignored",report->ignored}};
    for (const auto& s : report->skipped) out.value["skipped"].push_back({{"path",s.path},{"reason",s.reason},{"message_id",s.message_id}});
    return out;
  });
}
Result<AssetOutcome> list_library(const std::filesystem::path& root, bool trash) {
  return guard([&]() -> Result<AssetOutcome> {
    library::OpenOptions options; options.mode = library::Mode::read_only;
    auto opened = catalog::Catalog::open(root,options); if (!opened) return opened.error();
    auto rows = opened->indexer().list(trash); if (!rows) return rows.error();
    auto required = opened->indexer().rebuild_required(); if (!required) return required.error();
    AssetOutcome out; out.value = {{"assets",Json::array()},{"rebuild_required",*required}};
    for (const auto& r : *rows) out.value["assets"].push_back({{"asset_id",r.asset_id},{"revision",r.revision},{"title",r.title ? Json(*r.title) : Json(nullptr)},
      {"health",r.health},{"file_count",r.file_count},{"content_bytes",r.content_bytes ? Json(*r.content_bytes) : Json(nullptr)},{"in_trash",r.in_trash}});
    if (*required) out.diagnostics.push_back(Error::make(Errc::incomplete,"Index rebuild required").id("am.catalog.rebuild_required").with_severity(Severity::warning));
    return out;
  });
}
Result<AssetOutcome> sync_library(const std::filesystem::path& root, bool full) {
  return guard([&]() -> Result<AssetOutcome> {
    library::OpenOptions options;
    options.application_version = kAppVersion;
    auto opened = catalog::Catalog::open(root, options);
    if (!opened) return opened.error();
    auto report = opened->sync({full});
    if (!report) return report.error();
    AssetOutcome out;
    out.value = {{"added", report->added},
                 {"updated", report->updated},
                 {"unchanged", report->unchanged},
                 {"missing", report->missing},
                 {"skipped", Json::array()},
                 {"ignored", report->ignored},
                 {"duration_ms", report->duration_ms},
                 {"full", report->full}};
    for (const auto& s : report->skipped) {
      out.value["skipped"].push_back({{"path", s.path}, {"reason", s.reason}, {"message_id", s.message_id}});
      out.diagnostics.push_back(Error::make(Errc::corrupt_data, "Asset could not be indexed", s.path)
                                    .id(s.message_id)
                                    .with_severity(Severity::warning));
    }
    for (const auto& id : report->missing)
      out.diagnostics.push_back(Error::make(Errc::not_found, "Indexed asset is missing", id)
                                    .id("am.catalog.asset_missing")
                                    .with_severity(Severity::warning));
    return out;
  });
}
namespace {
Result<AssetOutcome> lifecycle_library(const std::filesystem::path& root, const std::vector<std::string>& requested,
                                       std::string_view kind, bool all, bool confirmed) {
  return guard([&]() -> Result<AssetOutcome> {
    if ((kind == "purge" && !confirmed) || (all && !requested.empty()) || (!all && requested.empty())) {
      auto error =
          Error::make(Errc::invalid_argument, "Lifecycle arguments are incomplete or inconsistent")
              .id(kind == "purge" && !confirmed ? "am.cli.purge_confirmation_required" : "am.repo.invalid_operation");
      AssetOutcome out;
      out.status = ExitCode::usage;
      out.diagnostics.push_back(error);
      auto json = error_json(error);
      if (!json) return json.error();
      out.value["error"] = *json;
      return out;
    }
    library::OpenOptions options;
    options.application_version = kAppVersion;
    auto opened = catalog::Catalog::open(root, options);
    if (!opened) return opened.error();
    auto ids = requested;
    if (all) {
      for (const auto& entry : std::filesystem::directory_iterator(paths::native(root / "trash"))) {
        if (entry.is_symlink() || !entry.is_regular_file() || entry.path().extension() != ".zip") continue;
        const auto raw = entry.path().stem().u8string();
        const std::string id(raw.begin(), raw.end());
        const auto uuid = Uuid::parse_canonical(id);
        if (uuid && uuid->version() == 4 && (uuid->bytes()[8] & 0xc0) == 0x80) ids.push_back(id);
      }
      std::sort(ids.begin(), ids.end());
    }
    AssetOutcome out;
    out.value["results"] = Json::array();
    for (const auto& id : ids) {
      repository::AssetRef ref{opened->library().config().library_id, id};
      auto result = kind == "trash"     ? opened->trash(ref)
                    : kind == "restore" ? opened->restore(ref)
                                        : opened->purge(ref);
      Json row{{"asset_id", id}, {"status", result ? "done" : "failed"}, {"error", nullptr}};
      if (!result) {
        auto encoded = error_json(result.error());
        if (!encoded) return encoded.error();
        row["error"] = *encoded;
        out.diagnostics.push_back(result.error());
        out.status = ExitCode::operation_failed;
      } else
        for (auto& warning : result->warnings) out.diagnostics.push_back(std::move(warning));
      out.value["results"].push_back(std::move(row));
    }
    return out;
  });
}
}  // namespace
Result<AssetOutcome> trash_library(const std::filesystem::path& root, const std::vector<std::string>& ids) {
  return lifecycle_library(root, ids, "trash", false, true);
}
Result<AssetOutcome> restore_library(const std::filesystem::path& root, const std::vector<std::string>& ids) {
  return lifecycle_library(root, ids, "restore", false, true);
}
Result<AssetOutcome> purge_library(const std::filesystem::path& root, const std::vector<std::string>& ids, bool all,
                                   bool confirmed) {
  return lifecycle_library(root, ids, "purge", all, confirmed);
}
Result<AssetOutcome> add_ready_library(const std::filesystem::path& root,
                                       const std::vector<std::filesystem::path>& sources,
                                       const std::optional<std::string>& replace, bool verbose) {
  return guard([&]() -> Result<AssetOutcome> {
    if (sources.empty() || (replace && sources.size() != 1)) {
      auto error = Error::make(Errc::invalid_argument, "Replacement requires exactly one explicit source")
                       .id("am.repo.invalid_operation");
      AssetOutcome out;
      out.status = ExitCode::usage;
      out.diagnostics.push_back(error);
      auto encoded = error_json(error);
      if (!encoded) return encoded.error();
      out.value["error"] = *encoded;
      return out;
    }
    library::OpenOptions options;
    options.application_version = kAppVersion;
    auto opened = catalog::Catalog::open(root, options);
    if (!opened) return opened.error();
    auto info_json = [](const std::optional<repository::ImportAssetInfo>& info) -> Json {
      if (!info) return nullptr;
      Json out{{"zip_size", info->zip_size}};
      if (info->revision) out["revision"] = *info->revision;
      if (info->manifest_sha256) out["manifest_sha256"] = *info->manifest_sha256;
      if (info->format_minor) out["format_minor"] = *info->format_minor;
      return out;
    };
    AssetOutcome out;
    out.value["results"] = Json::array();
    for (const auto& source : sources) {
      repository::ImportReadyResult detail;
      auto imported = opened->import_ready({source, replace}, &detail);
      if (imported)
        detail = std::move(*imported);
      else {
        detail.status = "failed";
        detail.error = imported.error();
      }
      const auto raw = source.generic_u8string();
      Json row{{"source", std::string(raw.begin(), raw.end())},
               {"asset_id", detail.ref.asset_id.empty() ? Json(nullptr) : Json(detail.ref.asset_id)},
               {"classification", detail.classification},
               {"status", detail.status},
               {"incoming", info_json(detail.incoming)},
               {"current", info_json(detail.current)},
               {"zip_bytes_identical", detail.zip_bytes_identical ? Json(*detail.zip_bytes_identical) : Json(nullptr)},
               {"error", nullptr}};
      if (detail.error) {
        auto encoded = error_json(*detail.error);
        if (!encoded) return encoded.error();
        row["error"] = *encoded;
        out.diagnostics.push_back(*detail.error);
      }
      if (detail.verification && (verbose || detail.status == "failed")) {
        auto report = repository::verification_json(*detail.verification);
        if (!report) return report.error();
        row["verification"] = *report;
      }
      for (auto& warning : detail.warnings) out.diagnostics.push_back(std::move(warning));
      if (detail.status == "failed" || detail.status == "conflict") out.status = ExitCode::operation_failed;
      out.value["results"].push_back(std::move(row));
    }
    return out;
  });
}
}  // namespace am::application
