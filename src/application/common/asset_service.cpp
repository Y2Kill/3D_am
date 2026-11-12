#include "am/application/asset_service.hpp"
#include "am/storage/repository/repository.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/unicode.hpp"
#include "am/foundation/uuid.hpp"
#include "am/foundation/version.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace am::application {
namespace {
namespace fs = std::filesystem;
using Path = fs::path;
namespace repo = am::repository;
Error error(Errc code, std::string id, std::string detail = {}) {
  return Error::make(code, "Asset operation failed", std::move(detail)).in("application.asset").id(std::move(id));
}
[[noreturn]] void fail(Errc code, std::string id, std::string detail = {}) {
  throw error(code, std::move(id), std::move(detail));
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
    return error(Errc::resource_exhausted, "am.app.out_of_memory");
  } catch (const std::exception& e) {
    return error(Errc::io_error, "am.app.io_error", e.what());
  } catch (...) {
    return error(Errc::internal, "am.app.internal");
  }
}
std::string u8(const Path& p) {
  auto s = p.u8string();
  return {s.begin(), s.end()};
}
Path from_u8(std::string_view s) { return Path(std::u8string(s.begin(), s.end())); }
const Json& array(const Json& j, std::string_view key) {
  static const Json empty = Json::array();
  auto i = j.find(std::string(key));
  return i != j.end() && i->is_array() ? *i : empty;
}
std::string str(const Json& j, std::string_view key) {
  auto i = j.find(std::string(key));
  return i != j.end() && i->is_string() ? i->get<std::string>() : std::string{};
}
std::string now() {
  auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream s;
  s << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return s.str();
}
ExitCode report_status(const repo::VerificationReport& r, bool streams = true, bool semantics = true) {
  using C = repo::CheckStatus;
  if (r.v1 == C::failed || r.v2 == C::failed || (semantics && r.v3 == C::failed) || r.error)
    return ExitCode::check_failed;
  if (r.format_support == repo::FormatSupport::newer_minor || r.v1 == C::partial || r.v2 == C::partial ||
      (semantics && (r.v3 == C::partial || !r.v3_complete)))
    return ExitCode::partial;
  if (streams && r.container_verification == repo::ContainerVerification::partial) return ExitCode::partial;
  return ExitCode::success;
}
AssetOutcome outcome(const repo::VerificationReport& r, bool streams = true) {
  AssetOutcome out;
  out.value = take(repo::verification_json(r));
  out.status = report_status(r, streams);
  if (r.error) out.diagnostics.push_back(*r.error);
  return out;
}
void publish(const Path& source, const Path& target) {
#ifdef _WIN32
  if (!MoveFileExW(paths::native(source).c_str(), paths::native(target).c_str(), MOVEFILE_WRITE_THROUGH)) {
    auto code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS || fs::exists(paths::native(target)))
      fail(Errc::conflict, "am.app.output_exists");
    fail(Errc::io_error, "am.app.io_error", "MoveFileExW OS error=" + std::to_string(code));
  }
#else
  // link() publishes without replacement; both files are on the same volume.
  if (::link(source.c_str(), target.c_str()) != 0) {
    if (errno == EEXIST) fail(Errc::conflict, "am.app.output_exists");
    fail(Errc::io_error, "am.app.io_error", "link errno=" + std::to_string(errno));
  }
  if (::unlink(source.c_str()) != 0) fail(Errc::io_error, "am.app.io_error", "unlink errno=" + std::to_string(errno));
#endif
}
struct DirectoryCleanup {
  Path path, parent;
  ~DirectoryCleanup() {
    try {
      auto absolute = fs::absolute(path).lexically_normal();
      if (absolute.parent_path() == parent && absolute.filename().string().starts_with(".am-create-")) {
        std::error_code ec;
        fs::remove_all(paths::native(absolute), ec);
      }
    } catch (...) {
    }
  }
};
struct OutputCleanup {
  Path path;
  bool owned = false;
  ~OutputCleanup() {
    if (owned) {
      std::error_code ec;
      fs::remove(paths::native(path), ec);
    }
  }
};
std::string digest(const asset_zip::Sha256Digest& d) {
  std::string out;
  constexpr char digits[] = "0123456789abcdef";
  for (auto b : d) {
    auto n = std::to_integer<unsigned>(b);
    out += digits[n >> 4];
    out += digits[n & 15];
  }
  return out;
}
bool within(const Path& root, const Path& child) {
  auto a = root.begin(), b = child.begin();
  for (; a != root.end(); ++a, ++b)
    if (b == child.end() || *a != *b) return false;
  return b != child.end();
}
Path destination(const Path& root, std::string_view name) {
  if (!paths::check_container_path(name)) fail(Errc::corrupt_data, "am.app.unsafe_extract_path", std::string(name));
  auto path = (root / from_u8(name)).lexically_normal();
  if (!within(root, path) || !within(root, fs::weakly_canonical(path)))
    fail(Errc::corrupt_data, "am.app.unsafe_extract_path", std::string(name));
  return path;
}
void write_bytes(const Path& target, std::string_view data) {
  fs::create_directories(paths::native(target.parent_path()));
  auto temporary = target.parent_path() / from_u8(".am-extract-" + Uuid::v4().to_string() + ".tmp");
  auto sink = take(asset_zip::file_sink(temporary));
  try {
    take(sink->write_at(0, std::as_bytes(std::span(data))));
    take(sink->flush());
    take(sink->close());
    publish(temporary, target);
  } catch (...) {
    (void)sink->discard();
    throw;
  }
}
void extract_file(const asset_zip::ZipArchive& zip, std::size_t index, const Json& file, const Path& target,
                  asset_zip::ReadBudget& budget) {
  fs::create_directories(paths::native(target.parent_path()));
  auto temporary = target.parent_path() / from_u8(".am-extract-" + Uuid::v4().to_string() + ".tmp");
  auto sink = take(asset_zip::file_sink(temporary));
  try {
    auto stream = take(zip.open_entry(index, budget, true));
    std::array<std::byte, 65536> block{};
    std::uint64_t offset = 0;
    for (;;) {
      auto n = take(stream.read(block));
      if (!n) break;
      take(sink->write_at(offset, std::span(block).first(n)));
      offset += n;
    }
    auto verified = stream.verification();
    auto size = str(file, "size_bytes");
    std::uint64_t expected = 0;
    auto parsed = std::from_chars(size.data(), size.data() + size.size(), expected);
    if (!verified || !verified->sha256 || parsed.ec != std::errc{} || parsed.ptr != size.data() + size.size() ||
        verified->size != expected || digest(*verified->sha256) != str(file, "sha256"))
      fail(Errc::corrupt_data, "am.app.extract_hash_mismatch", str(file, "id"));
    take(sink->flush());
    take(sink->close());
    publish(temporary, target);
  } catch (...) {
    (void)sink->discard();
    throw;
  }
}
}  // namespace
ExitCode error_exit_code(const Error& e) noexcept {
  if (e.message_id == "am.app.selective_requires_file") return ExitCode::usage;
  if (e.message_id == "am.app.post_create_verification" || e.message_id == "am.app.extract_hash_mismatch" ||
      e.message_id == "am.app.unsafe_extract_path" || e.message_id.starts_with("am.manifest."))
    return ExitCode::check_failed;
  return ExitCode::operation_failed;
}
Result<Json> error_json(const Error& e) {
  return guard<Json>(
      [&] { return Json{{"code", errc_name(e.code)}, {"message_id", e.message_id}, {"detail", e.ctx.detail}}; });
}
Result<AssetOutcome> create_asset(const CreateAssetOptions& options) {
  return guard<AssetOutcome>([&] {
    auto output = fs::absolute(options.output).lexically_normal();
    if (fs::exists(paths::native(output))) fail(Errc::conflict, "am.app.output_exists");
    if (options.files.empty()) fail(Errc::invalid_argument, "am.app.no_input_files");
    if (!fs::is_directory(paths::native(output.parent_path())))
      fail(Errc::not_found, "am.app.output_parent_missing", u8(output.parent_path()));
    auto input_id = Uuid::v4().to_string();
    auto prefix = "content/" + input_id + "/";
    auto mapper = take(paths::ZipPathMapper::create(prefix));
    std::set<std::string> names;
    repo::AddFiles add;
    add.input = {{"id", input_id},           {"imported_at", now()},
                 {"mode", "strict"},         {"completeness", "complete"},
                 {"objects", Json::array()}, {"entries", Json::array()}};
    for (const auto& file : options.files) {
      auto source = fs::absolute(file).lexically_normal();
      if (fs::is_directory(paths::native(source)))
        fail(Errc::invalid_argument, "am.app.directory_not_supported", u8(source));
      auto name = u8(source.filename());
      if (!names.insert(unicode::path_collision_key(name)).second) fail(Errc::conflict, "am.repo.path_collision", name);
      auto mapped = mapper.map(name);
      if (mapped.reason == paths::RenameReason::collision) fail(Errc::conflict, "am.repo.path_collision", name);
      repo::FileInput entry;
      entry.path = mapped.zip_path.substr(prefix.size());
      entry.source = source;
      add.files.push_back(std::move(entry));
    }
    const auto parent = output.parent_path();
    const auto temporary = parent / from_u8(".am-create-" + Uuid::v4().to_string());
    if (!fs::create_directory(paths::native(temporary)))
      fail(Errc::io_error, "am.app.io_error", "Could not create staging library");
    DirectoryCleanup cleanup{temporary, parent};
    OutputCleanup remove_output{output};
    std::string asset_id;
    std::uint64_t revision = 0;
    {
      repo::RepositoryOptions settings;
      settings.library_id = Uuid::v4().to_string();
      settings.application_version = kAppVersion;
      auto repository = take(repo::AssetRepository::open(temporary, std::move(settings)));
      repo::CreateRequest request;
      request.files.push_back(std::move(add));
      if (options.title) request.metadata = Json{{"title", *options.title}};
      auto created = take(repository.create(request));
      asset_id = created.ref.asset_id;
      revision = created.revision;
    }
    auto created_zip = temporary / "assets" / asset_id.substr(asset_id.size() - 2) / (asset_id + ".zip");
    if (options.before_publish) options.before_publish(output);
    publish(created_zip, output);
    remove_output.owned = true;
    if (options.after_publish) options.after_publish(output);
    auto report = repo::verify_container(output);
    if (!report || report_status(*report) == ExitCode::check_failed) {
      auto e = error(Errc::corrupt_data, "am.app.post_create_verification");
      if (!report)
        e = std::move(e).caused_by(report.error());
      else if (report->error)
        e = std::move(e).caused_by(*report->error);
      throw e;
    }
    AssetOutcome result;
    result.status = report_status(*report);
    result.value = {
        {"asset_id", asset_id}, {"revision", revision}, {"file_count", options.files.size()}, {"path", u8(output)}};
    remove_output.owned = false;
    return result;
  });
}
Result<AssetOutcome> inspect_asset(const Path& path) {
  return guard<AssetOutcome>([&] {
    repo::ContainerVerifyOptions settings;
    settings.streams = false;
    settings.semantics = false;
    repo::ContainerContents contents;
    auto report = take(repo::verify_container(path, settings, &contents));
    if (report.error) {
      auto failed = outcome(report, false);
      failed.value = Json{{"v1",failed.value.at("v1")},{"v2",failed.value.at("v2")},
                          {"error",take(error_json(*report.error))}};
      return failed;
    }
    if (!contents.document) fail(Errc::corrupt_data, "am.manifest.manifest_count");
    const auto& j = contents.document->known();
    const auto& asset = j.at("asset");
    AssetOutcome out;
    out.status = report_status(report, false, false);
    out.value = {
        {"format", j.at("format")},
        {"format_support", report.format_support == repo::FormatSupport::supported ? "supported" : "newer_minor"},
        {"v1", "passed"},
        {"v2", report.format_support == repo::FormatSupport::supported ? "passed" : "not_applicable_newer_minor"},
        {"v3", "not_performed"},
        {"asset_id", asset.at("id")},
        {"revision", asset.at("revision")},
        {"created_at", str(asset, "created_at")},
        {"modified_at", str(asset, "modified_at")},
        {"title", asset.contains("metadata") ? str(asset["metadata"], "title") : std::string{}},
        {"file_count", array(j, "files").size()},
        {"historical_file_count", array(j, "historical_files").size()},
        {"input_count", array(j, "inputs").size()},
        {"history_entry_count", j.contains("history") ? array(j["history"], "entries").size() : 0},
        {"files", Json::array()}};
    for (const auto& file : array(j, "files"))
      out.value["files"].push_back({{"id", str(file, "id")},
                                    {"path", str(file, "storage_path")},
                                    {"size_bytes", str(file, "size_bytes")},
                                    {"origin_kind", str(file, "origin_kind")}});
    return out;
  });
}
Result<AssetOutcome> validate_asset(const Path& path) {
  return guard<AssetOutcome>([&] {
    repo::ContainerVerifyOptions options;
    options.streams = false;
    return outcome(take(repo::verify_container(path, options)), false);
  });
}
Result<AssetOutcome> verify_asset(const Path& path, const VerifyAssetOptions& requested) {
  return guard<AssetOutcome>([&] {
    if (requested.level == VerificationLevel::selective && requested.file_ids.empty())
      fail(Errc::invalid_argument, "am.app.selective_requires_file");
    repo::ContainerVerifyOptions options;
    options.level = requested.level == VerificationLevel::quick       ? repo::VerifyLevel::quick
                    : requested.level == VerificationLevel::selective ? repo::VerifyLevel::selective
                                                                      : repo::VerifyLevel::full;
    options.file_ids = requested.file_ids;
    return outcome(take(repo::verify_container(path, options)));
  });
}
Result<AssetOutcome> extract_asset(const Path& path, const ExtractAssetOptions& requested) {
  return guard<AssetOutcome>([&] {
    repo::ContainerVerifyOptions options;
    options.streams = false;
    repo::ContainerContents contents;
    auto report = take(repo::verify_container(path, options, &contents));
    if (report_status(report, false) == ExitCode::check_failed) return outcome(report, false);
    if (!contents.document) fail(Errc::corrupt_data, "am.manifest.manifest_count");
    const auto& j = contents.document->known();
    std::set<std::string> wanted(requested.file_ids.begin(), requested.file_ids.end()), ids;
    for (const auto& file : array(j, "files")) ids.insert(str(file, "id"));
    for (const auto& id : wanted)
      if (!ids.contains(id)) fail(Errc::not_found, "am.app.unknown_file_id", id);
    auto root = fs::weakly_canonical(fs::absolute(requested.directory));
    if (fs::exists(paths::native(root)) &&
        (!fs::is_directory(paths::native(root)) || !fs::is_empty(paths::native(root))))
      fail(Errc::conflict, "am.app.target_not_empty");
    // Validate every destination before creating even the first directory.
    for (const auto& file : array(j, "files"))
      if (wanted.empty() || wanted.contains(str(file, "id"))) (void)destination(root, str(file, "storage_path"));
    fs::create_directories(paths::native(root));
    AssetOutcome out;
    out.status = report_status(report, false);
    out.value = {{"path", u8(root)},           {"file_count", 0},
                 {"extracted", Json::array()}, {"failed", Json::array()},
                 {"manifest_written", false},  {"verification", take(repo::verification_json(report))}};
    std::map<std::string, std::size_t> entries;
    for (const auto& entry : contents.archive->entries()) entries.emplace(entry.raw_name, entry.index);
    for (const auto& file : array(j, "files")) {
      auto id = str(file, "id"), name = str(file, "storage_path");
      if (!wanted.empty() && !wanted.contains(id)) continue;
      auto extracted = guard<void>([&]() -> Result<void> {
        auto entry = entries.find(name);
        if (entry == entries.end()) fail(Errc::corrupt_data, "am.manifest.missing_stream", id);
        extract_file(*contents.archive, entry->second, file, destination(root, name), *contents.budget);
        return {};
      });
      if (extracted)
        out.value["extracted"].push_back({{"id", id}, {"path", name}});
      else {
        out.value["failed"].push_back({{"id", id}, {"error", take(error_json(extracted.error()))}});
        out.diagnostics.push_back(extracted.error());
        if (extracted.error().code == Errc::corrupt_data)
          out.status = ExitCode::check_failed;
        else if (out.status != ExitCode::check_failed)
          out.status = ExitCode::operation_failed;
      }
    }
    if (requested.manifest) {
      write_bytes(destination(root, "manifest.json"), contents.manifest_text);
      out.value["manifest_written"] = true;
    }
    out.value["file_count"] = out.value["extracted"].size();
    return out;
  });
}
}  // namespace am::application
