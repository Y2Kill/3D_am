#include "am/application/import_service.hpp"
#include "am/services/import/run.hpp"

#include <fstream>
#include <toml++/toml.hpp>

#include "am/foundation/paths.hpp"
#include "am/foundation/unicode.hpp"
#include "am/foundation/uuid.hpp"
#include "am/storage/asset_zip/sha256.hpp"
#include "am/storage/asset_zip/sink.hpp"
#include "profile_default.hpp"
#include "profile_telegram.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace am::application {
namespace {
namespace fs = std::filesystem;
using J = import::PlanJson;
Error error(std::string id, std::string at = {}, Errc code = Errc::invalid_argument) {
  return Error::make(code, "Import request failed", std::move(at)).id(std::move(id));
}
template <class T>
T take(Result<T> value) {
  if (!value) throw value.error();
  return std::move(*value);
}
void take(Result<void> value) {
  if (!value) throw value.error();
}
J value(const toml::node& node, const std::string& path) {
  if (const auto* table = node.as_table()) {
    J out = J::object();
    for (const auto& [key, item] : *table)
      out[std::string(key.str())] = value(item, path + "/" + std::string(key.str()));
    return out;
  }
  if (const auto* array = node.as_array()) {
    J out = J::array();
    for (const auto& item : *array) out.push_back(value(item, path + "/" + std::to_string(out.size())));
    return out;
  }
  if (auto text = node.value<std::string>()) return *text;
  if (node.is_integer()) return *node.value<std::int64_t>();
  if (node.is_floating_point()) return *node.value<double>();
  if (node.is_boolean()) return *node.value<bool>();
  throw error("am.import.profile_invalid", path);
}
std::string digest(std::string_view text) {
  auto state = take(asset_zip::Sha256::create());
  take(state.update(std::as_bytes(std::span(text.data(), text.size()))));
  auto bytes = take(state.finish());
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (auto byte : bytes) {
    const auto b = std::to_integer<unsigned>(byte);
    out += digits[b >> 4];
    out += digits[b & 15];
  }
  return out;
}
std::string embedded(std::string_view id) {
  if (id == "default") return {reinterpret_cast<const char*>(profile_default), sizeof(profile_default)};
  return {reinterpret_cast<const char*>(profile_telegram), sizeof(profile_telegram)};
}
}  // namespace
Result<import::ImportProfile> load_import_profile(std::string_view selected) {
  try {
    std::string source;
    if (selected == "default" || selected == "telegram")
      source = embedded(selected);
    else {
      std::ifstream input(paths::native(fs::path(std::u8string(selected.begin(), selected.end()))), std::ios::binary);
      if (!input) return error("am.import.profile_invalid", "");
      source.assign(std::istreambuf_iterator<char>(input), {});
      if (input.bad()) return error("am.import.profile_invalid", "");
    }
    import::ImportProfile profile{value(toml::parse(source), ""), digest(source)};
    take(import::validate_profile(profile));
    auto defaults = value(toml::parse(embedded("default")), "");
    // Missing optional settings inherit the documented built-in hypotheses.
    defaults.merge_patch(profile.settings);
    profile.settings = std::move(defaults);
    take(import::validate_profile(profile));
    return profile;
  } catch (const Error& e) {
    return e;
  } catch (const std::exception&) {
    return error("am.import.profile_invalid");
  } catch (...) {
    return error("am.import.profile_invalid");
  }
}
Result<AssetOutcome> scan_import(import::ScanRequest request, const fs::path& output) {
  fs::path temporary;
  bool own = false;
  auto cleanup = [&] {
    if (own) {
      std::error_code ignored;
      fs::remove(paths::native(temporary), ignored);
    }
  };
  try {
    if (output.empty()) return error("am.import.arguments_invalid");
    take(import::validate_scan_paths(request, output));
    if (fs::exists(paths::native(output))) return error("am.import.output_exists", {}, Errc::conflict);
    auto plan = take(import::scan(request));
    auto content = import::serialize_plan(plan);
    if (content.empty()) return error("am.import.out_of_memory", {}, Errc::resource_exhausted);
    take(import::parse_plan(content));
    if (request.cancelled && request.cancelled()) return error("am.import.cancelled", {}, Errc::cancelled);
    temporary = fs::absolute(output).parent_path() / (".am-import-plan-" + Uuid::v4().to_string() + ".tmp");
    {
      auto sink = take(asset_zip::file_sink(temporary));
      own = true;
      take(sink->write_at(0, std::as_bytes(std::span(content.data(), content.size()))));
      take(sink->flush());
      take(sink->close());
    }
    if (request.cancelled && request.cancelled()) {
      cleanup();
      return error("am.import.cancelled", {}, Errc::cancelled);
    }
#ifdef _WIN32
    if (!MoveFileW(paths::native(temporary).c_str(), paths::native(output).c_str()))
      throw error("am.import.output_unavailable", {}, Errc::io_error);
#else
    if (::link(temporary.c_str(), output.c_str()) != 0) throw error("am.import.output_unavailable", {}, Errc::io_error);
    if (::unlink(temporary.c_str()) != 0) throw error("am.import.output_unavailable", {}, Errc::io_error);
#endif
    own = false;
    AssetOutcome out;
    J summary = {{"nodes", plan.value["nodes"].size()},
                 {"candidates", plan.value["candidates"].size()},
                 {"kinds", J::object()},
                 {"actions", J::object()},
                 {"excluded", 0},
                 {"ask", 0},
                 {"matches", 0},
                 {"completeness", plan.value["scan"]["completeness"]}};
    summary["archives"]=plan.value["archives"].size();
    summary["archive_entries"]=plan.value["archive_entries"].size();
    for (const auto& c : plan.value["candidates"]) {
      const auto kind = c["kind"].get<std::string>(), action = c["action"].get<std::string>();
      summary["kinds"][kind] = summary["kinds"].value(kind, std::uint64_t{}) + 1;
      summary["actions"][action] = summary["actions"].value(action, std::uint64_t{}) + 1;
      summary["matches"] = summary["matches"].get<std::uint64_t>() + c["matches"].size();
      for (const auto& e : c["entries"]) {
        if (e["decision"] == "exclude") summary["excluded"] = summary["excluded"].get<std::uint64_t>() + 1;
        if (e["decision"] == "ask") summary["ask"] = summary["ask"].get<std::uint64_t>() + 1;
      }
    }
    out.value = Json::parse(summary.dump());
    return out;
  } catch (const Error& e) {
    cleanup();
    return e;
  } catch (const std::exception& e) {
    cleanup();
    return error("am.import.output_unavailable", e.what(), Errc::io_error);
  } catch (...) {
    cleanup();
    return error("am.import.output_unavailable", {}, Errc::io_error);
  }
}
Result<AssetOutcome> run_import(const fs::path& plan,const fs::path& library,bool enqueue_only,
                                const JobServiceOptions& options,std::stop_token stop) {
  try {
    take(import::validate_run_plan(plan,library));
    auto service=take(JobService::open(library,options));
    const auto queued=take(import::enqueue_run(service.catalog(),plan));
    if(!enqueue_only && !queued.reused) take(service.run(stop));
    auto summary=take(import::run_summary(service.catalog(),queued));
    AssetOutcome out; out.value=Json::parse(summary.dump()); out.diagnostics=service.diagnostics();
    for(const auto& row:summary.at("candidates")) {
      const auto status=row.at("status").get<std::string>();
      if(status=="failed") out.status=ExitCode::operation_failed;
      else if(out.status!=ExitCode::operation_failed && (status=="deferred" || status=="cancelled" || (!enqueue_only && status=="already_queued"))) out.status=ExitCode::partial;
    }
    if(stop.stop_requested() && out.status==ExitCode::success) out.status=ExitCode::partial;
    return out;
  } catch(const Error& e) { return e; }
  catch(const std::exception& e) { return error("am.import.run.failed",e.what(),Errc::io_error); }
}
}  // namespace am::application
