#include "am/foundation/log.hpp"
#include "am/application/library_service.hpp"
#include "am/application/job_service.hpp"
#include "am/services/import/run.hpp"
#include "am/services/export/run.hpp"
#include "am/services/catalog/verify_job.hpp"
#include "am/foundation/json_text.hpp"
#include <array>
#include <limits>
#include <mutex>
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
void opening_warnings(AssetOutcome& out, const catalog::Catalog& catalog) {
  const auto& warnings = catalog.open_report().warnings;
  out.diagnostics.insert(out.diagnostics.end(),warnings.begin(),warnings.end());
}
Json database_checks_json(const std::vector<library::DatabaseCheck>& checks) {
  Json out = Json::object();
  for (const auto& check : checks) {
    const char* name = check.database == library::DatabaseKind::index ? "index" :
                       check.database == library::DatabaseKind::state ? "state" : "userdata";
    out[name] = {{"status",check.status},{"check",check.level == library::DatabaseCheckLevel::full ? "integrity_check" : "quick_check"},
                 {"detail",check.detail},{"message_id",check.message_id}};
  }
  return out;
}
Result<AssetOutcome> catalog_failure(const Error& e, const catalog::Catalog& catalog) {
  if (catalog.open_report().warnings.empty()) return e;
  AssetOutcome out; out.status = error_exit_code(e);
  auto encoded = error_json(e); if (!encoded) return encoded.error();
  out.value["error"] = *encoded; out.diagnostics.push_back(e); opening_warnings(out,catalog);
  return out;
}
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
      if (kind == library::DatabaseKind::state && info.state == library::DatabaseState::ok) {
        auto state_database = am::db::Connection::borrow(lib.database(kind));
        auto event = state_database.prepare("SELECT at FROM op_log WHERE code='am.library.state_recreated' ORDER BY seq DESC LIMIT 1");
        if (!event) return event.error();
        auto row = event->step();
        if (!row) return row.error();
        if (*row == am::db::Step::row) {
          db["reason"] = "recreated";
          auto at = event->text(0);
          if (!at) return at.error();
          db["recreated_at"] = *at;
        }
      }
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
    auto report = opened->indexer().rebuild(); if (!report) return catalog_failure(report.error(),*opened);
    AssetOutcome out;
    out.value = {{"indexed",report->indexed},{"duration_ms",report->duration_ms},{"skipped",Json::array()},{"ignored",report->ignored}};
    for (const auto& s : report->skipped) out.value["skipped"].push_back({{"path",s.path},{"reason",s.reason},{"message_id",s.message_id}});
    opening_warnings(out,*opened);
    return out;
  });
}
Result<AssetOutcome> list_library(const std::filesystem::path& root, bool trash) {
  return guard([&]() -> Result<AssetOutcome> {
    library::OpenOptions options; options.mode = library::Mode::read_only;
    auto opened = catalog::Catalog::open(root,options); if (!opened) return opened.error();
    auto rows = opened->indexer().list(trash); if (!rows) return catalog_failure(rows.error(),*opened);
    auto required = opened->indexer().rebuild_required(); if (!required) return catalog_failure(required.error(),*opened);
    AssetOutcome out; out.value = {{"assets",Json::array()},{"rebuild_required",*required}};
    for (const auto& r : *rows) out.value["assets"].push_back({{"asset_id",r.asset_id},{"revision",r.revision},{"title",r.title ? Json(*r.title) : Json(nullptr)},
      {"health",r.health},{"file_count",r.file_count},{"content_bytes",r.content_bytes ? Json(*r.content_bytes) : Json(nullptr)},{"in_trash",r.in_trash}});
    if (*required) out.diagnostics.push_back(Error::make(Errc::incomplete,"Index rebuild required").id("am.catalog.rebuild_required").with_severity(Severity::warning));
    opening_warnings(out,*opened);
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
    if (!report) return catalog_failure(report.error(),*opened);
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
    opening_warnings(out,*opened);
    return out;
  });
}
Result<AssetOutcome> copy_library(const std::filesystem::path& root, const std::string& id, bool verbose) {
  return guard([&]() -> Result<AssetOutcome> {
    AssetOutcome out;
    out.value = {{"source",{{"asset_id",id},{"revision",nullptr}}},{"asset_id",nullptr},
                 {"revision",nullptr},{"files",Json::array()},{"error",nullptr}};
    auto rejected = [&](const Error& e) -> Result<AssetOutcome> {
      out.status = ExitCode::operation_failed;
      auto encoded = error_json(e); if (!encoded) return encoded.error();
      out.value["error"] = *encoded; out.diagnostics.push_back(e); return out;
    };
    const auto uuid = Uuid::parse_canonical(id);
    if (!uuid || uuid->version() != 4 || (uuid->bytes()[8] & 0xc0) != 0x80) {
      auto result = rejected(Error::make(Errc::invalid_argument,"AssetID must be canonical UUIDv4")
                                 .id("am.repo.invalid_operation"));
      if (result) result->status = ExitCode::usage;
      return result;
    }
    library::OpenOptions options; options.application_version = kAppVersion;
    auto opened = catalog::Catalog::open(root,options); if (!opened) return rejected(opened.error());
    const repository::AssetRef ref{opened->library().config().library_id,id};
    auto copied = opened->copy_independent({ref,{},{}});
    if (!copied) {
      auto result = rejected(copied.error());
      if (result) {
        opening_warnings(*result,*opened);
        const auto& warnings=opened->operation_warnings(); result->diagnostics.insert(result->diagnostics.end(),warnings.begin(),warnings.end());
      }
      return result;
    }
    const auto& result = copied->copy;
    out.value["source"]["revision"] = result.source_revision;
    out.value["asset_id"] = result.ref.asset_id; out.value["revision"] = result.change.revision;
    for (const auto& stream : result.change.verification.v4)
      out.value["files"].push_back({{"id",stream.id},{"path",stream.path}});
    for (auto& warning : copied->warnings) out.diagnostics.push_back(std::move(warning));
    if (verbose) {
      auto report = repository::verification_json(result.change.verification); if (!report) return report.error();
      out.value["verification"] = *report;
    }
    opening_warnings(out,*opened);
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
    if (all && std::filesystem::exists(paths::native(root / "trash"))) {
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
        const auto& warnings=opened->operation_warnings(); out.diagnostics.insert(out.diagnostics.end(),warnings.begin(),warnings.end());
        out.status = ExitCode::operation_failed;
      } else
        for (auto& warning : result->warnings) out.diagnostics.push_back(std::move(warning));
      out.value["results"].push_back(std::move(row));
    }
    opening_warnings(out,*opened);
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
    opening_warnings(out,*opened);
    return out;
  });
}
Result<AssetOutcome> verify_library(const std::filesystem::path& root, bool full, bool trash, bool verbose) {
  return guard([&]() -> Result<AssetOutcome> {
    auto failed = [&](const Error& e, const catalog::Catalog* catalog = nullptr) -> Result<AssetOutcome> {
      AssetOutcome out; out.status = ExitCode::operation_failed;
      auto encoded = error_json(e); if (!encoded) return encoded.error();
      out.value["error"] = *encoded; out.diagnostics.push_back(e);
      if (catalog) opening_warnings(out,*catalog);
      return out;
    };
    library::OpenOptions options; options.application_version = kAppVersion;
    auto opened = catalog::Catalog::open(root,options);
    if (!opened && (opened.error().message_id == "am.library.storage_read_only" || opened.error().message_id == "am.repo.storage_read_only")) {
      options.mode = library::Mode::read_only; opened = catalog::Catalog::open(root,options);
    }
    if (!opened) {
      // A damaged authoritative database can prevent recovery/open. Still expose all DB findings.
      library::OpenOptions read; read.mode = library::Mode::read_only;
      auto inspection = library::Library::open(root, read);
      if (inspection) {
        std::vector<library::DatabaseCheck> checks; bool damaged = false;
        for (const auto kind : {library::DatabaseKind::index, library::DatabaseKind::userdata, library::DatabaseKind::state}) {
          auto check = inspection->check_database(kind, full ? library::DatabaseCheckLevel::full : library::DatabaseCheckLevel::quick);
          if (!check) return check.error();
          if (kind != library::DatabaseKind::index && check->status != "ok") damaged = true;
          checks.push_back(std::move(*check));
        }
        if (damaged) {
          auto out = failed(opened.error()); if (!out) return out;
          out->status = ExitCode::check_failed; out->value["databases"] = database_checks_json(checks);
          return out;
        }
      }
      return failed(opened.error());
    }
    catalog::LibraryVerifyOptions verify; verify.level = full ? catalog::LibraryVerifyLevel::full : catalog::LibraryVerifyLevel::quick; verify.include_trash = trash;
    auto report = opened->verify(verify); if (!report) return failed(report.error(),&*opened);
    AssetOutcome out;
    out.status = report->database_failed || report->corrupt || report->unreadable ? ExitCode::check_failed :
                 report->unsupported || report->cancelled ? ExitCode::partial : ExitCode::success;
    out.value = {{"level",full ? "full" : "quick"},{"total",report->total},{"checked",report->checked},{"ok",report->ok},
      {"warn",report->warn},{"corrupt",report->corrupt},{"unsupported",report->unsupported},{"unreadable",report->unreadable},
      {"cancelled",report->cancelled},{"index_updated",report->index_updated},{"items",Json::array()},
      {"skipped",Json::array()},{"ignored",report->ignored},{"duration_ms",report->duration_ms}};
    out.value["databases"] = database_checks_json(report->databases);
    for (const auto& item : report->items) {
      if (item.status != "ok" || verbose)
        out.value["items"].push_back({{"asset_id",item.asset_id},{"path",item.path},{"in_trash",item.in_trash},{"indexed",item.indexed},
          {"status",item.status},{"message_id",item.message_id},{"pointer",item.pointer ? Json(*item.pointer) : Json(nullptr)},
          {"stream",item.stream ? Json{{"id",item.stream->id},{"path",item.stream->path}} : Json(nullptr)}});
      if (item.status != "ok")
        out.diagnostics.push_back(Error::make(Errc::corrupt_data,"Library verification finding",item.path).id(item.message_id)
          .with_severity(item.status == "warn" ? Severity::warning : Severity::error));
    }
    for (const auto& item : report->skipped)
      out.value["skipped"].push_back({{"path",item.path},{"reason",item.reason},{"message_id",item.message_id}});
    const auto& recovery = opened->open_report();
    out.value["recovery"] = {{"recovered",Json::array()},{"reindexed",recovery.reindexed},
      {"previous_session_unclosed",recovery.previous_session_unclosed},{"sync",nullptr}};
    for (const auto& item : recovery.recovered) {
      const auto raw = item.path.generic_u8string();
      out.value["recovery"]["recovered"].push_back({{"op_id",item.op_id},{"path",std::string(raw.begin(),raw.end())},
        {"outcome",commit::outcome_name(item.outcome)},{"detail",item.detail}});
    }
    if (recovery.sync) {
      const auto& s = *recovery.sync;
      auto& json = out.value["recovery"]["sync"];
      json = {{"added",s.added},{"updated",s.updated},{"unchanged",s.unchanged},{"missing",s.missing},
        {"skipped",Json::array()},{"ignored",s.ignored},{"duration_ms",s.duration_ms},{"full",s.full}};
      for (const auto& item : s.skipped) json["skipped"].push_back({{"path",item.path},{"reason",item.reason},{"message_id",item.message_id}});
    }
    out.diagnostics.insert(out.diagnostics.end(),report->warnings.begin(),report->warnings.end());
    opening_warnings(out,*opened);
    return out;
  });
}
Result<LibraryLogOutcome> library_log(const std::filesystem::path& root,const db::LogQuery& query) {
  try {
    library::OpenOptions options; options.mode=library::Mode::read_only;
    auto catalog=catalog::Catalog::open(root,options); if(!catalog) return catalog.error();
    db::OperationLog log(catalog->library().database(library::DatabaseKind::state),true);
    auto entries=log.query(query); if(!entries) return entries.error();
    LibraryLogOutcome out; out.entries=std::move(*entries); out.diagnostics=catalog->open_report().warnings;
    out.json="{\"entries\":[";
    bool first=true;
    for(const auto& entry:out.entries) {
      Json row{{"seq",entry.seq},{"at",entry.at},{"level",entry.level},{"code",entry.code},
        {"job_id",entry.job_id ? Json(*entry.job_id) : Json(nullptr)},
        {"asset_id",entry.asset_id ? Json(*entry.asset_id) : Json(nullptr)},{"message",entry.message}};
      auto encoded=row.dump(); encoded.pop_back();
      if(!first) out.json+=',';
      first=false;
      // OperationLog validates this JSON without converting exact numbers.
      out.json+=encoded+",\"detail\":"+entry.detail_json.value_or("null")+"}";
    }
    out.json+="],\"next_after\":"+(out.entries.empty() ? std::string("null") : std::to_string(out.entries.back().seq))+",\"command\":\"library log\"}";
    return out;
  } catch(const std::bad_alloc&) { return Error::make(Errc::resource_exhausted,"Allocation failed").id("am.app.out_of_memory"); }
  catch(...) { return Error::make(Errc::internal,"Log presentation failed").id("am.app.internal"); }
}
}  // namespace am::application

namespace am::application {
namespace {
namespace jc = contracts;
using JobState = jc::JobState;
Error job_error(std::string id, Errc code = Errc::invalid_argument) {
  return Error::make(code, "Job service operation failed").in("application.jobs").id(std::move(id));
}
template <class T>
T job_take(Result<T> result) {
  if (!result) throw result.error();
  return std::move(*result);
}
void job_take(Result<void> result) {
  if (!result) throw result.error();
}
template <class T, class F>
Result<T> job_guard(F&& function) {
  try {
    return function();
  } catch (const Error& e) {
    return e;
  } catch (const std::bad_alloc&) {
    return job_error("am.app.out_of_memory", Errc::resource_exhausted);
  } catch (const std::exception& e) {
    return Error::make(Errc::internal, "Job service failed", e.what()).id("am.app.internal");
  } catch (...) {
    return job_error("am.app.internal", Errc::internal);
  }
}
jobs::MediaKind job_media(repository::MediaKind kind) {
  switch (kind) {
    case repository::MediaKind::hdd:
      return jobs::MediaKind::hdd;
    case repository::MediaKind::ssd:
      return jobs::MediaKind::ssd;
    case repository::MediaKind::network:
      return jobs::MediaKind::network;
    case repository::MediaKind::removable:
      return jobs::MediaKind::removable;
    default:
      return jobs::MediaKind::unknown;
  }
}
std::optional<std::string> job_message_id(const std::optional<std::string>& encoded) {
  if (!encoded) return {};
  const auto parsed = json::parse_strict(*encoded);
  if (!parsed || parsed->nodes[0].kind != json::Kind::object) return {};
  for (auto index : parsed->nodes[0].children)
    if (parsed->nodes[index].edge == "message_id" && parsed->nodes[index].kind == json::Kind::string)
      return parsed->nodes[index].decoded;
  return {};
}
class ProbeJob final : public jobs::JobHandler {
 public:
  jobs::JobOutcome run(jobs::JobContext& context) override {
    std::uint64_t steps = 1;
    bool ask = false;
    std::optional<std::string> failure;
    try {
      const auto params = Json::parse(context.record().params_json);
      if (!params.is_object()) return invalid();
      for (const auto& [key, value] : params.items()) {
        if (key == "resource_profile") continue;
        if (key == "steps") {
          if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
            return invalid();
          steps = value.get<std::uint64_t>();
          if (steps > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return invalid();
        } else if (key == "ask") {
          if (!value.is_boolean()) return invalid();
          ask = value.get<bool>();
        } else if (key == "fail") {
          if (!value.is_null()) {
            if (!value.is_string() || value == "") return invalid();
            failure = value.get<std::string>();
          }
        } else
          return invalid();
      }
    } catch (const Json::exception&) {
      return invalid();
    }
    if (context.stop().stop_requested()) return jobs::JobOutcome::interrupted();
    if (ask && !context.answer()) return jobs::JobOutcome::ask({"probe", {"continue", "stop"}, "{}"});
    if (context.answer() == "stop")
      return jobs::JobOutcome::failure(
          Error::make(Errc::cancelled, "Development probe stopped").id("am.jobs.probe_stopped"));
    const std::array<jobs::ResourceRequest, 1> request{{{jobs::ResourceKind::cpu, ""}}};
    auto lease = context.acquire(request);
    if (!lease)
      return context.stop().stop_requested() ? jobs::JobOutcome::interrupted()
                                             : jobs::JobOutcome::failure(lease.error());
    for (auto done = std::min(steps, context.record().progress.done.value_or(0)); done < steps; ++done) {
      if (context.stop().stop_requested()) return jobs::JobOutcome::interrupted();
      context.report({done + 1, steps, "steps", "probe"});
    }
    if (failure) return jobs::JobOutcome::failure(Error::make(Errc::internal, "Development probe failed").id(*failure));
    return jobs::JobOutcome::success(Json{{"steps", steps}}.dump());
  }

 private:
  static jobs::JobOutcome invalid() { return jobs::JobOutcome::failure(job_error("am.jobs.params_invalid")); }
};
Json progress_json(const jc::JobProgress& progress) {
  return {{"done", progress.done ? Json(*progress.done) : Json(nullptr)},
          {"total", progress.total ? Json(*progress.total) : Json(nullptr)},
          {"unit", progress.unit},
          {"stage", progress.stage}};
}
Json job_row(const jc::JobRecord& record, const jobs::ChildrenSummary* children = nullptr) {
  Json row{{"job_id", record.job_id},
           {"type", record.type},
           {"state", jc::job_state_name(record.state)},
           {"priority", record.priority},
           {"position", record.position},
           {"attempt", record.attempt},
           {"progress", progress_json(children ? children->progress : record.progress)},
           {"parent_id", record.parent_id ? Json(*record.parent_id) : Json(nullptr)},
           {"created_at", record.created_at},
           {"started_at", record.started_at ? Json(*record.started_at) : Json(nullptr)},
           {"finished_at", record.finished_at ? Json(*record.finished_at) : Json(nullptr)}};
  if (children) {
    row["children"] = Json::object();
    for (const auto& [state, count] : children->counts) row["children"][jc::job_state_name(state)] = count;
  }
  return row;
}
std::string full_job_json(const jc::JobRecord& record, Json row) {
  std::string encoded = row.dump();
  encoded.pop_back();
  const std::array<std::pair<std::string_view, std::optional<std::string>>, 4> fields{
      {{"params", record.params_json},
       {"decision", record.decision_json},
       {"result", record.result_json},
       {"error", record.error_json}}};
  for (const auto& [key, value] : fields) {
    if (value && !json::parse_strict(*value)) throw job_error("am.jobs.json_invalid", Errc::corrupt_data);
    encoded += "," + Json(key).dump() + ":" + value.value_or("null");
  }
  return encoded + "}";
}
JobCommandOutcome failed_open(const Error& error) {
  JobCommandOutcome out;
  out.status = ExitCode::operation_failed;
  out.value["error"] = job_take(error_json(error));
  out.diagnostics.push_back(error);
  out.json = out.value.dump();
  return out;
}
}  // namespace
struct JobService::Impl final : jobs::JobObserver {
  std::optional<am::catalog::Catalog> catalog;
  JobServiceOptions options;
  mutable std::mutex diagnostics_mutex;
  std::vector<Error> diagnostics;
  jobs::HandlerRegistry registry;
  std::unique_ptr<jobs::ResourceManager> resources;
  std::unique_ptr<jobs::Executor> executor;
  void warn(Error warning) noexcept {
    try {
      const std::lock_guard lock(diagnostics_mutex);
      diagnostics.push_back(std::move(warning));
    } catch (...) {
    }
  }
  void on_event(const jobs::JobEvent& event) noexcept override {
    try {
      if (event.kind == jobs::EventKind::started || event.kind == jobs::EventKind::finished) {
        const auto& job = event.job;
        db::LogEntry entry;
        entry.job_id = job.job_id;
        entry.code = event.kind == jobs::EventKind::started ? "job.started" : "job.finished";
        entry.message = event.kind == jobs::EventKind::started ? "Job started" : "Job finished";
        entry.level = event.kind == jobs::EventKind::started || job.state == JobState::succeeded ? "info"
                      : job.state == JobState::failed || job.state == JobState::resource_error ||
                              job.state == JobState::cannot_continue_safely
                          ? "error"
                          : "warn";
        Json detail{{"type", job.type}, {"state", jc::job_state_name(job.state)}, {"attempt", job.attempt}};
        if (auto message = job_message_id(job.error_json)) detail["message_id"] = *message;
        entry.detail_json = detail.dump();
        auto written = catalog->operation_log().append(entry);
        if (!written)
          warn(Error::make(Errc::io_error, "Job log write failed")
                   .id("am.catalog.log_failed")
                   .with_severity(Severity::warning)
                   .caused_by(written.error()));
      }
      if (options.on_event) options.on_event(event);
    } catch (...) {
      try {
        warn(Error::make(Errc::internal, "Job event could not be presented")
                 .id("am.catalog.log_failed")
                 .with_severity(Severity::warning));
      } catch (...) {
      }
    }
  }
};
JobService::JobService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
JobService::JobService(JobService&&) noexcept = default;
JobService& JobService::operator=(JobService&&) noexcept = default;
JobService::~JobService() = default;
Result<JobService> JobService::open(const std::filesystem::path& root, const JobServiceOptions& options,
                                    library::OpenOptions library_options, am::catalog::IndexerHooks hooks) {
  return job_guard<JobService>([&]() -> Result<JobService> {
    auto p = std::make_unique<Impl>();
    p->options = options;
    if (library_options.application_version.empty()) library_options.application_version = kAppVersion;
    p->catalog.emplace(job_take(am::catalog::Catalog::open(root, std::move(library_options), std::move(hooks))));
    p->diagnostics = p->catalog->open_report().warnings;
    auto limits = options.limits.value_or(jobs::profile_limits(options.profile, std::thread::hardware_concurrency()));
    if (options.parallel_jobs) limits.jobs = *options.parallel_jobs;
    job_take(jobs::validate_limits(limits));
    p->resources = std::make_unique<jobs::ResourceManager>(limits);
    const auto media = repository::detect_media(root);
    p->resources->set_media(media.device_id, job_media(media.kind));
    auto* target = p.get();
    job_take(am::catalog::register_verify_job(p->registry, *p->catalog, media.device_id,
                                              [target](Error warning) { target->warn(std::move(warning)); }));
    job_take(p->registry.add("dev.probe", [](const auto&) { return std::make_unique<ProbeJob>(); }));
    job_take(am::import::register_import_jobs(p->registry,*p->catalog,media.device_id));
    job_take(am::exporting::register_export_jobs(p->registry,*p->catalog,*p->resources));
    jobs::ExecutorOptions executor;
    executor.limits = limits;
    executor.profile = options.profile;
    executor.report_interval = options.report_interval;
    executor.retry_interval = options.retry_interval;
    executor.auto_retries = options.auto_retries;
    p->executor =
        std::make_unique<jobs::Executor>(p->catalog->job_store(), *p->resources, p->registry, executor, p.get());
    return JobService(std::move(p));
  });
}
Result<jc::JobRecord> JobService::enqueue(const jc::JobSpec& spec) {
  auto result = impl_->catalog->job_store().enqueue(spec);
  if (result) impl_->executor->wake();
  return result;
}
Result<jobs::RunSummary> JobService::run(std::stop_token stop) { log::Scope scope({{},{},impl_->catalog->library().config().library_id,{}}); return impl_->executor->run_until_idle(stop); }
Result<void> JobService::control(std::string_view command, std::string_view id) {
  if (command == "pause") return impl_->executor->pause(id);
  if (command == "resume") return impl_->executor->resume(id);
  if (command == "cancel") return impl_->executor->cancel(id);
  if (command == "retry") return impl_->executor->retry(id);
  return job_error("am.jobs.transition_invalid");
}
Result<void> JobService::answer(std::string_view id, std::string_view option) {
  return impl_->executor->answer(id, option);
}
Result<void> JobService::move(std::string_view id, std::optional<std::string_view> before) {
  auto result = impl_->catalog->job_store().move_before(id, before);
  if (result) impl_->executor->wake();
  return result;
}
jc::IJobStore& JobService::store() noexcept { return impl_->catalog->job_store(); }
am::catalog::Catalog& JobService::catalog() noexcept { return *impl_->catalog; }
std::vector<Error> JobService::diagnostics() const {
  const std::lock_guard lock(impl_->diagnostics_mutex);
  return impl_->diagnostics;
}
Result<JobCommandOutcome> list_jobs(const std::filesystem::path& root, const jc::JobFilter& selected, bool all) {
  return job_guard<JobCommandOutcome>([&]() -> Result<JobCommandOutcome> {
    library::OpenOptions options;
    options.mode = library::Mode::read_only;
    auto service = JobService::open(root, {}, options);
    if (!service) return failed_open(service.error());
    auto filter = selected;
    if (!filter.parent_id && !all) filter.top_level_only = true;
    auto records = job_take(service->store().list(filter));
    const auto every = job_take(service->store().list());
    std::map<std::string, std::vector<jc::JobRecord>> children;
    for (const auto& record : every)
      if (record.parent_id) children[*record.parent_id].push_back(record);
    JobCommandOutcome out;
    out.value["jobs"] = Json::array();
    out.diagnostics = service->diagnostics();
    for (auto& record : records) {
      if (!all && selected.states.empty() && jc::job_terminal(record.state)) continue;
      const auto at = children.find(record.job_id);
      if (at != children.end()) {
        const auto summary = jobs::summarize_children(at->second);
        out.value["jobs"].push_back(job_row(record, &summary));
      } else
        out.value["jobs"].push_back(job_row(record));
      out.records.push_back(std::move(record));
    }
    out.json = "{\"jobs\":[";
    for(std::size_t i=0;i<out.records.size();++i) {
      if(i) out.json += ',';
      auto row=out.value.at("jobs").at(i).dump();
      const auto& record=out.records[i];
      if((record.state==jc::JobState::failed || record.state==jc::JobState::needs_decision) && record.result_json) {
        if(!json::parse_strict(*record.result_json)) return job_error("am.jobs.json_invalid",Errc::corrupt_data);
        row.pop_back(); row+=",\"result\":"+*record.result_json+"}";
      }
      out.json += row;
    }
    out.json += "]}";
    return out;
  });
}
Result<JobCommandOutcome> show_job(const std::filesystem::path& root, std::string_view id) {
  return job_guard<JobCommandOutcome>([&]() -> Result<JobCommandOutcome> {
    library::OpenOptions options;
    options.mode = library::Mode::read_only;
    auto service = JobService::open(root, {}, options);
    if (!service) return failed_open(service.error());
    const auto record = job_take(service->store().get(id));
    if (!record) return job_error("am.jobs.not_found", Errc::not_found);
    JobCommandOutcome out;
    out.value = job_row(*record);
    out.json = full_job_json(*record, out.value);
    out.records.push_back(*record);
    out.diagnostics = service->diagnostics();
    return out;
  });
}
Result<JobCommandOutcome> run_jobs(const std::filesystem::path& root, const JobServiceOptions& options,
                                   std::stop_token stop) {
  return job_guard<JobCommandOutcome>([&]() -> Result<JobCommandOutcome> {
    auto service = JobService::open(root, options);
    if (!service) return failed_open(service.error());
    const auto result = job_take(service->run(stop));
    JobCommandOutcome out;
    out.records = result.jobs;
    out.diagnostics = service->diagnostics();
    out.status = result.failed                            ? ExitCode::check_failed
                 : result.attention || result.interrupted ? ExitCode::partial
                                                          : ExitCode::success;
    out.value = {{"processed", result.processed}, {"succeeded", result.succeeded},     {"failed", result.failed},
                 {"attention", result.attention}, {"interrupted", result.interrupted}, {"jobs", Json::array()}};
    for (const auto& record : result.jobs) {
      Json row{{"job_id", record.job_id}, {"type", record.type}, {"state", jc::job_state_name(record.state)}};
      if (auto message = job_message_id(record.error_json)) row["message_id"] = *message;
      out.value["jobs"].push_back(std::move(row));
    }
    out.json = out.value.dump();
    return out;
  });
}
Result<JobCommandOutcome> control_jobs(const std::filesystem::path& root, std::string_view command,
                                       const std::vector<std::string>& ids) {
  return job_guard<JobCommandOutcome>([&]() -> Result<JobCommandOutcome> {
    auto service = JobService::open(root);
    if (!service) return failed_open(service.error());
    JobCommandOutcome out;
    out.value["results"] = Json::array();
    for (const auto& id : ids) {
      auto applied = service->control(command, id);
      Json row{{"job_id", id}, {"ok", applied.ok()}};
      if (applied) {
        const auto record = job_take(service->store().get(id));
        if (record) row["state"] = jc::job_state_name(record->state);
      } else {
        row["message_id"] = applied.error().message_id;
        out.status = ExitCode::operation_failed;
        out.diagnostics.push_back(applied.error());
      }
      out.value["results"].push_back(std::move(row));
    }
    auto warnings = service->diagnostics();
    out.diagnostics.insert(out.diagnostics.end(), warnings.begin(), warnings.end());
    out.json = out.value.dump();
    return out;
  });
}
Result<JobCommandOutcome> answer_job(const std::filesystem::path& root, std::string_view id, std::string_view option) {
  return job_guard<JobCommandOutcome>([&]() -> Result<JobCommandOutcome> {
    auto service = JobService::open(root);
    if (!service) return failed_open(service.error());
    job_take(service->answer(id, option));
    const auto record = job_take(service->store().get(id));
    JobCommandOutcome out;
    out.value = job_row(*record);
    out.json = out.value.dump();
    out.diagnostics = service->diagnostics();
    return out;
  });
}
Result<JobCommandOutcome> move_job(const std::filesystem::path& root, std::string_view id,
                                   std::optional<std::string_view> before) {
  return job_guard<JobCommandOutcome>([&]() -> Result<JobCommandOutcome> {
    auto service = JobService::open(root);
    if (!service) return failed_open(service.error());
    job_take(service->move(id, before));
    const auto record = job_take(service->store().get(id));
    JobCommandOutcome out;
    out.value = job_row(*record);
    out.json = out.value.dump();
    out.diagnostics = service->diagnostics();
    return out;
  });
}
Result<JobCommandOutcome> enqueue_verify(const std::filesystem::path& root, bool full, bool trash) {
  return job_guard<JobCommandOutcome>([&]() -> Result<JobCommandOutcome> {
    auto service = JobService::open(root);
    if (!service) return failed_open(service.error());
    jc::JobSpec spec;
    spec.type = "library.verify";
    spec.params_json = Json{{"level", full ? "full" : "quick"}, {"include_trash", trash}}.dump();
    const auto record = job_take(service->enqueue(spec));
    JobCommandOutcome out;
    out.value = {{"job_id", record.job_id}};
    out.json = out.value.dump();
    out.diagnostics = service->diagnostics();
    return out;
  });
}
Result<JobCommandOutcome> enqueue_probe(const std::filesystem::path& root, std::uint64_t steps, bool ask,
                                        const std::optional<std::string>& failure) {
  return job_guard<JobCommandOutcome>([&]() -> Result<JobCommandOutcome> {
    auto service = JobService::open(root);
    if (!service) return failed_open(service.error());
    jc::JobSpec spec;
    spec.type = "dev.probe";
    spec.params_json = Json{{"steps", steps}, {"ask", ask}, {"fail", failure ? Json(*failure) : Json(nullptr)}}.dump();
    const auto record = job_take(service->enqueue(spec));
    JobCommandOutcome out;
    out.value = {{"job_id", record.job_id}};
    out.json = out.value.dump();
    out.diagnostics = service->diagnostics();
    return out;
  });
}
Result<JobCommandOutcome> inspect_media(const std::filesystem::path& root) {
  return job_guard<JobCommandOutcome>([&]() -> Result<JobCommandOutcome> {
    const auto media = repository::detect_media(root);
    const char* kind = "unknown";
    switch (media.kind) {
      case repository::MediaKind::hdd:
        kind = "hdd";
        break;
      case repository::MediaKind::ssd:
        kind = "ssd";
        break;
      case repository::MediaKind::network:
        kind = "network";
        break;
      case repository::MediaKind::removable:
        kind = "removable";
        break;
      default:
        break;
    }
    JobCommandOutcome out;
    out.value = {{"device_id", media.device_id}, {"kind", kind}};
    out.json = out.value.dump();
    return out;
  });
}
}  // namespace am::application
