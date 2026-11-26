#include "am/services/export/run.hpp"

#include <algorithm>
#include <array>
#include <mutex>
#include <set>

#include "am/foundation/log.hpp"
#include "am/foundation/unicode.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/export/names.hpp"
#include "am/services/export/selection.hpp"
#include "am/storage/db/job_store.hpp"
#include "am/storage/db/operation_log.hpp"
#include "am/storage/fs/publish.hpp"

namespace am::exporting {
namespace {
using J = ExportJson;
using Path = std::filesystem::path;
namespace jc = contracts;
namespace backend = storage::exporting;
Error error(Errc code, std::string_view id, std::string detail = {}) {
  return Error::make(code, "Export operation failed", std::move(detail)).in("services.export").id(std::string(id));
}
template <class T> T take(Result<T> result) { if (!result) throw result.error(); return std::move(*result); }
void take(Result<void> result) { if (!result) throw result.error(); }
template <class T, class F> Result<T> guard(F&& f) {
  try { return f(); }
  catch (const Error& exception) { return exception; }
  catch (const std::bad_alloc&) { return error(Errc::resource_exhausted, "am.export.out_of_memory"); }
  catch (const std::exception& exception) { return error(Errc::io_error, "am.export.io_error", exception.what()); }
  catch (...) { return error(Errc::internal,"am.export.io_error"); }
}
std::string utf8(const Path& path) { const auto bytes = path.generic_u8string(); return {bytes.begin(), bytes.end()}; }
Path path(std::string_view value) { return Path(std::u8string(value.begin(), value.end())); }
bool full(const ExportRequest& request) { return request.presets.empty() && request.file_ids.empty(); }
J failed(const Error& error) {
  J result{{"code",std::string(errc_name(error.code))},{"message_id",error.message_id}};
  if (error.cause) result["cause"] = failed(*error.cause);
  return result;
}
J parameters(const ExportRequest& request) {
  J presets = J::array();
  for (auto preset : request.presets) presets.push_back(preset_name(preset));
  J result{{"mode",full(request) ? "full" : "files"},{"presets",presets},{"file_ids",request.file_ids},
           {"target_dir",utf8(std::filesystem::absolute(request.target_dir))},
           {"on_conflict",request.on_conflict == ConflictPolicy::fail ? "fail" : "rename"},
           {"verify",request.verify_full ? "full" : "quick"}};
  if (request.name) result["name"] = *request.name;
  return result;
}
ExportRequest request_from(const J& value) {
  ExportRequest request;
  if (value.contains("asset_ids")) request.asset_ids = value.at("asset_ids").get<std::vector<std::string>>();
  else request.asset_ids = {value.at("asset_id").get<std::string>()};
  for (const auto& preset : value.at("presets")) request.presets.push_back(take(parse_preset(preset.get<std::string>())));
  request.file_ids = value.at("file_ids").get<std::vector<std::string>>();
  request.target_dir = path(value.at("target_dir").get<std::string>());
  if (value.contains("name")) request.name = value.at("name").get<std::string>();
  const auto conflict = value.at("on_conflict").get<std::string>(), verification = value.at("verify").get<std::string>();
  if ((conflict != "fail" && conflict != "rename") || (verification != "quick" && verification != "full") ||
      value.at("mode") != (full(request) ? "full" : "files"))
    throw error(Errc::invalid_argument, "am.export.arguments_invalid");
  request.on_conflict = conflict == "fail" ? ConflictPolicy::fail : ConflictPolicy::rename;
  request.verify_full = verification == "full";
  return request;
}
jobs::MediaKind media_kind(repository::MediaKind kind) {
  switch (kind) {
    case repository::MediaKind::hdd: return jobs::MediaKind::hdd;
    case repository::MediaKind::ssd: return jobs::MediaKind::ssd;
    case repository::MediaKind::network: return jobs::MediaKind::network;
    case repository::MediaKind::removable: return jobs::MediaKind::removable;
    default: return jobs::MediaKind::unknown;
  }
}
std::array<jobs::ResourceRequest, 2> resources(jobs::ResourceManager& manager, catalog::Catalog& catalog,
                                             const ExportRequest& request) {
  const auto input = repository::detect_media(catalog.library().root()), output = repository::detect_media(request.target_dir);
  manager.set_media(input.device_id, media_kind(input.kind));
  manager.set_media(output.device_id, media_kind(output.kind));
  return {{{jobs::ResourceKind::read,input.device_id},{jobs::ResourceKind::write,output.device_id}}};
}
std::string relative_file(std::string_view original) {
  if (original.starts_with("content/")) {
    const auto slash = original.find('/', 8);
    if (slash != std::string_view::npos) return std::string(original.substr(slash + 1));
  }
  return std::string(original);
}
std::string collided(std::string_view original, std::uint64_t ordinal) {
  const auto slash = original.rfind('/'), dot = original.rfind('.');
  const auto position = dot != std::string_view::npos && dot != (slash == std::string_view::npos ? 0 : slash + 1) &&
                                (slash == std::string_view::npos || dot > slash) ? dot : original.size();
  return std::string(original.substr(0,position)) + " (" + std::to_string(ordinal) + ")" + std::string(original.substr(position));
}
struct Shared {
  catalog::Catalog& catalog;
  jobs::ResourceManager& manager;
  RunHooks hooks;
  std::mutex mutex;
};
using Checkpoint = std::function<Result<void>(const std::string&)>;
J execute(catalog::Catalog& catalog, const ExportRequest& request, std::string_view asset_id,
          const std::optional<std::string>& previous, std::stop_token stop, const RunHooks& hooks,
          const Checkpoint& checkpoint) {
  take(backend::validate_target(catalog.library().root(), request.target_dir));
  if (previous) {
    const auto saved = J::parse(*previous);
    if (saved.contains("checkpoint")) {
      const auto& cp = saved.at("checkpoint");
      const auto target_name = cp.at("target_name").get<std::string>();
      const auto checked = export_name({}, asset_id, {}, false, target_name);
      if (!checked || checked->filename != target_name || cp.at("kind") != "export.publish" ||
          cp.at("result").at("asset_id").get<std::string>() != asset_id)
        throw error(Errc::corrupt_data,"am.export.checkpoint_invalid");
      if (take(backend::matches(request.target_dir / path(target_name), cp.at("size").get<std::uint64_t>(),
                                 cp.at("sha256").get<std::string>(), stop)))
        return cp.at("result");
    }
  }
  const repository::AssetRef ref{catalog.library().config().library_id,std::string(asset_id)};
  auto acquired = catalog.library().assets().snapshot(ref,true);
  if (!acquired && (acquired.error().code == Errc::corrupt_data || acquired.error().code == Errc::unsupported))
    throw error(Errc::corrupt_data,"am.export.source_invalid").caused_by(acquired.error());
  auto snapshot = take(std::move(acquired));
  if (snapshot.in_trash()) throw error(Errc::not_found,"am.export.asset_in_trash");
  if (hooks.after_snapshot) hooks.after_snapshot(snapshot);
  const auto& manifest = snapshot.manifest().known();
  std::string title;
  const auto& asset = manifest.at("asset");
  if (asset.contains("metadata")) title = asset.at("metadata").value("title",std::string{});
  std::vector<std::size_t> selection;
  if (!full(request)) selection = take(select_files(snapshot.manifest(), request.presets, request.file_ids));
  for (unsigned ordinal = 1; ordinal <= kMaxCollisionOrdinal; ++ordinal) {
    if (stop.stop_requested()) throw error(Errc::cancelled,"am.export.cancelled");
    const auto name = take(export_name(title,asset_id,request.presets,!request.file_ids.empty(),
                                       request.name ? std::optional<std::string_view>(*request.name) : std::nullopt,ordinal));
    const auto target = request.target_dir / path(name.filename);
    const auto existing = paths::node_info(target);
    if (existing) {
      if (request.on_conflict == ConflictPolicy::fail) throw error(Errc::conflict,"am.export.target_exists");
      continue;
    }
    J row{{"asset_id",asset_id},{"revision",snapshot.revision()},{"mode",full(request) ? "full" : "files"},
          {"presets",parameters(request).at("presets")},{"file_count",full(request) ? manifest.at("files").size() : selection.size()},
          {"target_name",name.filename},{"name_sanitized",name.sanitized},{"renamed",J::array()},{"status","exported"}};
    backend::PrepareOptions options;
    options.target_dir = request.target_dir; options.full = full(request); options.verify_full = request.verify_full;
    options.stop = stop; options.hooks = hooks.storage;
    std::set<std::string> used;
    const auto root = name.filename.substr(0,name.filename.size()-4);
    for (const auto index : selection) {
      const auto& file = manifest.at("files").at(index);
      const auto relative = relative_file(file.at("storage_path").get<std::string>());
      auto candidate = root + "/" + relative;
      for (std::uint64_t suffix = 2; !used.insert(unicode::path_collision_key(candidate)).second; ++suffix)
        candidate = root + "/" + collided(relative,suffix);
      take(paths::check_container_path(candidate,paths::ContainerPathProfile::plain));
      options.files.push_back({index,candidate});
      if (candidate != root + "/" + relative) row["renamed"].push_back({{"file_id",file.at("id")},{"path",candidate}});
    }
    auto prepared = take(backend::prepare(snapshot,options));
    row["sha256"] = prepared.sha256; row["size"] = prepared.size;
    const J saved{{"checkpoint",{{"kind","export.publish"},{"target_name",name.filename},{"sha256",prepared.sha256},
                                  {"size",prepared.size},{"result",row}}}};
    if (checkpoint) take(checkpoint(saved.dump()));
    if (stop.stop_requested()) throw error(Errc::cancelled,"am.export.cancelled");
    if (hooks.before_publish) hooks.before_publish(target);
    if (stop.stop_requested()) throw error(Errc::cancelled,"am.export.cancelled");
    const auto published = storage::fs::publish_no_replace(prepared.temporary,target);
    if (!published && published.error().code == Errc::conflict && request.on_conflict == ConflictPolicy::rename) continue;
    take(published);
    if (hooks.after_publish) hooks.after_publish(target);
    return row;
  }
  throw error(Errc::conflict,"am.export.target_exists");
}
void operation(catalog::Catalog& catalog, const ExportRequest& request, J row, std::optional<std::string> job = {}) {
  if (catalog.library().mode() != library::Mode::read_write) return;
  db::LogEntry entry;
  entry.level = row.at("status") == "exported" ? "info" : "error";
  entry.code = "export"; entry.asset_id = row.at("asset_id").get<std::string>(); entry.job_id = std::move(job);
  entry.message = "Asset export";
  if (!row.contains("revision")) row["revision"] = nullptr;
  if (!row.contains("mode")) row["mode"] = full(request) ? "full" : "files";
  if (!row.contains("presets")) row["presets"] = parameters(request).at("presets");
  if (!row.contains("file_count")) row["file_count"] = nullptr;
  row["target_dir"] = utf8(request.target_dir);
  entry.detail_json = row.dump();
  const auto logged = catalog.operation_log().append(entry);
  if (!logged) AM_LOG(warning,"services.export","export.log_failed","Export operation log failed",{},&logged.error());
}
J summary(const std::vector<jc::JobRecord>& records, bool queued) {
  J result{{"queued",queued},{"assets",J::array()}};
  for (const auto& record : records) {
    const auto params = J::parse(record.params_json);
    J row{{"asset_id",params.at("asset_id")},{"status",std::string(jc::job_state_name(record.state))},{"job_id",record.job_id}};
    if (record.result_json) {
      auto saved = J::parse(*record.result_json);
      if (!saved.contains("checkpoint")) row.update(saved);
    }
    if (record.state != jc::JobState::succeeded && record.error_json) row["error"] = J::parse(*record.error_json);
    result["assets"].push_back(std::move(row));
  }
  return result;
}
class ExportAsset final : public jobs::JobHandler {
 public:
  explicit ExportAsset(std::shared_ptr<Shared> state) : state_(std::move(state)) {}
  jobs::JobOutcome run(jobs::JobContext& context) override {
    try {
      auto request = request_from(J::parse(context.record().params_json));
      take(validate_request(state_->catalog,request));
      auto lease = take(context.acquire(resources(state_->manager,state_->catalog,request)));
      context.report({{}, {}, "bytes", "exporting"});
      auto row = execute(state_->catalog,request,request.asset_ids.at(0),context.record().result_json,context.stop(),
                          state_->hooks,[&](const auto& json) {
                            context.report({{}, {}, "bytes", "publishing"});
                            return context.checkpoint(json);
                          });
      { const std::lock_guard lock(state_->mutex); operation(state_->catalog,request,row,context.record().job_id); }
      return jobs::JobOutcome::success(row.dump());
    } catch (const Error& exception) {
      if (exception.code == Errc::cancelled) return jobs::JobOutcome::interrupted();
      auto outcome = jobs::JobOutcome::failure(exception);
      const auto params = J::parse(context.record().params_json);
      J row{{"asset_id",params.at("asset_id")},{"status","failed"},{"error",failed(exception)}};
      if (!context.record().result_json) outcome.result_json = row.dump();
      const auto request = request_from(params);
      const std::lock_guard lock(state_->mutex); operation(state_->catalog,request,row,context.record().job_id);
      return outcome;
    } catch (const std::bad_alloc&) { return jobs::JobOutcome::failure(error(Errc::resource_exhausted,"am.export.out_of_memory")); }
    catch (const std::exception& exception) { return jobs::JobOutcome::failure(error(Errc::io_error,"am.export.io_error",exception.what())); }
  }
 private:
  std::shared_ptr<Shared> state_;
};
class ExportRun final : public jobs::JobHandler {
 public:
  explicit ExportRun(std::shared_ptr<Shared> state) : state_(std::move(state)) {}
  jobs::JobOutcome run(jobs::JobContext& context) override {
    try {
      const auto params = J::parse(context.record().params_json);
      const auto request = request_from(params);
      auto children = context.children();
      if (children.error) return jobs::JobOutcome::failure(*children.error);
      std::set<std::string> present;
      for (const auto& child : children.records) present.insert(J::parse(child.params_json).at("asset_id").get<std::string>());
      for (const auto& id : request.asset_ids) if (!present.contains(id)) {
        auto child = parameters(request); child["asset_id"] = id;
        jc::JobSpec spec; spec.type = "export.asset"; spec.params_json = child.dump();
        take(context.enqueue_child(std::move(spec)));
      }
      children = context.children();
      if (children.error) return jobs::JobOutcome::failure(*children.error);
      if (!children.settled()) return jobs::JobOutcome::await_children();
      auto result = summary(children.records,true);
      auto outcome = jobs::JobOutcome::success(result.dump());
      for (const auto& child : children.records) if (child.state != jc::JobState::succeeded)
        outcome = jobs::JobOutcome::failure(error(Errc::io_error,"am.export.asset_failed"));
      outcome.result_json = result.dump();
      return outcome;
    } catch (const Error& exception) { return jobs::JobOutcome::failure(exception); }
    catch (const std::bad_alloc&) { return jobs::JobOutcome::failure(error(Errc::resource_exhausted,"am.export.out_of_memory")); }
    catch (const std::exception& exception) { return jobs::JobOutcome::failure(error(Errc::io_error,"am.export.io_error",exception.what())); }
  }
 private:
  std::shared_ptr<Shared> state_;
};
}  // namespace

Result<void> validate_request(catalog::Catalog& catalog, const ExportRequest& request) {
  return guard<void>([&]() -> Result<void> {
    if (request.asset_ids.empty()) return error(Errc::invalid_argument,"am.export.arguments_invalid");
    std::set<std::string> ids;
    for (const auto& id : request.asset_ids) {
      const auto parsed = Uuid::parse_canonical(id);
      if (!parsed || parsed->version() != 4 || (parsed->bytes()[8] & 0xc0) != 0x80 || !ids.insert(id).second)
        return error(Errc::invalid_argument,"am.export.arguments_invalid");
    }
    if (request.name && request.asset_ids.size() != 1) return error(Errc::invalid_argument,"am.export.name_requires_single_asset");
    if (!request.file_ids.empty() && request.asset_ids.size() != 1) return error(Errc::invalid_argument,"am.export.file_requires_single_asset");
    if (request.name) take(export_name({},request.asset_ids.front(),{},false,*request.name));
    take(backend::validate_target(catalog.library().root(),request.target_dir));
    if (!request.file_ids.empty()) {
      auto snapshot = take(catalog.library().assets().snapshot({catalog.library().config().library_id,request.asset_ids.front()},true));
      take(select_files(snapshot.manifest(),request.presets,request.file_ids));
    }
    return {};
  });
}
Result<std::string> enqueue_export(catalog::Catalog& catalog, const ExportRequest& request) {
  return guard<std::string>([&] {
    take(validate_request(catalog,request));
    auto params = parameters(request); params["asset_ids"] = request.asset_ids;
    jc::JobSpec spec; spec.type = "export.run"; spec.params_json = params.dump();
    const auto parent = take(catalog.job_store().enqueue(spec));
    for (const auto& id : request.asset_ids) {
      auto child = parameters(request); child["asset_id"] = id;
      spec.type = "export.asset"; spec.params_json = child.dump(); spec.parent_id = parent.job_id;
      take(catalog.job_store().enqueue(spec));
    }
    return parent.job_id;
  });
}
Result<J> export_summary(catalog::Catalog& catalog, std::string_view parent_id) {
  return guard<J>([&] {
    jc::JobFilter filter; filter.parent_id = std::string(parent_id);
    auto result = summary(take(catalog.job_store().list(filter)),true);
    result["job_id"] = parent_id;
    return result;
  });
}
Result<J> export_direct(catalog::Catalog& catalog, const ExportRequest& request, std::stop_token stop, RunHooks hooks) {
  return guard<J>([&] {
    take(validate_request(catalog,request));
    jobs::ResourceManager manager(jobs::profile_limits(jobs::Profile::balanced,std::thread::hardware_concurrency()));
    J result{{"queued",false},{"assets",J::array()}};
    for (const auto& id : request.asset_ids) {
      auto outcome = guard<J>([&] {
        auto lease = take(manager.acquire(resources(manager,catalog,request),false,0,stop));
        return execute(catalog,request,id,{},stop,hooks,{});
      });
      J row = outcome ? *outcome : J{{"asset_id",id},{"status","failed"},{"error",failed(outcome.error())}};
      operation(catalog,request,row);
      result["assets"].push_back(std::move(row));
    }
    return result;
  });
}
Result<void> register_export_jobs(jobs::HandlerRegistry& registry, catalog::Catalog& catalog,
                                   jobs::ResourceManager& manager, RunHooks hooks) {
  return guard<void>([&]() -> Result<void> {
    auto state = std::shared_ptr<Shared>(new Shared{catalog,manager,std::move(hooks),{}});
    take(registry.add("export.run",[state](const auto&) { return std::make_unique<ExportRun>(state); }));
    return registry.add("export.asset",[state](const auto&) { return std::make_unique<ExportAsset>(state); });
  });
}
}  // namespace am::exporting
