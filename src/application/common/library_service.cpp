#include "am/application/library_service.hpp"
#include "am/storage/library/library.hpp"
#include "am/foundation/version.hpp"
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
}  // namespace am::application
