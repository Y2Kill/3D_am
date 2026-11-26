#include "am/storage/repository/repository.hpp"
#include "am/storage/asset_zip/probe.hpp"
#include "am/storage/archive/zip_input.hpp"
#include "am/foundation/json_text.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/unicode.hpp"
#include "am/foundation/uuid.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#endif

namespace am::repository {
namespace {
using U64 = std::uint64_t;
using Path = std::filesystem::path;
bool is_independent_copy(const AssetSnapshot&, const manifest::CodecLimits&);
constexpr std::string_view format_id = "urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128";
Error error(Errc code, std::string id, std::string detail) {
  return Error::make(code, "Asset repository operation failed", std::move(detail)).in("storage.repository").op("asset.repository").id(std::move(id));
}
std::string diagnostic_name(std::string_view name) {
  if(unicode::is_valid_utf8(name))return std::string(name);
  // Preserve invalid ZIP-name bytes in a diagnostic that remains valid UTF-8.
  std::string out;constexpr char hex[]="0123456789ABCDEF";
  for(unsigned char byte:name) {
    if(byte>=0x20&&byte<0x7f&&byte!='\\')out+=static_cast<char>(byte);
    else {out+="\\x";out+=hex[byte>>4];out+=hex[byte&15];}
  }
  return out;
}
[[noreturn]] void fail(Errc code, std::string id, std::string detail) { throw error(code, std::move(id), std::move(detail)); }
template<class T> T take(Result<T> r) { if (!r) throw r.error(); return std::move(r).value(); }
void take(Result<void> r) { if (!r) throw r.error(); }
template<class T, class F> Result<T> guard(F&& f) {
  try { return f(); } catch (const Error& e) {
    const auto& id = e.message_id;
    std::string mapped;
    if (id == "am.commit.locked") mapped = "am.repo.locked";
    else if (id == "am.commit.insufficient_space") mapped = "am.repo.insufficient_space";
    else if (id == "am.commit.replace_failed") mapped = "am.repo.target_in_use";
    else if (id == "am.commit.journal_error" || id == "am.commit.journal_version") mapped = "am.repo.journal_error";
    else if (id == "am.commit.out_of_memory" || (id.starts_with("am.zip.") && e.code == Errc::resource_exhausted)) mapped = "am.repo.limit_exceeded";
    else if (id.starts_with("am.zip.") && e.code == Errc::corrupt_data) mapped = "am.repo.corrupt_container";
    else if (id.starts_with("am.zip.") && e.code == Errc::unsupported) mapped = "am.repo.unsupported_container";
    else if (id.starts_with("am.zip.") || (id.starts_with("am.commit.") && id != "am.commit.base_changed" && id != "am.commit.verification_failed")) mapped = "am.repo.io_error";
    if (!mapped.empty()) return error(e.code,std::move(mapped),e.ctx.detail).caused_by(e);
    return e;
  }
  catch (const std::bad_alloc&) { return error(Errc::resource_exhausted, "am.repo.out_of_memory", "Allocation failed"); }
  catch (const std::exception& e) { return error(Errc::io_error, "am.repo.io_error", e.what()); }
  catch (...) { return error(Errc::internal, "am.repo.callback_failed", "Unknown callback exception"); }
}
bool uuid4(std::string_view s) { auto id = Uuid::parse_canonical(s); return id && id->version() == 4; }
std::string identifier(const std::optional<std::string>& s) {
  auto value = s.value_or(Uuid::v4().to_string()); if (!uuid4(value)) fail(Errc::invalid_argument, "am.repo.invalid_operation", "Identifier must be UUIDv4"); return value;
}
const Json& empty_array() { static const Json value = Json::array(); return value; }
const Json& array(const Json& j, std::string_view key) {
  const auto it = j.find(std::string(key)); return it != j.end() && it->is_array() ? *it : empty_array();
}
std::string string(const Json& j, std::string_view key) { auto it = j.find(std::string(key)); return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{}; }
std::optional<U64> number(const Json& j) {
  if (j.is_number_unsigned()) return j.get<U64>();
  if (j.is_number_integer() && j.get<std::int64_t>() >= 0) return static_cast<U64>(j.get<std::int64_t>());
  return {};
}
std::optional<U64> decimal(const Json& j, std::string_view key) {
  auto text = string(j, key); U64 out = 0; auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) return {}; return out;
}
U64 revision(const Json& j) { if (!j.contains("asset") || !j["asset"].is_object() || !j["asset"].contains("revision")) return 0; return number(j["asset"]["revision"]).value_or(0); }
std::string hex(const asset_zip::Sha256Digest& digest) {
  constexpr char chars[] = "0123456789abcdef"; std::string out;
  for (auto b : digest) { auto n = std::to_integer<unsigned>(b); out += chars[n >> 4]; out += chars[n & 15]; } return out;
}
std::string hash_text(std::string_view text) { auto sha = take(asset_zip::Sha256::create()); take(sha.update(std::as_bytes(std::span(text)))); return hex(take(sha.finish())); }
std::string timestamp(std::chrono::system_clock::time_point now) {
  const auto rounded = std::chrono::floor<std::chrono::seconds>(now); const auto seconds = std::chrono::system_clock::to_time_t(rounded); std::tm time{};
#ifdef _WIN32
  gmtime_s(&time, &seconds);
#else
  gmtime_r(&seconds, &time);
#endif
  std::ostringstream out; out << std::put_time(&time, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << std::chrono::duration_cast<std::chrono::milliseconds>(now - rounded).count() << 'Z'; return out.str();
}
struct ManifestRead { std::string text; manifest::ManifestDocument document; };
ManifestRead read_manifest(const asset_zip::ZipArchive& zip, const manifest::CodecLimits& limits, asset_zip::ReadBudget& budget) {
  const asset_zip::EntryInfo* found = nullptr;
  for (const auto& e : zip.entries()) if (e.raw_name == "manifest.json") { if (found) fail(Errc::corrupt_data, "am.manifest.manifest_count", "Multiple root manifests"); found = &e; }
  if (!found) fail(Errc::corrupt_data, "am.manifest.manifest_count", "Root manifest missing");
  if (found->uncompressed_size > limits.max_bytes) fail(Errc::resource_exhausted, "am.manifest.limit_exceeded", "Manifest exceeds codec byte limit");
  auto stream = take(zip.open_entry(found->index, budget, true)); std::array<std::byte, 65536> buffer{}; std::string text;
  for (;;) { auto count = take(stream.read(buffer)); if (!count) break; text.append(reinterpret_cast<const char*>(buffer.data()), count); }
  auto doc = take(manifest::parse_manifest(text, limits)); return {std::move(text), std::move(doc)};
}
FormatSupport classify(const Json& j) {
  if (!j.contains("format") || !j["format"].is_object()) fail(Errc::corrupt_data, "am.manifest.invalid_format", "Missing format");
  const auto& f = j["format"];
  if (string(f, "id") != format_id || !f.contains("version") || !f["version"].is_object()) fail(Errc::unsupported, "am.repo.unsupported_version", "Unknown container format");
  const auto& v = f["version"];
  if (!v.contains("major") || !v.contains("minor") || number(v["major"]) != 1 || !number(v["minor"])) fail(Errc::unsupported, "am.repo.unsupported_version", "Unsupported container version");
  return *number(v["minor"]) == 0 ? FormatSupport::supported : FormatSupport::newer_minor;
}
struct Registered { std::string id, path, sha; std::optional<U64> size; };
std::vector<Registered> registered(const Json& j) {
  std::vector<Registered> out;
  for (const auto& f : array(j, "files")) out.push_back({string(f,"id"), string(f,"storage_path"), string(f,"sha256"), decimal(f,"size_bytes")});
  if (j.contains("extensions") && j["extensions"].is_object()) for (const auto& ext : j["extensions"])
    for (const auto& f : array(ext, "attachments")) out.push_back({string(f,"id"), string(f,"path"), string(f,"sha256"), decimal(f,"size_bytes")});
  return out;
}
std::set<std::string> opaque_paths(const Json& j, const asset_zip::ZipArchive& zip) {
  std::set<std::string> names{"manifest.json"}; for (const auto& f : registered(j)) names.insert(f.path);
  std::set<std::string> out; for (const auto& e : zip.entries()) if (!names.contains(e.raw_name)) out.insert(e.raw_name); return out;
}
class Invariants final : public SemanticValidator {
 public:
  explicit Invariants(bool full = false) : full_(full) {}
  Result<SemanticResult> validate(const manifest::ManifestDocument& doc, const asset_zip::ZipArchive& zip, const SemanticContext& context) const override {
    return guard<SemanticResult>([&] {
      SemanticResult result;
      if (full_) static_cast<manifest::SemanticReport&>(result) = take(manifest::validate_semantics(doc));
      const auto& j = doc.known();
      auto check = [&](bool passed, std::string_view rule, std::string pointer, std::string detail = "Repository invariant failed") {
        if (!passed) result.findings.push_back({std::string(rule),manifest::FindingSeverity::error,std::move(pointer),"am.manifest."+std::string(rule),std::move(detail)});
      };
      if (!context.ref.asset_id.empty()) {
      result.checked_rules.push_back("format_asset_identity");
      check(classify(j) == FormatSupport::supported && j.contains("asset") && string(j["asset"],"id") == context.ref.asset_id,"format_asset_identity","/asset/id");
      }
      result.checked_rules.push_back("zip_paths");
      result.checked_rules.push_back("zip_entry_kind");
      std::set<std::string> used;
      for (const auto& e : zip.entries()) {
        check(e.raw_name.size() <= 200 && paths::check_container_path(e.raw_name).ok() && used.insert(unicode::path_collision_key(e.raw_name)).second,"zip_paths","",diagnostic_name(e.raw_name));
        const char* kind = e.kind == asset_zip::EntryKind::symlink ? "symlink" :
            e.kind == asset_zip::EntryKind::directory ? "directory" : "special";
        check(e.kind == asset_zip::EntryKind::regular,"zip_entry_kind","",diagnostic_name(e.raw_name)+" ("+kind+")");
      }
      if (context.new_operation_id) {
        result.checked_rules.push_back("history_operation_id");
        const auto& h = j.contains("history") ? array(j["history"],"entries") : empty_array();
        check(!h.empty() && string(h.back(),"operation_id") == *context.new_operation_id,"history_operation_id","/history/entries");
      }
      return result;
    });
  }
 private:
  bool full_;
};
const char* severity_name(manifest::FindingSeverity s) {
  switch (s) { case manifest::FindingSeverity::error:return "error";case manifest::FindingSeverity::warning:return "warning";default:return "unverifiable"; }
}
const char* status_name(CheckStatus s) { switch(s) { case CheckStatus::passed:return "passed"; case CheckStatus::partial:return "partial"; case CheckStatus::failed:return "failed"; case CheckStatus::not_applicable_newer_minor:return "not_applicable_newer_minor"; default:return "not_performed"; } }
const char* stream_name(StreamStatus s) { switch(s) { case StreamStatus::verified_sha256:return "verified_sha256"; case StreamStatus::verified_crc_size_only:return "verified_crc_size_only"; case StreamStatus::unverifiable:return "unverifiable"; case StreamStatus::unsupported:return "unsupported"; default:return "failed"; } }
} // namespace
Result<std::shared_ptr<const SemanticValidator>> repository_invariants() { return guard<std::shared_ptr<const SemanticValidator>>([] { return std::shared_ptr<const SemanticValidator>(std::make_shared<Invariants>()); }); }
Result<Json> verification_json(const VerificationReport& r) {
  return guard<Json>([&] { Json out{{"format_support", r.format_support == FormatSupport::supported ? "supported" : "newer_minor"},{"v1",status_name(r.v1)},{"v2",status_name(r.v2)},{"v3",status_name(r.v3)},{"v3_rules",r.v3_rules},{"v3_complete",r.v3_complete},{"v3_findings",Json::array()},{"v3_deferred_rules",Json::array()},{"plugin_validation",status_name(r.plugin_validation)},{"container_verification",r.container_verification == ContainerVerification::full ? "full" : "partial"},{"v4",Json::array()}};
    for (const auto& f : r.v3_findings) out["v3_findings"].push_back({{"rule",f.rule},{"severity",severity_name(f.severity)},{"pointer",f.pointer},{"message_id",f.message_id},{"detail",f.detail}});
    for (const auto& d : r.v3_deferred_rules) out["v3_deferred_rules"].push_back({{"rule",d.rule},{"reason",d.reason}});
    for (const auto& s : r.v4) out["v4"].push_back({{"id",s.id},{"path",diagnostic_name(s.path)},{"status",stream_name(s.status)}}); if (r.error) out["error"] = r.error->message_id; return out; });
}
Result<StorageCapabilities> classify_storage_capabilities(DriveKind drive, std::string_view filesystem) {
  return guard<StorageCapabilities>([&] {
    StorageCapabilities caps; caps.filesystem = filesystem; caps.remote = drive == DriveKind::remote;
    if (filesystem == "FAT32") caps.max_file_size = 0xffffffffULL;
    if (drive == DriveKind::fixed && filesystem == "NTFS") {
      caps.mode = StorageMode::full; caps.posix_rename = true; caps.replace_file = true;
      caps.locks_reliable = true; caps.durability = Durability::assumed;
    }
    return caps;
  });
}
Result<StorageCapabilities> detect_storage_capabilities(const Path& root) {
  return guard<StorageCapabilities>([&] {
    StorageCapabilities caps;
#ifdef _WIN32
    std::array<wchar_t,32768> volume{}; std::array<wchar_t,64> fs{};
    const auto path = paths::native(std::filesystem::absolute(root));
    if (!GetVolumePathNameW(path.c_str(),volume.data(),static_cast<DWORD>(volume.size())) || !GetVolumeInformationW(volume.data(),nullptr,0,nullptr,nullptr,nullptr,fs.data(),static_cast<DWORD>(fs.size()))) fail(Errc::io_error,"am.repo.io_error","Volume information query failed");
    const auto drive_type = GetDriveTypeW(volume.data()); caps.remote = drive_type == DRIVE_REMOTE; const std::wstring name(fs.data());
    const auto bytes = WideCharToMultiByte(CP_UTF8,0,name.data(),static_cast<int>(name.size()),nullptr,0,nullptr,nullptr);
    if (bytes <= 0) fail(Errc::io_error,"am.repo.io_error","Filesystem name conversion failed"); caps.filesystem.resize(static_cast<std::size_t>(bytes));
    if (!WideCharToMultiByte(CP_UTF8,0,name.data(),static_cast<int>(name.size()),caps.filesystem.data(),bytes,nullptr,nullptr)) fail(Errc::io_error,"am.repo.io_error","Filesystem name conversion failed");
    DriveKind drive = DriveKind::unknown;
    switch (drive_type) {
      case DRIVE_FIXED:drive=DriveKind::fixed;break;case DRIVE_REMOTE:drive=DriveKind::remote;break;
      case DRIVE_REMOVABLE:drive=DriveKind::removable;break;case DRIVE_RAMDISK:drive=DriveKind::ramdisk;break;
      case DRIVE_CDROM:drive=DriveKind::optical;break;default:break;
    }
    caps = take(classify_storage_capabilities(drive,caps.filesystem));
#else
    (void)root; // No non-Windows FS has been verified by fs_support.md v0.1.
#endif
    return caps;
  });
}

namespace detail {
MediaKind media_from_sysfs(const std::filesystem::path& sysfs_root, const std::filesystem::path& entry) noexcept {
  try {
    const auto boundary = std::filesystem::weakly_canonical(sysfs_root);
    auto current = std::filesystem::weakly_canonical(entry);
    auto detected = MediaKind::unknown;
    if (!std::filesystem::exists(current)) return MediaKind::unknown;
    auto read = [](const std::filesystem::path& p) {
      std::ifstream input(paths::native(p));
      std::string value;
      input >> value;
      return value;
    };
    for (;;) {
      const auto relative = current.lexically_relative(boundary);
      if (relative.empty() || *relative.begin() == "..") break;
      if (read(current / "removable") == "1") return MediaKind::removable;
      const auto rotational = read(current / "queue/rotational");
      if (detected == MediaKind::unknown) {
        if (rotational == "1") detected = MediaKind::hdd;
        if (rotational == "0") detected = MediaKind::ssd;
      }
      if (current == boundary || current == current.parent_path()) break;
      current = current.parent_path();
    }
    return detected;
  } catch (...) {
  }
  return MediaKind::unknown;
}
}  // namespace detail
MediaInfo detect_media(const std::filesystem::path& root) noexcept {
  MediaInfo result;
  try {
    const auto absolute = paths::native(std::filesystem::absolute(root));
#ifdef _WIN32
    std::vector<wchar_t> volume(absolute.native().size() + MAX_PATH + 1), name(MAX_PATH + 1);
    if (!GetVolumePathNameW(absolute.c_str(), volume.data(), static_cast<DWORD>(volume.size()))) return result;
    auto utf8 = [](const wchar_t* text) {
      const auto p = std::filesystem::path(text).u8string();
      return std::string(p.begin(), p.end());
    };
    result.device_id = utf8(volume.data());
    if (GetVolumeNameForVolumeMountPointW(volume.data(), name.data(), static_cast<DWORD>(name.size())))
      result.device_id = utf8(name.data());
    const auto drive = GetDriveTypeW(volume.data());
    if (drive == DRIVE_REMOTE) {
      result.kind = MediaKind::network;
      return result;
    }
    if (drive == DRIVE_REMOVABLE) {
      result.kind = MediaKind::removable;
      return result;
    }
    if (drive != DRIVE_FIXED) return result;
    std::wstring device = name[0] ? name.data() : volume.data();
    if (!device.empty() && device.back() == L'\\') device.pop_back();
    const auto handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return result;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
    DWORD returned = 0;
    const auto ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &descriptor,
                                    sizeof(descriptor), &returned, nullptr);
    CloseHandle(handle);
    if (ok && returned >= sizeof(descriptor))
      result.kind = descriptor.IncursSeekPenalty ? MediaKind::hdd : MediaKind::ssd;
#else
    struct stat device{};
    if (::stat(absolute.c_str(), &device) != 0) return result;
    result.device_id = std::to_string(major(device.st_dev)) + ":" + std::to_string(minor(device.st_dev));
    struct statfs filesystem{};
    if (::statfs(absolute.c_str(), &filesystem) == 0) {
      const auto type = static_cast<unsigned long>(filesystem.f_type);
      if (type == 0x6969UL || type == 0xff534d42UL || type == 0xfe534d42UL) {
        result.kind = MediaKind::network;
        return result;
      }
      if (type == 0x65735546UL) {
        std::ifstream mounts("/proc/self/mountinfo");
        std::string line;
        while (std::getline(mounts, line)) {
          std::istringstream input(line);
          std::string id, parent, mount_device, mount_root, mount_point;
          input >> id >> parent >> mount_device >> mount_root >> mount_point;
          if (mount_device != result.device_id) continue;
          const auto separator = line.find(" - ");
          if (separator == std::string::npos) continue;
          std::istringstream suffix(line.substr(separator + 3));
          std::string kind;
          suffix >> kind;
          if (kind == "fuse.sshfs" || kind == "fuse.rclone" || kind == "fuse.gvfsd-fuse") {
            result.kind = MediaKind::network;
            return result;
          }
        }
      }
    }
    result.kind = detail::media_from_sysfs("/sys", std::filesystem::path("/sys/dev/block") / result.device_id);
#endif
  } catch (...) {
    result.kind = MediaKind::unknown;
  }
  return result;
}

namespace {
manifest::SchemaValidator container_schema() {
  std::string schema;for(const auto part:manifest::published_schema_parts())schema+=part;
  return take(manifest::SchemaValidator::create(schema));
}
VerificationReport check_container(const asset_zip::ZipArchive& zip, const manifest::ManifestDocument& doc,
    const ContainerVerifyOptions& options, const SemanticContext& context, asset_zip::ReadBudget& budget,
    const std::function<void()>& validate_v2, const std::optional<std::set<std::string>>& allowed_opaque = {}) {
    VerificationReport report; report.v1 = CheckStatus::passed; report.format_support = classify(doc.known());
    auto record_error = [&](const Error& e) { if (!report.error) report.error = e; };
    if (report.format_support == FormatSupport::supported) {
      auto valid = guard<void>([&]() -> Result<void> { validate_v2(); return {}; });
      report.v2 = valid ? CheckStatus::passed : CheckStatus::failed; if (!valid) record_error(valid.error());
      if (options.semantics) {
      auto semantic = options.semantic_validator->validate(doc,zip,context);
      if (semantic) {
        report.v3_rules = semantic->checked_rules; report.violations = semantic->violations;
        report.v3_complete = semantic->complete; report.v3_findings = semantic->findings;
        report.v3_deferred_rules = semantic->deferred_rules;
        report.v3 = semantic->complete ? CheckStatus::passed : CheckStatus::partial;
        for (const auto& f : report.v3_findings) {
          if (f.severity == manifest::FindingSeverity::error) {
            report.v3 = CheckStatus::failed;
            record_error(error(Errc::corrupt_data,f.message_id,f.detail));
          } else if (f.severity == manifest::FindingSeverity::unverifiable && report.v3 == CheckStatus::passed) report.v3 = CheckStatus::partial;
        }
        for (const auto& e : report.violations) { report.v3 = CheckStatus::failed; record_error(e); }
      } else { report.v3 = CheckStatus::failed; record_error(semantic.error()); }
      }
    } else { report.v2 = CheckStatus::not_applicable_newer_minor; report.v3 = CheckStatus::not_performed; }
    if (!options.streams) return report;
    std::map<std::string,const asset_zip::EntryInfo*> entries;
    for (const auto& e : zip.entries()) entries.emplace(e.raw_name,&e);
    std::set<std::string> registered_paths{"manifest.json"}, selected(options.file_ids.begin(),options.file_ids.end());
    for (const auto& f : registered(doc.known())) {
      registered_paths.insert(f.path); StreamReport item{f.id,f.path,StreamStatus::unverifiable,{}};
      const bool wanted = options.level == VerifyLevel::full || (options.level == VerifyLevel::selective && selected.contains(f.id));
      if (wanted && f.size && !f.id.empty() && !f.path.empty() && f.sha.size() == 64) {
        auto result = guard<asset_zip::EntryVerification>([&]() -> Result<asset_zip::EntryVerification> {
          auto it = entries.find(f.path); if (it == entries.end()) return error(Errc::corrupt_data,"am.manifest.missing_stream",f.path);
          auto verified = take(zip.verify_entry(it->second->index,budget,true));
          if (verified.size != *f.size || !verified.sha256 || hex(*verified.sha256) != f.sha) return error(Errc::corrupt_data,"am.manifest.stream_mismatch",f.path);
          return verified;
        });
        if (result) item.status = StreamStatus::verified_sha256;
        else {
          item.status = result.error().code == Errc::unsupported ? StreamStatus::unsupported : StreamStatus::failed;
          item.error = result.error(); record_error(result.error());
        }
      }
      report.v4.push_back(std::move(item));
    }
    for (const auto& e : zip.entries()) if (!registered_paths.contains(e.raw_name)) {
      StreamReport item{"",e.raw_name,StreamStatus::unverifiable,{}};
      if (allowed_opaque && !allowed_opaque->contains(e.raw_name)) {
        item.status = StreamStatus::failed; item.error = error(Errc::corrupt_data,"am.manifest.unregistered_stream",e.raw_name); record_error(*item.error);
      } else if (options.level == VerifyLevel::full) {
        auto verified = guard<asset_zip::EntryVerification>([&] { return take(zip.verify_entry(e.index,budget,false)); });
        if (verified) item.status = StreamStatus::verified_crc_size_only;
        else { item.status = verified.error().code == Errc::unsupported ? StreamStatus::unsupported : StreamStatus::failed; item.error = verified.error(); record_error(verified.error()); }
      }
      report.v4.push_back(std::move(item));
    }
    bool full = report.v1 == CheckStatus::passed && report.v2 == CheckStatus::passed && !report.error;
    for (const auto& s : report.v4) if (s.status != StreamStatus::verified_sha256) full = false;
    report.container_verification = full ? ContainerVerification::full : ContainerVerification::partial; return report;
}
} // namespace
Result<VerificationReport> verify_container(const Path& path,const ContainerVerifyOptions& supplied,ContainerContents* output) {
  if(output)*output={};
  return guard<VerificationReport>([&] {
    ContainerContents contents;
    contents.budget=std::make_shared<asset_zip::ReadBudget>(supplied.read_budget);
    auto opened=asset_zip::ZipArchive::open(take(asset_zip::file_source(path,true)),supplied.zip_limits);
    if(!opened && (opened.error().code==Errc::corrupt_data || opened.error().code==Errc::unsupported)) {
      VerificationReport report;report.v1=CheckStatus::failed;report.error=opened.error();
      if(output)*output=std::move(contents);
      return report;
    }
    contents.archive=take(std::move(opened));
    auto parsed=guard<ManifestRead>([&]{return read_manifest(*contents.archive,supplied.codec_limits,*contents.budget);});
    if(!parsed){VerificationReport report;report.v1=CheckStatus::failed;report.error=parsed.error();if(output)*output=std::move(contents);return report;}
    contents.manifest_text=std::move(parsed->text);contents.document=std::move(parsed->document);
    auto options=supplied;if(!options.semantic_validator)options.semantic_validator=std::make_shared<Invariants>(true);
    auto schema=container_schema();
    auto report=check_container(*contents.archive,*contents.document,options,{},*contents.budget,[&]{take(schema.validate(*contents.document));});
    if(output)*output=std::move(contents);
    return report;
  });
}

struct AssetSnapshot::Impl {
  AssetRef ref;
  asset_zip::ZipArchive zip;
  std::string text;
  manifest::ManifestDocument document;
  db::BaseSnapshot base;
  FormatSupport support;
  bool can_write;
  bool trashed;
  Impl(AssetRef r, asset_zip::ZipArchive z, ManifestRead m, db::BaseSnapshot b, FormatSupport s, bool writable, bool in_trash = false)
      : ref(std::move(r)), zip(std::move(z)), text(std::move(m.text)), document(std::move(m.document)), base(std::move(b)), support(s), can_write(writable), trashed(in_trash) {}
};
AssetSnapshot::AssetSnapshot(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
AssetSnapshot::AssetSnapshot(AssetSnapshot&&) noexcept = default;
AssetSnapshot& AssetSnapshot::operator=(AssetSnapshot&&) noexcept = default;
AssetSnapshot::~AssetSnapshot() = default;
const AssetRef& AssetSnapshot::ref() const noexcept { return impl_->ref; }
const asset_zip::ZipArchive& AssetSnapshot::archive() const noexcept { return impl_->zip; }
const manifest::ManifestDocument& AssetSnapshot::manifest() const noexcept { return impl_->document; }
const std::string& AssetSnapshot::manifest_text() const noexcept { return impl_->text; }
const db::BaseSnapshot& AssetSnapshot::base() const noexcept { return impl_->base; }
U64 AssetSnapshot::revision() const noexcept { return repository::revision(impl_->document.known()); }
FormatSupport AssetSnapshot::format_support() const noexcept { return impl_->support; }
bool AssetSnapshot::writable() const noexcept { return impl_->can_write; }
bool AssetSnapshot::in_trash() const noexcept { return impl_->trashed; }

struct AssetRepository::Impl {
  Path root;
  RepositoryOptions options;
  StorageCapabilities capabilities;
  manifest::SchemaValidator schema;
  std::shared_ptr<db::CommitJournal> journal;
  mutable std::mutex schema_mutex, table_mutex;
  std::map<AssetRef,std::weak_ptr<std::mutex>> mutexes;
  Impl(Path r, RepositoryOptions o, StorageCapabilities c, manifest::SchemaValidator s)
      : root(std::move(r)), options(std::move(o)), capabilities(std::move(c)), schema(std::move(s)) {}
  Path path(const AssetRef& ref) const {
    if (ref.library_id != options.library_id || !uuid4(ref.asset_id)) fail(Errc::invalid_argument,"am.repo.invalid_operation","Invalid AssetRef");
    return root / "assets" / ref.asset_id.substr(ref.asset_id.size()-2) / (ref.asset_id + ".zip");
  }
  void require_write() const { if (options.mode == LibraryMode::read_only) fail(Errc::read_only,"am.repo.read_only","Library is read-only"); }
  void require_absent(const AssetRef& ref) const {
    if (std::filesystem::exists(paths::native(path(ref))) ||
        std::filesystem::exists(paths::native(root / "trash" / (ref.asset_id+".zip"))))
      fail(Errc::conflict,"am.repo.asset_id_exists","AssetID already exists in assets or trash");
  }
  void validate(const manifest::ManifestDocument& doc) const { std::lock_guard lock(schema_mutex); take(schema.validate(doc)); }
  struct Lease {
    Impl* owner; AssetRef ref; std::shared_ptr<std::mutex> mutex; std::unique_lock<std::mutex> lock;
    Lease(Impl& o, const AssetRef& r) : owner(&o), ref(r) {
      { std::lock_guard table(o.table_mutex); auto& weak = o.mutexes[r]; mutex = weak.lock(); if (!mutex) { mutex = std::make_shared<std::mutex>(); weak = mutex; } }
      lock = std::unique_lock<std::mutex>(*mutex);
    }
    ~Lease() {
      lock.unlock(); mutex.reset(); std::lock_guard table(owner->table_mutex);
      auto it = owner->mutexes.find(ref); if (it != owner->mutexes.end() && it->second.expired()) owner->mutexes.erase(it);
    }
  };
  VerificationReport check(const AssetRef& ref, const asset_zip::ZipArchive& zip, const manifest::ManifestDocument& doc,
                           VerifyLevel level, const std::vector<std::string>& ids, asset_zip::ReadBudget& budget,
                           const std::optional<std::set<std::string>>& allowed_opaque = {}, const std::optional<std::string>& op = {}) const {
    ContainerVerifyOptions settings;settings.level=level;settings.file_ids=ids;
    settings.semantic_validator=options.semantic_validator;
    return check_container(zip,doc,settings,{ref,op},budget,[&]{validate(doc);},allowed_opaque);
  }
};
AssetRepository::AssetRepository(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}
AssetRepository::AssetRepository(AssetRepository&&) noexcept = default;
AssetRepository& AssetRepository::operator=(AssetRepository&&) noexcept = default;
AssetRepository::~AssetRepository() = default;
Result<AssetRepository> AssetRepository::open(const Path& root, RepositoryOptions options) {
  return guard<AssetRepository>([&] {
    if (options.library_id.empty()) fail(Errc::invalid_argument,"am.repo.invalid_operation","LibraryID is missing");
    auto caps = options.capabilities ? *options.capabilities : take(detect_storage_capabilities(root));
    if (options.mode == LibraryMode::read_write && caps.mode == StorageMode::read_only_default)
      throw Error::make(Errc::read_only,"Storage is read-only by default",caps.filesystem).in("storage.repository").id("am.repo.storage_read_only").allow(ErrorAction::open_read_only);
    if (!options.clock) fail(Errc::invalid_argument,"am.repo.invalid_operation","Clock is missing");
    if (!options.semantic_validator) options.semantic_validator = std::make_shared<Invariants>(true);
    std::string schema; for (const auto part : manifest::published_schema_parts()) schema += part;
    auto state = std::make_shared<Impl>(std::filesystem::weakly_canonical(std::filesystem::absolute(root)),std::move(options),std::move(caps),take(manifest::SchemaValidator::create(schema)));
    if (state->options.mode == LibraryMode::read_write) state->journal = take(db::StateJournal::open(state->root));
    return AssetRepository(std::move(state));
  });
}
Result<AssetSnapshot> AssetRepository::load(const AssetRef& ref, bool require_v2, bool include_trash) const {
  return guard<AssetSnapshot>([&] {
    auto path = impl_->path(ref);
    bool trashed = false;
    if (include_trash) {
      const auto trash = impl_->root / "trash" / (ref.asset_id+".zip");
      const bool active_exists = std::filesystem::exists(paths::native(path));
      if (std::filesystem::exists(paths::native(trash))) {
        if (active_exists) fail(Errc::conflict,"am.repo.asset_id_exists","AssetID exists in both assets and trash");
        path = trash;
        trashed = true;
      }
    }
    if (!std::filesystem::exists(paths::native(path))) fail(Errc::not_found,"am.repo.not_found","Asset does not exist");
    // Keep the source handle open. If replacement raced the metadata query,
    // retry rather than combine a manifest and base from different versions.
    for (int attempt = 0; attempt < 2; ++attempt) {
      asset_zip::ReadBudget budget(impl_->options.read_budget);
      auto base = take(commit::inspect_zip(path,budget,impl_->options.zip_limits));
      auto source = take(asset_zip::file_source(path,true)); auto zip = take(asset_zip::ZipArchive::open(source,impl_->options.zip_limits));
      auto manifest = read_manifest(zip,impl_->options.codec_limits,budget); const auto support = classify(manifest.document.known());
      if (source->size() != base.size || hash_text(manifest.text) != base.manifest_sha256) continue;
      if (require_v2 && support == FormatSupport::supported) impl_->validate(manifest.document);
      base.revision = static_cast<std::int64_t>(repository::revision(manifest.document.known()));
      return AssetSnapshot(std::make_unique<AssetSnapshot::Impl>(ref,std::move(zip),std::move(manifest),std::move(base),support,support == FormatSupport::supported && impl_->options.mode == LibraryMode::read_write && !trashed,trashed));
    }
    fail(Errc::conflict,"am.repo.revision_conflict","Snapshot changed during acquisition");
  });
}
Result<AssetSnapshot> AssetRepository::snapshot(const AssetRef& ref, bool include_trash) const { return load(ref,true,include_trash); }
Result<VerificationReport> verify_snapshot(const AssetSnapshot& snapshot, const ContainerVerifyOptions& supplied,
                                           std::stop_token stop, asset_zip::ReadBudget* shared_budget) {
  return guard<VerificationReport>([&] {
    class StoppableSource final : public asset_zip::RandomAccessSource {
     public:
      std::shared_ptr<const asset_zip::RandomAccessSource> retained;
      std::stop_token stop;
      StoppableSource(std::shared_ptr<const asset_zip::RandomAccessSource> source, std::stop_token token)
          : retained(std::move(source)), stop(token) {}
      std::uint64_t size() const noexcept override { return retained->size(); }
      Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
        if (stop.stop_requested()) return error(Errc::cancelled,"am.export.cancelled",{});
        return retained->read_at(offset, bytes);
      }
    };
    auto options = supplied;
    if (!options.semantic_validator) options.semantic_validator = std::make_shared<Invariants>(true);
    auto source = std::make_shared<StoppableSource>(snapshot.archive().source(), stop);
    auto archive = take(asset_zip::ZipArchive::open(source, options.zip_limits));
    asset_zip::ReadBudget budget(options.read_budget);
    auto schema = container_schema();
    return check_container(archive, snapshot.manifest(), options, {snapshot.ref(), {}}, shared_budget ? *shared_budget : budget,
                           [&] { take(schema.validate(snapshot.manifest())); });
  });
}
Result<VerificationReport> AssetRepository::verify(const AssetRef& ref, VerifyLevel level, const std::vector<std::string>& ids) const {
  return guard<VerificationReport>([&] {
    auto snap = take(load(ref,false)); asset_zip::ReadBudget budget(impl_->options.read_budget);
    return impl_->check(ref,snap.archive(),snap.manifest(),level,ids,budget);
  });
}
Result<std::vector<commit::RecoveryItem>> AssetRepository::recover() {
  return guard<std::vector<commit::RecoveryItem>>([&]() -> Result<std::vector<commit::RecoveryItem>> {
    impl_->require_write();
    commit::RecoveryOptions options;
    options.journal = impl_->journal;
    options.lock_timeout = impl_->options.lock_timeout;
    options.zip_limits = impl_->options.zip_limits;
    options.on_committed = [&](const db::PendingOperation& row) -> Result<void> {
      if (row.kind == "create" && impl_->options.on_indexed) {
        auto snap = take(load({impl_->options.library_id,row.asset_id},false));
        if (is_independent_copy(snap,impl_->options.codec_limits)) {
          try { impl_->options.on_indexed({snap.ref(),snap.revision(),row.op_id,"create"}); } catch (...) {}
        }
      }
      if (row.kind == "import_ready" && impl_->options.on_indexed) {
        auto snap = take(load({impl_->options.library_id, row.asset_id}, false));
        try {
          impl_->options.on_indexed({snap.ref(), snap.revision(), row.op_id, row.kind});
        } catch (...) {
        }
      }
      if ((row.kind == "trash" || row.kind == "restore" || row.kind == "purge") && impl_->options.on_indexed) {
        try {
          impl_->options.on_indexed({{impl_->options.library_id, row.asset_id}, 0, row.op_id, row.kind});
        } catch (...) {
        }
      }
      return {};
    };
    const auto before = take(impl_->journal->list());
    auto recovered = take(commit::recover(impl_->root, options));
    // The shared FSM only cleans an already-indexed row. An import can crash
    // before its repository notification, so replay that new-kind notification.
    for (auto& item : recovered)
      if (item.outcome == commit::RecoveryOutcome::cleaned) {
        for (const auto& row : before)
          if (row.op_id == item.op_id && row.kind == "import_ready" && row.state == db::CommitState::indexed) {
            auto notified = options.on_committed(row);
            if (!notified) {
              item.outcome = commit::RecoveryOutcome::failed;
              item.error = notified.error();
            }
          }
      }
    return recovered;
  });
}
Result<LifecycleResult> AssetRepository::lifecycle(const AssetRef& ref, std::string_view kind) {
  return guard<LifecycleResult>([&]() -> Result<LifecycleResult> {
    impl_->require_write();
    (void)impl_->path(ref);
    Impl::Lease lease(*impl_, ref);
    commit::LifecycleRequest request;
    request.library_root = impl_->root;
    request.asset_id = ref.asset_id;
    request.kind = kind;
    request.journal = impl_->journal;
    request.lock_timeout = impl_->options.lock_timeout;
    request.hooks = impl_->options.test_hooks.commit;
    request.on_committed = [&](const db::PendingOperation& row) -> Result<void> {
      // Location events carry no manifest revision: even unreadable ZIPs can move.
      if (impl_->options.on_indexed) {
        try {
          impl_->options.on_indexed({ref, 0, row.op_id, row.kind});
        } catch (...) {
        }
      }
      return {};
    };
    return LifecycleResult{ref, take(commit::commit_lifecycle(request))};
  });
}
Result<LifecycleResult> AssetRepository::trash(const AssetRef& ref) { return lifecycle(ref, "trash"); }
Result<LifecycleResult> AssetRepository::restore(const AssetRef& ref) { return lifecycle(ref, "restore"); }
Result<LifecycleResult> AssetRepository::purge(const AssetRef& ref) { return lifecycle(ref, "purge"); }

namespace {
Path import_path(const Path& path) {
  auto result = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
#ifdef _WIN32
  auto text = result.wstring();
  if (text.starts_with(L"\\\\?\\UNC\\"))
    result = Path(L"\\\\" + text.substr(8));
  else if (text.starts_with(L"\\\\?\\"))
    result = Path(text.substr(4));
#endif
  return result;
}
bool import_inside(const Path& root, const Path& source) {
  auto a = root.begin(), b = source.begin();
  for (; a != root.end(); ++a, ++b) {
    if (b == source.end()) return false;
#ifdef _WIN32
    const auto x = a->wstring(), y = b->wstring();
    if (CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE) !=
        CSTR_EQUAL)
      return false;
#else
    if (*a != *b) return false;
#endif
  }
  return true;
}
ImportAssetInfo import_info(const ManifestRead& manifest, U64 size) {
  const auto& j = manifest.document.known();
  const auto asset = j.find("asset"), format = j.find("format");
  if (asset == j.end() || !asset->is_object() || format == j.end() || !format->is_object())
    fail(Errc::corrupt_data, "am.repo.import_invalid", "Required asset identity is unavailable");
  const auto rev = revision(j);
  const auto version = format->find("version");
  const auto id = Uuid::parse_canonical(string(*asset, "id"));
  if (!rev || !id || id->version() != 4 || (id->bytes()[8] & 0xc0) != 0x80 || version == format->end() ||
      !version->is_object() || !version->contains("minor") || !number((*version)["minor"]))
    fail(Errc::corrupt_data, "am.repo.import_invalid", "Required asset identity or version is unavailable");
  return {std::to_string(size), rev, number((*version)["minor"]), hash_text(manifest.text)};
}
std::string import_file_hash(const std::shared_ptr<const asset_zip::RandomAccessSource>& source) {
  auto sha = take(asset_zip::Sha256::create());
  std::array<std::byte, 65536> buffer{};
  for (U64 offset = 0; offset < source->size();) {
    const auto count = take(source->read_at(offset, buffer));
    if (!count) fail(Errc::conflict, "am.repo.import_current_changed", "Unexpected current file EOF");
    take(sha.update(std::span(buffer).first(count)));
    offset += count;
  }
  return hex(take(sha.finish()));
}
}  // namespace
Result<ImportReadyResult> AssetRepository::import_ready(const ImportReadyRequest& incoming,
                                                        ImportReadyResult* failure) {
  if (failure) *failure = {};
  return guard<ImportReadyResult>([&]() -> Result<ImportReadyResult> {
    ImportReadyResult out;
    out.ref.library_id = impl_->options.library_id;
    auto operation = guard<void>([&]() -> Result<void> {
      impl_->require_write();
      if(incoming.stop.stop_requested()) fail(Errc::cancelled,"am.commit.cancelled","Import cancelled before publication");
      const auto source_path = import_path(incoming.source), root = import_path(impl_->root);
      if (import_inside(root, source_path))
        fail(Errc::invalid_argument, "am.repo.import_source_inside_library", "Source is inside destination library");
      if (!std::filesystem::is_regular_file(paths::native(source_path)))
        fail(Errc::invalid_argument, "am.repo.invalid_operation", "Import source must be an explicit regular file");
      const auto source_base = take(commit::inspect_copy_source(source_path));
      {
        auto source = take(asset_zip::file_source(source_path, true));
        const auto probe =
            take(asset_zip::probe_container(source, impl_->options.zip_limits, impl_->options.codec_limits));
        if (probe.outcome != asset_zip::ProbeOutcome::own) {
          if (probe.outcome == asset_zip::ProbeOutcome::own_unsupported_version) {
            out.classification = "unsupported_version";
            fail(Errc::unsupported, "am.repo.import_unsupported_version",
                 "Incoming major format version is unsupported");
          }
          if (probe.outcome == asset_zip::ProbeOutcome::own_corrupt) {
            out.classification = "invalid";
            out.verification.emplace();
            out.verification->v1 = CheckStatus::failed;
            out.verification->error = probe.error;
            fail(Errc::corrupt_data, "am.repo.import_invalid", "Incoming own container is corrupt");
          }
          out.classification = "not_own";
          fail(Errc::unsupported, "am.repo.not_own_format", "Source is not an own asset ZIP");
        }
        auto archive = take(asset_zip::ZipArchive::open(source, impl_->options.zip_limits));
        asset_zip::ReadBudget budget(impl_->options.read_budget);
        const auto manifest = read_manifest(archive, impl_->options.codec_limits, budget);
        auto info = guard<ImportAssetInfo>([&] { return import_info(manifest, source_base.size); });
        if (!info) { out.classification = "invalid"; throw info.error(); }
        out.incoming = std::move(*info);
        out.ref.asset_id = string(manifest.document.known()["asset"], "id");
      }
      (void)impl_->path(out.ref);
      Impl::Lease lease(*impl_, out.ref);
      bool publish = false;
      std::string current_zip_hash;
      asset_zip::ReadBudget budget(impl_->options.read_budget);
      commit::CommitRequest request(budget);
      request.stop = incoming.stop;
      request.library_root = impl_->root;
      request.target_path = impl_->path(out.ref);
      request.asset_id = out.ref.asset_id;
      request.kind = "import_ready";
      request.estimated_size = source_base.size;
      request.capabilities = impl_->capabilities;
      request.zip_limits = impl_->options.zip_limits;
      request.journal = impl_->journal;
      request.lock_timeout = impl_->options.lock_timeout;
      request.hooks = impl_->options.test_hooks.commit;
      request.copy_source.emplace();
      request.copy_source->path = source_path;
      request.copy_source->expected = source_base;
      request.copy_source->prepare = [&]() -> Result<std::optional<db::BaseSnapshot>> {
        return guard<std::optional<db::BaseSnapshot>>([&]() -> Result<std::optional<db::BaseSnapshot>> {
          const auto target = impl_->path(out.ref), trash = impl_->root / "trash" / (out.ref.asset_id + ".zip");
          const bool in_trash = std::filesystem::exists(paths::native(trash));
          const auto current_path = in_trash ? trash : target;
          std::optional<db::BaseSnapshot> base;
          if (std::filesystem::exists(paths::native(current_path))) {
            out.current =
                ImportAssetInfo{std::to_string(std::filesystem::file_size(paths::native(current_path))), {}, {}, {}};
            auto readable = guard<void>([&]() -> Result<void> {
              auto input = take(asset_zip::file_source(current_path, true));
              auto archive = take(asset_zip::ZipArchive::open(input, impl_->options.zip_limits));
              const auto manifest = read_manifest(archive, impl_->options.codec_limits, budget);
              const auto info = import_info(manifest, input->size());
              if (string(manifest.document.known()["asset"], "id") != out.ref.asset_id)
                fail(Errc::corrupt_data, "am.repo.import_invalid", "Current asset identity differs from path");
              current_zip_hash = import_file_hash(input);
              out.current = info;
              base = take(commit::inspect_zip(current_path, budget, impl_->options.zip_limits));
              if (base->manifest_sha256 != *info.manifest_sha256)
                fail(Errc::conflict, "am.repo.import_current_changed", "Current manifest changed during acquisition");
              return {};
            });
            if (!readable) {
              if (readable.error().code == Errc::resource_exhausted ||
                  readable.error().message_id == "am.repo.import_current_changed")
                throw readable.error();
              out.current = ImportAssetInfo{out.current->zip_size, {}, {}, {}};
              base.reset();
            }
          }
          if (in_trash)
            out.classification = "in_trash";
          else if (!out.current)
            out.classification = "new";
          else if (!out.current->manifest_sha256)
            out.classification = "current_unreadable";
          else if (out.current->manifest_sha256 == out.incoming->manifest_sha256)
            out.classification = "identical";
          else if (*out.incoming->revision > *out.current->revision)
            out.classification = "newer_revision";
          else if (*out.incoming->revision < *out.current->revision)
            out.classification = "older_revision";
          else
            out.classification = "same_revision_different";
          const bool replaceable = out.classification == "newer_revision" || out.classification == "older_revision" ||
                                   out.classification == "same_revision_different";
          if (incoming.replace_expected) {
            if (!replaceable)
              fail(Errc::invalid_argument, "am.repo.import_replace_not_applicable",
                   "Replacement is not applicable to this classification");
            if (*incoming.replace_expected != *out.current->manifest_sha256)
              fail(Errc::conflict, "am.repo.import_current_changed",
                   "Current manifest no longer equals the observed manifest");
          }
          publish = out.classification == "new" || (replaceable && incoming.replace_expected.has_value());
          if (!publish) {
            out.status = out.classification == "identical" ? "identical" : "conflict";
            if (out.status == "conflict")
              out.error = error(Errc::conflict, "am.repo.import_conflict", out.classification);
          }
          return in_trash ? std::nullopt : base;
        });
      };
      request.verifier = [&](const asset_zip::ZipArchive& zip) -> Result<void> {
        return guard<void>([&]() -> Result<void> {
          const auto actual = read_manifest(zip, impl_->options.codec_limits, budget);
          if (hash_text(actual.text) != *out.incoming->manifest_sha256)
            fail(Errc::conflict, "am.repo.import_source_changed", "Incoming manifest changed before copying");
          out.verification = impl_->check(out.ref, zip, actual.document, VerifyLevel::full, {}, budget);
          const auto& v = *out.verification;
          bool valid = v.v1 == CheckStatus::passed &&
                       (v.v2 == CheckStatus::passed || v.v2 == CheckStatus::not_applicable_newer_minor) &&
                       (v.v3 == CheckStatus::passed || v.v3 == CheckStatus::partial ||
                        (v.format_support == FormatSupport::newer_minor && v.v3 == CheckStatus::not_performed));
          for (const auto& stream : v.v4)
            if (stream.status != StreamStatus::verified_sha256 && stream.status != StreamStatus::verified_crc_size_only)
              valid = false;
          if (!valid || v.error) {
            auto e = error(Errc::corrupt_data, "am.repo.import_invalid", "Incoming copy failed required verification");
            if (v.error) e = std::move(e).caused_by(*v.error);
            throw e;
          }
          if (v.format_support == FormatSupport::newer_minor)
            out.warnings.push_back(
                error(Errc::unsupported, "am.repo.import_newer_minor", "Incoming minor version is newer")
                    .with_severity(Severity::warning));
          return {};
        });
      };
      request.copy_source->publish = [&](const db::NewFileEvidence& evidence) -> Result<bool> {
        if (out.classification == "identical") out.zip_bytes_identical = current_zip_hash == evidence.file_sha256;
        return publish;
      };
      auto committed = commit::commit_zip(request);
      if (!committed) {
        if (committed.error().message_id == "am.commit.verification_failed") {
          for (const Error* cause = &committed.error(); cause; cause = cause->cause.get())
            if (cause->message_id == "am.repo.import_source_changed") return *cause;
          if (!out.verification) {
            out.verification.emplace();
            out.verification->v1 = CheckStatus::failed;
            out.verification->error = committed.error();
          }
          return error(Errc::corrupt_data, "am.repo.import_invalid", "Temporary import verification failed")
              .caused_by(committed.error());
        }
        return committed.error();
      }
      if (!committed->no_op) {
        out.op_id = committed->op_id;
        out.status = out.classification == "new" ? "imported" : "replaced";
        if (out.classification == "older_revision")
          out.warnings.push_back(
              error(Errc::conflict, "am.repo.import_older_revision", "Explicit replacement uses an older revision")
                  .with_severity(Severity::warning));
        if (impl_->options.on_indexed) {
          try {
            impl_->options.on_indexed({out.ref, *out.incoming->revision, *out.op_id, "import_ready"});
          } catch (...) {
          }
        }
      }
      return {};
    });
    if (!operation) {
      out.status = "failed";
      out.error = operation.error();
      if (failure) *failure = out;
      return operation.error();
    }
    return out;
  });
}

namespace {
struct ArchivePassState {
  std::shared_ptr<const archive::ZipInputSnapshot> expected;
  std::shared_ptr<archive::ZipInputJob> job;
  std::optional<archive::ZipInput> preflight, packing;
  std::optional<std::string> sha256;
};
struct NewSource {
  Path path; U64 size; std::string sha;
  asset_zip::CompressionMethod method;
  std::stop_token stop;
  std::optional<std::string> expected_sha256;
  std::string file_id;
  std::optional<SourceFailure>* failure = nullptr;
  std::shared_ptr<ArchivePassState> archive{};
  std::size_t archive_index=0;
};
void check_stop(std::stop_token stop) {
  if (stop.stop_requested()) fail(Errc::cancelled,"am.commit.cancelled","Import cancelled before publication");
}
template<class T> T read_source(Result<T> result, const std::string& id, std::optional<SourceFailure>* failure) {
  if (!result && failure && (result.error().code == Errc::io_error || result.error().code == Errc::not_found))
    *failure = SourceFailure{id,result.error()};
  return take(std::move(result));
}
[[noreturn]] void changed_source(const std::string& id, std::optional<SourceFailure>* failure, const char* detail) {
  auto cause=error(Errc::conflict,"am.repo.source_changed",detail);
  if(failure) *failure=SourceFailure{id,cause,false};
  throw cause;
}
template<class T> T read_archive(Result<T> result,const std::string& id,std::optional<SourceFailure>* failure) {
  if(!result && failure && (result.error().code==Errc::io_error || result.error().code==Errc::not_found || result.error().code==Errc::corrupt_data))
    *failure=SourceFailure{id,result.error(),true};
  return take(std::move(result));
}
archive::ZipInput& archive_pass(ArchivePassState& state,bool packing,std::stop_token stop,
                               const std::string& id,std::optional<SourceFailure>* failure) {
  auto& current=packing?state.packing:state.preflight;
  if(!current) {
    check_stop(stop);
    auto attempt=archive::open_snapshot(*state.expected,state.job,stop);
    if(!attempt) {
      // Reopening a nested archive reads bytes, so a sticky input limit may fire here; it is not a source change.
      if((!state.job || state.job->exceeded_limit().empty()) &&(state.sha256 || state.expected->sha256) && (attempt.error().code==Errc::corrupt_data || attempt.error().code==Errc::unsupported || attempt.error().code==Errc::not_found))
        changed_source(id,failure,"Archive became unreadable after its snapshot");
      throw attempt.error(); // Whole-container failures must not become per-record retries.
    }
    auto opened=take(std::move(attempt));
    if(opened.identity()!=state.expected->entries) changed_source(id,failure,"Archive listing differs from its snapshot");
    auto digest=take(opened.sha256(stop));
    if((state.expected->sha256 && digest!=*state.expected->sha256) || (state.sha256 && digest!=*state.sha256))
      changed_source(id,failure,"Archive SHA-256 changed between passes");
    state.sha256=digest; current=std::move(opened);
  }
  return *current;
}
NewSource preflight_archive(std::shared_ptr<ArchivePassState> state,std::size_t index,std::string_view name,
                             std::stop_token stop,const std::string& id,std::optional<SourceFailure>* failure) {
  auto& zip=archive_pass(*state,false,stop,id,failure);
  if(index>=zip.entries().size()) changed_source(id,failure,"Archive index changed");
  auto stream=read_archive(zip.open_entry(index),id,failure);
  auto hash=take(asset_zip::Sha256::create());
  std::array<std::byte,65536> bytes{}; std::vector<std::byte> sample;
  U64 size=0;
  for(;;) {
    check_stop(stop);
    const auto n=read_archive(stream.read(bytes),id,failure); if(!n) break;
    if(size==0) sample.assign(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(n));
    take(hash.update(std::span(bytes).first(n))); size+=n;
  }
  NewSource source{state->expected->path,size,hex(take(hash.finish())),take(asset_zip::choose_method(name,sample)),stop,{},id,failure};
  source.archive=std::move(state); source.archive_index=index;
  return source;
}
NewSource preflight(const Path& path, std::string_view name, std::stop_token stop = {},
                    const std::string& id = {}, std::optional<SourceFailure>* failure = nullptr) {
  check_stop(stop);
  auto source = read_source(asset_zip::file_source(path),id,failure); auto hash = take(asset_zip::Sha256::create());
  std::array<std::byte,65536> bytes{}; std::vector<std::byte> sample;
  for (U64 offset = 0; offset < source->size();) {
    check_stop(stop);
    const auto count = read_source(source->read_at(offset,bytes),id,failure); if (!count) fail(Errc::conflict,"am.repo.source_changed","Unexpected source EOF");
    if (offset == 0) sample.assign(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(count));
    take(hash.update(std::span(bytes).first(count))); offset += count;
  }
  return {path,source->size(),hex(take(hash.finish())),take(asset_zip::choose_method(name,sample)),stop,{},id,failure};
}
struct Plan {
  std::stop_token stop; std::optional<SourceFailure>* failure = nullptr;
  std::map<std::string,NewSource> sources; std::set<std::string> removed_ids; Json changes = Json::array();
  std::map<Path,std::shared_ptr<ArchivePassState>> archives;
};
Json& file(Json& j, const std::string& id) {
  if (j.contains("files")) for (auto& f : j["files"]) if (string(f,"id") == id) return f;
  fail(Errc::invalid_argument,"am.repo.invalid_operation","Unknown FileID: " + id);
}
void dependency_check(const Json& j, const std::string& id) {
  if (!j.contains("extensions")) return;
  for (const auto& e : j["extensions"]) {
    bool blocked = !e.contains("dependencies") || string(e["dependencies"],"state") == "unknown";
    if (!blocked && string(e["dependencies"],"state") == "declared") for (const auto& r : array(e["dependencies"],"refs")) if (string(r,"kind") == "file" && string(r,"file_id") == id) blocked = true;
    if (blocked) throw Error::make(Errc::needs_decision,"Extension dependency prevents changing file",id).in("storage.repository").id("am.repo.extension_dependency").allow(ErrorAction::decide);
  }
}
void collision_check(const Json& j, const std::set<std::string>& opaque) {
  std::set<std::string> used{unicode::path_collision_key("manifest.json")};
  auto add = [&](std::string_view name) {
    if (!used.insert(unicode::path_collision_key(name)).second) fail(Errc::conflict,"am.repo.path_collision",std::string(name));
    if (name.size() > 200 || !paths::check_container_path(name)) fail(Errc::invalid_argument,"am.repo.invalid_operation","Unsafe or oversized container path");
  };
  for (const auto& f : registered(j)) add(f.path); for (const auto& p : opaque) add(p);
}
void add_files(Json& j, Plan& plan, const AddFiles& add, const std::set<std::string>& opaque) {
  const auto input_id = string(add.input,"id"); if (!uuid4(input_id)) fail(Errc::invalid_argument,"am.repo.invalid_operation","InputID must be UUIDv4");
  if (!j.contains("inputs")) j["inputs"] = Json::array();
  std::set<std::string> ids; for (const auto& f : array(j,"files")) ids.insert(string(f,"id")); for (const auto& f : array(j,"historical_files")) ids.insert(string(f,"id"));
  std::set<std::string> used{unicode::path_collision_key("manifest.json")};
  for (const auto& record : registered(j)) used.insert(unicode::path_collision_key(record.path));
  for (const auto& name : opaque) used.insert(unicode::path_collision_key(name));
  for (const auto& f : add.files) {
    const auto path = "content/"+input_id+"/"+f.path;
    if (!used.insert(unicode::path_collision_key(path)).second) fail(Errc::conflict,"am.repo.path_collision",path);
    if (path.size()>200 || !paths::check_container_path(path)) fail(Errc::invalid_argument,"am.repo.invalid_operation","Unsafe or oversized container path");
  }
  for (const auto& f : add.files) {
    if (f.origin_kind != "imported" && f.origin_kind != "user_added") fail(Errc::invalid_argument,"am.repo.invalid_operation","AddFiles origin must be imported or user_added");
    const auto id = identifier(f.file_id), path = "content/" + input_id + "/" + f.path;
    if (!ids.insert(id).second) fail(Errc::conflict,"am.repo.path_collision","FileID already exists: " + id);
    auto source = [&] {
      if(!f.archive_entry) return preflight(f.source,path,plan.stop,id,plan.failure);
      if(!f.source.empty() || !f.archive_entry->archive)
        fail(Errc::invalid_argument,"am.repo.invalid_operation","Archive input requires exactly one content source");
      const auto& expected=f.archive_entry->archive;
      const auto key=std::filesystem::absolute(expected->path).lexically_normal();
      auto& state=plan.archives[key];
      if(!state) {
        state=std::make_shared<ArchivePassState>(); state->expected=expected;
        if(expected->job) state->job=expected->job;
        else {
          archive::ZipInputHooks hooks;
          hooks.cancelled=[stop=plan.stop] { return stop.stop_requested(); };
          state->job=std::make_shared<archive::ZipInputJob>(archive::ZipInputLimits{},std::move(hooks));
        }
      } else if(state->expected.get()!=expected.get() &&
          (state->expected->entries!=expected->entries || state->expected->sha256!=expected->sha256))
        changed_source(id,plan.failure,"Conflicting archive snapshots in one request");
      return preflight_archive(state,f.archive_entry->index,path,plan.stop,id,plan.failure);
    }();
    source.expected_sha256 = f.expected_sha256;
    if (f.expected_sha256 && source.sha != *f.expected_sha256)
      changed_source(id,plan.failure,"Source differs from expected SHA-256");
    Json record{{"id",id},{"storage_path",path},{"size_bytes",std::to_string(source.size)},{"sha256",source.sha},{"origin_kind",f.origin_kind}};
    if (f.metadata) record["metadata"] = *f.metadata;
    j["files"].push_back(record); plan.sources.emplace(path,std::move(source)); plan.changes.push_back({{"kind","file_add"},{"target",id}});
  }
  collision_check(j,opaque);
  for (const auto& i : j["inputs"]) if (string(i,"id") == input_id) fail(Errc::invalid_argument,"am.repo.invalid_operation","InputID already exists");
  j["inputs"].push_back(add.input);
}
void precheck_add_paths(const Json& j, const std::vector<ChangeOperation>& operations, const std::set<std::string>& opaque) {
  std::map<std::string,std::string> current;
  std::set<std::string> used{unicode::path_collision_key("manifest.json")};
  for (const auto& f : registered(j)) { auto key=unicode::path_collision_key(f.path); current.emplace(f.id,key); used.insert(key); }
  for (const auto& p : opaque) used.insert(unicode::path_collision_key(p));
  for (const auto& op : operations) {
    if (const auto* remove=std::get_if<RemoveFile>(&op)) {
      auto found=current.find(remove->file_id); if (found!=current.end()) { used.erase(found->second); current.erase(found); }
    }
    if (const auto* add=std::get_if<AddFiles>(&op)) for (const auto& f : add->files) {
      const auto path="content/"+string(add->input,"id")+"/"+f.path;
      auto key=unicode::path_collision_key(path);
      if (!used.insert(key).second) fail(Errc::conflict,"am.repo.path_collision",path);
      if (path.size()>200 || !paths::check_container_path(path)) fail(Errc::invalid_argument,"am.repo.invalid_operation","Unsafe or oversized container path");
      if (f.file_id) current.emplace(*f.file_id,std::move(key));
    }
  }
}
void apply(Json& j, Plan& plan, const std::vector<ChangeOperation>& operations, U64 new_revision, const std::set<std::string>& opaque) {
  precheck_add_paths(j,operations,opaque);
  static const std::set<std::string> fields{"title","description","authors","publisher","license","tags","categories","links","dates"};
  for (const auto& operation : operations) std::visit([&](const auto& op) {
    using T = std::decay_t<decltype(op)>;
    if constexpr (std::is_same_v<T,AddFiles>) add_files(j,plan,op,opaque);
    if constexpr (std::is_same_v<T,SetMetadata>) {
      if (!fields.contains(op.field)) fail(Errc::invalid_argument,"am.repo.invalid_operation","Field is not standard metadata");
      Json* owner = nullptr;
      if (op.scope == "asset") owner = &j["asset"];
      else if (op.scope.starts_with("file:")) owner = &file(j,op.scope.substr(5));
      else fail(Errc::invalid_argument,"am.repo.invalid_operation","Invalid metadata scope");
      if (!op.value) { if (!owner->contains("metadata") || !(*owner)["metadata"].contains(op.field)) return; (*owner)["metadata"].erase(op.field); }
      else {
        if (owner->contains("metadata") && (*owner)["metadata"].contains(op.field) && (*owner)["metadata"][op.field] == *op.value) return;
        (*owner)["metadata"][op.field] = *op.value;
      }
      plan.changes.push_back({{"kind","metadata_set"},{"target",op.scope},{"field",op.field}});
    }
    if constexpr (std::is_same_v<T,RemoveFile>) {
      const auto old = file(j,op.file_id); dependency_check(j,op.file_id);
      if (!j.contains("historical_files")) j["historical_files"] = Json::array();
      j["historical_files"].push_back({{"id",op.file_id},{"removed_at_revision",new_revision},{"last_known_sha256",old["sha256"]},{"last_known_size_bytes",old["size_bytes"]},{"last_known_storage_path",old["storage_path"]}});
      auto& files = j["files"]; files.erase(std::remove_if(files.begin(),files.end(),[&](const auto& f) { return string(f,"id") == op.file_id; }),files.end());
      plan.sources.erase(string(old,"storage_path")); plan.removed_ids.insert(op.file_id); plan.changes.push_back({{"kind","file_remove"},{"target",op.file_id}});
    }
    if constexpr (std::is_same_v<T,ReplaceFileContent>) {
      auto& f = file(j,op.file_id); if (string(f,"origin_kind") == "generated") fail(Errc::invalid_argument,"am.repo.invalid_operation","Generated content must be recreated by its handler");
      dependency_check(j,op.file_id); const auto path = string(f,"storage_path"); auto source = preflight(op.source,path);
      if (source.sha == string(f,"sha256") && source.size == decimal(f,"size_bytes")) return;
      plan.changes.push_back({{"kind","file_replace"},{"target",op.file_id},{"old_sha256",f["sha256"]},{"new_sha256",source.sha}});
      f["sha256"] = source.sha; f["size_bytes"] = std::to_string(source.size); plan.sources.insert_or_assign(path,std::move(source));
    }
  },operation);
}
[[noreturn]] void revision_conflict(U64 current) {
  throw Error::make(Errc::conflict,"Asset revision conflict","current_revision=" + std::to_string(current)).in("storage.repository").id("am.repo.revision_conflict").allow(ErrorAction::decide);
}
// Read only the small, typed history coordinates from the strict token tree.
// Parsing the complete JSON through a numeric DOM would lose O-13 lexemes.
void rebase(const AssetSnapshot& snap, const ChangeRequest& request, const manifest::CodecLimits& limits) {
  if (request.expected_rev == snap.revision()) return;
  if (request.expected_rev == 0 || request.expected_rev > snap.revision()) revision_conflict(snap.revision());
  for (const auto& op : request.ops) if (std::holds_alternative<RemoveFile>(op) || std::holds_alternative<ReplaceFileContent>(op)) revision_conflict(snap.revision());
  const auto& j = snap.manifest().known();
  for (const auto& r : array(j["history"],"compacted_ranges")) if (number(r["to_revision"]).value_or(0) > request.expected_rev) revision_conflict(snap.revision());
  auto tree = take(am::json::parse_strict(snap.manifest_text(),{limits.max_depth,limits.max_bytes}));
  auto child = [&](std::size_t node, std::string_view name) { if (node != am::json::no_node) for (auto c : tree.nodes[node].children) if (tree.nodes[c].edge == name) return c; return am::json::no_node; };
  auto text = [&](std::size_t node, std::string_view name) { const auto c = child(node,name); return c != am::json::no_node && tree.nodes[c].kind == am::json::Kind::string ? tree.nodes[c].decoded : std::string{}; };
  auto entries = child(child(0,"history"),"entries"); if (entries == am::json::no_node) revision_conflict(snap.revision());
  const std::set<std::string> vocabulary{"asset_created","files_changed","metadata_changed","groups_changed","previews_changed","provenance_changed","field_definitions_changed","extension_data_changed","format_migrated","history_compacted","session_committed"};
  for (auto e : tree.nodes[entries].children) {
    const auto rev = child(e,"revision"); if (rev == am::json::no_node) revision_conflict(snap.revision());
    U64 value = 0; auto raw = snap.manifest_text().substr(tree.nodes[rev].begin,tree.nodes[rev].end-tree.nodes[rev].begin); auto parsed = std::from_chars(raw.data(),raw.data()+raw.size(),value);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data()+raw.size()) revision_conflict(snap.revision());
    if (value <= request.expected_rev) continue;
    auto changes = child(e,"changes"); if (!vocabulary.contains(text(e,"operation")) || changes == am::json::no_node || tree.nodes[changes].kind != am::json::Kind::array) revision_conflict(snap.revision());
    for (auto c : tree.nodes[changes].children) {
      const auto kind = text(c,"kind"), target = text(c,"target"), field_name = text(c,"field");
      if (tree.nodes[c].kind != am::json::Kind::object || kind.empty() || target.empty() || (kind == "metadata_set" && field_name.empty())) revision_conflict(snap.revision());
      for (const auto& op : request.ops) if (const auto* metadata = std::get_if<SetMetadata>(&op))
        if ((kind == "metadata_set" && target == metadata->scope && field_name == metadata->field) || (kind == "file_remove" && metadata->scope == "file:" + target)) revision_conflict(snap.revision());
    }
  }
}
void append_history(manifest::ManifestDocument& doc, const std::string& op_id, const std::string& kind, const std::string& now,
                    const std::string& app, Json changes, const Json& details) {
  for (auto& change : changes) {
    const auto change_kind = string(change,"kind");
    for (auto i = change.begin(); i != change.end(); ++i) {
      const bool named_string = i.value().is_string() && manifest::is_named_change_field(change_kind,i.key());
      if (i.key() != "kind" && i.key() != "target" && !named_string)
        i.value() = take(doc.make_opaque(i.value().dump()));
    }
  }
  auto& j = doc.known(); j["history"]["entries"].push_back({{"revision",j["asset"]["revision"]},{"operation_id",op_id},{"timestamp",now},{"operation",kind},{"tool",{{"application_version",app}}},{"changes",std::move(changes)},{"details",take(doc.make_opaque(details.dump()))}});
}
manifest::IntendedChanges intended_changes(const std::vector<manifest::OpaqueKey>& before, const manifest::ManifestDocument& after,
                                         const std::string& op, const std::set<std::string>& removed, std::vector<manifest::OpaqueKey>& removed_owners) {
  const auto next = after.opaque_keys(); std::set<manifest::OpaqueKey> old(before.begin(),before.end()), current(next.begin(),next.end()); manifest::IntendedChanges intended;
  const auto history_scope = "/history/" + op + "/";
  for (const auto& key : current) if (!old.contains(key)) {
    if (!key.value.starts_with(history_scope)) fail(Errc::internal,"am.repo.unexpected_opaque_change","New owner outside new history entry: " + key.value);
    intended.owners.insert(key);
  }
  for (const auto& key : old) if (!current.contains(key)) {
    bool allowed = false; for (const auto& id : removed) if (key.value.starts_with("/files/" + id + "/")) allowed = true;
    if (!allowed) fail(Errc::internal,"am.repo.unexpected_opaque_change","Lost owner outside removed files: " + key.value);
    intended.owners.insert(key); removed_owners.push_back(key);
  }
  return intended;
}
U64 estimate(const Json& j, const std::string& manifest, const Plan& plan, const asset_zip::ZipArchive* old, const std::set<std::string>& opaque) {
  U64 total = 98 + manifest.size() + 128 + 26;
  std::map<std::string,U64> compressed_sizes; if (old) for (const auto& e : old->entries()) compressed_sizes.emplace(e.raw_name,e.compressed_size);
  std::vector<std::string> names; for (const auto& f : registered(j)) names.push_back(f.path); for (const auto& p : opaque) names.push_back(p);
  for (const auto& name : names) {
    U64 size = 0; auto found = plan.sources.find(name);
    if (found != plan.sources.end()) size = found->second.size;
    else if (old) { auto e = compressed_sizes.find(name); if (e == compressed_sizes.end()) fail(Errc::corrupt_data,"am.manifest.missing_stream",name); size = e->second; }
    if (size > UINT64_MAX - total || 128 + 2*name.size() > UINT64_MAX-total-size) fail(Errc::insufficient_space,"am.repo.file_too_large","Estimated size overflow");
    total += size + 128 + 2*name.size();
  }
  return total;
}
void write_text(asset_zip::ZipWriter& writer, const std::string& text) {
  asset_zip::EntryOptions options; options.uncompressed_size = text.size(); auto entry = take(writer.begin_entry("manifest.json",options)); take(entry.write(std::as_bytes(std::span(text)))); take(entry.finish());
}
void write_source(asset_zip::ZipWriter& writer, const std::string& name, const NewSource& fresh) {
  check_stop(fresh.stop);
  if(fresh.archive) {
    auto& zip=archive_pass(*fresh.archive,true,fresh.stop,fresh.file_id,fresh.failure);
    auto stream=read_archive(zip.open_entry(fresh.archive_index),fresh.file_id,fresh.failure);
    asset_zip::EntryOptions options; options.uncompressed_size=fresh.size; options.method=fresh.method;
    auto entry=take(writer.begin_entry(name,options)); std::array<std::byte,65536> bytes{};
    for(;;) {
      check_stop(fresh.stop);
      const auto n=read_archive(stream.read(bytes),fresh.file_id,fresh.failure); if(!n) break;
      take(entry.write(std::span(bytes).first(n)));
    }
    const auto actual=take(entry.finish());
    if(actual.uncompressed_size!=fresh.size || !actual.sha256 || hex(*actual.sha256)!=fresh.sha ||
        (fresh.expected_sha256 && hex(*actual.sha256)!=*fresh.expected_sha256))
      changed_source(fresh.file_id,fresh.failure,"Archive record SHA-256 changed between passes");
    return;
  }
  auto source = read_source(asset_zip::file_source(fresh.path),fresh.file_id,fresh.failure);
  if (source->size() != fresh.size) changed_source(fresh.file_id,fresh.failure,"Source size changed between passes");
  asset_zip::EntryOptions options; options.uncompressed_size = fresh.size; options.method = fresh.method;
  auto entry = take(writer.begin_entry(name,options)); std::array<std::byte,65536> bytes{};
  for (U64 offset = 0; offset < source->size();) {
    check_stop(fresh.stop);
    auto n = read_source(source->read_at(offset,bytes),fresh.file_id,fresh.failure); if (!n) changed_source(fresh.file_id,fresh.failure,"Source EOF changed between passes"); take(entry.write(std::span(bytes).first(n))); offset += n;
  }
  auto actual = take(entry.finish());
  if (actual.uncompressed_size != fresh.size || !actual.sha256 || hex(*actual.sha256) != fresh.sha || (fresh.expected_sha256 && hex(*actual.sha256) != *fresh.expected_sha256)) changed_source(fresh.file_id,fresh.failure,"Source SHA-256 changed between passes");
}
// Edit only schema-defined string tokens. Opaque numeric lexemes never enter a
// floating-point DOM, including core-typed values stored behind codec handles.
struct CopyRemap {
  std::string text;
  json::Tree tree;
  std::map<std::string,std::string> ids, field_types;
  std::vector<std::size_t> references, identities, storage_paths;
  explicit CopyRemap(std::string source, const manifest::CodecLimits& limits)
      : text(std::move(source)), tree(take(json::parse_strict(text,{limits.max_depth,limits.max_bytes}))) {}
  std::size_t member(std::size_t node, std::string_view key) const {
    if (node == json::no_node || tree.nodes[node].kind != json::Kind::object) return json::no_node;
    for (auto i : tree.nodes[node].children) if (tree.nodes[i].edge == key) return i;
    return json::no_node;
  }
  std::string value(std::size_t node) const { return node == json::no_node ? std::string{} : tree.nodes[node].decoded; }
  template<class F> void each(std::size_t node, F f) {
    if (node != json::no_node && tree.nodes[node].kind == json::Kind::array)
      for (auto i : tree.nodes[node].children) f(i);
  }
  void ref(std::size_t node) { if (node != json::no_node) references.push_back(node); }
  void ref(std::size_t node, std::string_view key) { ref(member(node,key)); }
  void identity(std::size_t node) {
    const auto id = member(node,"id");
    if (id != json::no_node) { identities.push_back(id); ids.emplace(value(id),std::string{}); }
  }
  void dependencies(std::size_t node) {
    each(member(node,"refs"),[&](auto r) { ref(r,"file_id"); ref(r,"group_id"); ref(r,"owner_id"); });
  }
  void authors(std::size_t node) { each(node,[&](auto a) { identity(a); }); }
  void claims(std::size_t node) {
    each(node,[&](auto c) {
      identity(c);
      const auto target = member(c,"target"); ref(target,"item_id");
      each(member(c,"evidence"),[&](auto e) {
        for (auto key : {"file_id","source_id","input_object_id","source_entry_id"}) ref(e,key);
      });
      const auto v = member(c,"value");
      const auto kind = value(member(target,"kind")), field = value(member(target,"field"));
      if (kind == "standard" && field == "authors") authors(v);
      if (kind == "standard" && field == "license") ref(v,"file_ref");
      if (kind == "custom" && field_types[value(member(target,"field_id"))] == "file_ref") ref(v);
    });
  }
  void metadata(std::size_t owner) {
    const auto m = member(owner,"metadata");
    authors(member(m,"authors")); ref(member(m,"license"),"file_ref");
    each(member(m,"custom_values"),[&](auto v) {
      const auto type = field_types[value(member(v,"field_id"))];
      if (type.starts_with("plugin:")) fail(Errc::unsupported,"am.repo.copy_plugin_values_unsupported","Plugin field value cannot be remapped");
      if (type == "file_ref") ref(v,"value");
      dependencies(member(v,"dependencies"));
    });
    // A plugin claim is also a value of that field.
    each(member(m,"claims"),[&](auto c) {
      const auto target = member(c,"target");
      if (value(member(target,"kind")) == "custom" && member(c,"value") != json::no_node &&
          field_types[value(member(target,"field_id"))].starts_with("plugin:"))
        fail(Errc::unsupported,"am.repo.copy_plugin_values_unsupported","Plugin claim value cannot be remapped");
    });
    claims(member(m,"claims"));
  }
  void collect() {
    each(member(0,"field_definitions"),[&](auto d) {
      const auto type = value(member(d,"type")); field_types[value(member(d,"id"))] = type;
      if (type == "file_ref") ref(d,"default");
      if (type.starts_with("plugin:") && member(d,"default") != json::no_node)
        fail(Errc::unsupported,"am.repo.copy_plugin_values_unsupported","Plugin default value cannot be remapped");
    });
    const auto asset = member(0,"asset"); identity(asset); metadata(asset);
    each(member(0,"files"),[&](auto f) {
      identity(f); metadata(f); storage_paths.push_back(member(f,"storage_path"));
      each(member(f,"derived_from"),[&](auto r) { ref(r,"file_id"); });
    });
    each(member(0,"groups"),[&](auto g) {
      identity(g); metadata(g); ref(g,"parent_id"); each(member(g,"file_refs"),[&](auto r) { ref(r); });
    });
    const auto previews = member(0,"previews"); ref(previews,"primary_id");
    each(member(previews,"items"),[&](auto p) {
      identity(p); ref(p,"file_id"); each(member(p,"group_refs"),[&](auto r) { ref(r); });
    });
    each(member(member(0,"origin"),"sources"),[&](auto s) { identity(s); claims(member(s,"claims")); });
    each(member(0,"inputs"),[&](auto input) {
      identity(input); each(member(input,"source_refs"),[&](auto r) { ref(r); });
      each(member(input,"objects"),[&](auto o) { identity(o); ref(o,"parent_object_id"); ref(o,"parent_entry_id"); });
      each(member(input,"entries"),[&](auto e) { identity(e); ref(e,"object_id"); ref(e,"file_id"); });
    });
  }
  std::string render(const std::string& source_id, const std::string& new_id) {
    std::set<std::string> used;
    for (const auto& [old,next] : ids) { (void)next; used.insert(old); }
    if (used.contains(new_id)) fail(Errc::invalid_argument,"am.repo.invalid_operation","New AssetID must differ from source instance IDs");
    used.insert(new_id);
    for (auto& [old,next] : ids) {
      if (old == source_id) next = new_id;
      else { do { next = Uuid::v4().to_string(); } while (!used.insert(next).second); }
    }
    std::map<std::size_t,std::string> edits;
    auto replace = [&](std::size_t node) {
      const auto found = ids.find(value(node));
      if (found == ids.end()) fail(Errc::unsupported,"am.repo.copy_remap_incomplete","Reference has no current instance");
      edits[node] = Json(found->second).dump();
    };
    for (auto node : identities) replace(node);
    for (auto node : references) replace(node);
    // Only InputID prefixes are rewritten; generated/ paths are retained.
    each(member(0,"inputs"),[&](auto input) {
      const auto old = value(member(input,"id"));
      const auto prefix = "content/" + old + "/";
      for (auto node : storage_paths) {
        const auto path = value(node);
        if (path.starts_with(prefix)) edits[node] = Json("content/"+ids.at(old)+"/"+path.substr(prefix.size())).dump();
      }
    });
    std::string out; std::size_t position = 0;
    // Token indices are in source order; edited nodes are scalar and disjoint.
    for (const auto& [node,replacement] : edits) {
      const auto& n = tree.nodes[node]; out.append(text,position,n.begin-position); out += replacement; position = n.end;
    }
    out.append(text,position,std::string::npos); return out;
  }
};
std::string copy_pointer(const json::Tree& tree, std::size_t node) {
  std::vector<std::string> parts;
  while (tree.nodes[node].parent != json::no_node) {
    const auto& n = tree.nodes[node]; const auto& parent = tree.nodes[n.parent];
    std::string part = parent.kind == json::Kind::array ? std::to_string(n.index) : n.edge;
    std::string escaped; for (char c : part) { if (c == '~') escaped += "~0"; else if (c == '/') escaped += "~1"; else escaped += c; }
    parts.push_back(std::move(escaped)); node = n.parent;
  }
  std::string out; for (auto i = parts.rbegin(); i != parts.rend(); ++i) out += "/"+*i; return out;
}
void check_copy_remap(const std::string& output, const std::map<std::string,std::string>& ids,
                      const std::string& source_id, const manifest::CodecLimits& limits) {
  const auto tree = take(json::parse_strict(output,{limits.max_depth,limits.max_bytes}));
  for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
    const auto& n = tree.nodes[i];
    for (const auto& [old,next] : ids) {
      (void)next;
      const bool in_key = n.edge.find(old) != std::string::npos;
      const bool in_value = n.kind == json::Kind::string && n.decoded.find(old) != std::string::npos;
      if (!in_key && !in_value) continue;
      const auto pointer = copy_pointer(tree,i);
      if (!in_key && old == source_id && n.decoded == old && pointer == "/history/entries/0/details/source_asset/asset_id") continue;
      throw error(Errc::unsupported,"am.repo.copy_remap_incomplete",pointer);
    }
  }
}
struct IndependentCopy {
  const AssetSnapshot& source;
  std::map<std::string,std::string> ids;
  std::map<std::string,std::size_t> entries;
};
bool is_independent_copy(const AssetSnapshot& snapshot, const manifest::CodecLimits& limits) {
  CopyRemap text(snapshot.manifest_text(),limits);
  const auto entries = text.member(text.member(0,"history"),"entries");
  bool found = false;
  text.each(entries,[&](auto entry) {
    const auto details = text.member(entry,"details");
    found = found || (text.value(text.member(entry,"operation")) == "asset_created" &&
                     text.value(text.member(details,"creation_kind")) == "independent_copy");
  });
  return found;
}
template<class Repo> ChangeResult commit_update(Repo& repo, const AssetRef& ref, manifest::ManifestDocument doc, Plan plan,
                                               const AssetSnapshot* old, const std::string& op, const std::string& kind,
                                               const Json& details, VerificationReport* failure_report,
                                               const IndependentCopy* copy = nullptr, bool guard_identity = false) {
  ChangeResult result; result.ref = ref; result.op_id = op; result.revision = repository::revision(doc.known());
  const auto allowed = old ? opaque_paths(old->manifest().known(),old->archive()) : std::set<std::string>{};
  const auto old_keys = old ? old->manifest().opaque_keys() : std::vector<manifest::OpaqueKey>{};
  append_history(doc,op,kind,string(doc.known()["asset"],"modified_at"),repo.options.application_version,plan.changes,details);
  if (repo.options.test_hooks.mutate_manifest) repo.options.test_hooks.mutate_manifest(doc);
  // Render first so invalid handles produce the codec's structured error.
  const auto output = take(manifest::write_manifest(doc));
  auto intended = copy ? manifest::IntendedChanges{} : intended_changes(old_keys,doc,op,plan.removed_ids,result.removed_opaque_owners);
  if (copy) check_copy_remap(output,copy->ids,copy->source.ref().asset_id,repo.options.codec_limits);
  if (old) take(manifest::verify_preserved(old->manifest_text(),output,intended,repo.options.codec_limits));
  else {
    // Creation has no old document; its only opaque owners are the new history.
    (void)intended;
  }
  try { repo.validate(doc); }
  catch (const Error& e) { if (failure_report) { failure_report->v1 = CheckStatus::passed; failure_report->v2 = CheckStatus::failed; failure_report->error = e; } throw; }
  collision_check(doc.known(),allowed);
  const auto estimated = copy ? estimate(copy->source.manifest().known(),output,plan,&copy->source.archive(),allowed)
                              : estimate(doc.known(),output,plan,old ? &old->archive() : nullptr,allowed);
  if (estimated > repo.capabilities.max_file_size) fail(Errc::insufficient_space,"am.repo.file_too_large","Estimate exceeds storage maximum file size");
  // Release preflight handles so the packing pass reopens and checks a fresh snapshot.
  for(auto& [path,state]:plan.archives) { (void)path; state->preflight.reset(); }
  if (repo.options.test_hooks.after_preflight) repo.options.test_hooks.after_preflight();
  asset_zip::ReadBudget budget(repo.options.read_budget); commit::CommitRequest request(budget);
  request.library_root = repo.root; request.asset_id = ref.asset_id; request.target_path = repo.path(ref); request.op_id = op;
  request.stop = plan.stop;
  if (guard_identity) request.precondition = [&repo,ref]() -> Result<void> {
    return guard<void>([&]() -> Result<void> { repo.require_absent(ref); return {}; });
  };
  request.kind = old ? "update" : "create"; if (old) { request.base = old->base(); request.expected_rev = static_cast<std::int64_t>(old->revision()); }
  request.estimated_size = estimated; request.capabilities = repo.capabilities; request.zip_limits = repo.options.zip_limits; request.lock_timeout = repo.options.lock_timeout; request.journal = repo.journal; request.hooks = repo.options.test_hooks.commit;
  std::map<std::string,std::size_t> original_indices; if (old) for (const auto& e : old->archive().entries()) original_indices.emplace(e.raw_name,e.index);
  request.producer = [&](asset_zip::ZipWriter& writer) -> Result<void> {
    return guard<void>([&]() -> Result<void> {
      write_text(writer,output);
      auto emit = [&](const std::string& name) {
        if (copy) {
          asset_zip::RawCopyOptions options; options.drop_unlisted = true; options.target_name = name;
          take(writer.copy_raw(copy->source.archive(),copy->entries.at(name),budget,options)); return;
        }
        auto fresh = plan.sources.find(name); if (fresh != plan.sources.end()) { write_source(writer,name,fresh->second); return; }
        if (!old) fail(Errc::corrupt_data,"am.manifest.missing_stream",name);
        auto index = original_indices.find(name); if (index == original_indices.end()) fail(Errc::corrupt_data,"am.manifest.missing_stream",name);
        asset_zip::RawCopyOptions raw_options;
        raw_options.drop_unlisted = true;
        take(writer.copy_raw(old->archive(),index->second,budget,raw_options));
      };
      for (const auto& f : registered(doc.known())) emit(f.path);
      if (old) for (const auto& e : old->archive().entries()) if (allowed.contains(e.raw_name)) emit(e.raw_name);
      return {};
    });
  };
  if (copy) request.on_committed = [&](const db::PendingOperation&) -> Result<void> {
    if (repo.options.on_indexed) { try { repo.options.on_indexed({ref,result.revision,op,"create"}); } catch (...) {} }
    return {};
  };
  request.verifier = [&](const asset_zip::ZipArchive& zip) -> Result<void> {
    return guard<void>([&]() -> Result<void> {
      auto actual = read_manifest(zip,repo.options.codec_limits,budget);
      result.verification = repo.check(ref,zip,actual.document,VerifyLevel::full,{},budget,allowed,op);
      if (!result.verification.error && !result.verification.v3_complete)
        result.verification.error = error(Errc::corrupt_data,"am.manifest.incomplete","Semantic validation did not complete");
      if (failure_report) *failure_report = result.verification;
      if (result.verification.error) return *result.verification.error;
      if (repo.options.test_hooks.verifier_barrier) repo.options.test_hooks.verifier_barrier(); return {};
    });
  };
  auto committed = commit::commit_zip(request);
  if (!committed) {
    if (result.verification.error) throw *result.verification.error;
    const auto& e = committed.error();
    if (e.message_id == "am.commit.verification_failed") {
      auto cause = error(e.code,"am.manifest.stream_mismatch",e.ctx.detail).caused_by(e);
      if (failure_report) { failure_report->error = cause; failure_report->v4.push_back({"","temporary ZIP",StreamStatus::failed,cause}); }
      throw cause;
    }
    throw e;
  }
  result.commit = std::move(committed).value();
  if (!copy && repo.options.on_indexed) { try { repo.options.on_indexed({ref,result.revision,op,kind}); } catch (...) { /* Observers cannot undo an indexed commit. */ } }
  return result;
}
} // namespace

Result<ChangeResult> AssetRepository::create(const CreateRequest& request, VerificationReport* failure_report) {
  if (failure_report) *failure_report = {};
  return guard<ChangeResult>([&] {
    if(request.source_failure) request.source_failure->reset();
    check_stop(request.stop);
    impl_->require_write(); AssetRef ref{impl_->options.library_id,identifier(request.asset_id)}; Impl::Lease lock(*impl_,ref);
    if (request.asset_id) impl_->require_absent(ref);
    if (std::filesystem::exists(paths::native(impl_->path(ref)))) {
      if (request.op_id) {
        auto old = take(snapshot(ref));
        for (const auto& entry : array(old.manifest().known()["history"],"entries"))
          if (string(entry,"operation_id") == *request.op_id) fail(Errc::conflict,"am.repo.duplicate_operation_id","OperationID already exists in history");
      }
      fail(Errc::conflict,"am.repo.asset_exists","Asset already exists");
    }
    if (request.creation_kind != "user" && request.creation_kind != "import") fail(Errc::invalid_argument,"am.repo.invalid_operation","Invalid creation kind");
    const auto now = timestamp(impl_->options.clock()), op = identifier(request.op_id);
    Json j{{"format",{{"id",format_id},{"version",{{"major",1},{"minor",0}}}}},{"asset",{{"id",ref.asset_id},{"revision",1},{"created_at",now},{"modified_at",now}}},{"files",Json::array()},{"history",{{"entries",Json::array()}}}};
    if (request.metadata) j["asset"]["metadata"] = *request.metadata;
    std::vector<ChangeOperation> paths_to_check; for (const auto& add : request.files) paths_to_check.emplace_back(add);
    precheck_add_paths(j,paths_to_check,{});
    Plan plan; plan.stop = request.stop; plan.failure = request.source_failure;
    if (plan.failure) plan.failure->reset();
    if (request.origin) j["origin"] = *request.origin;
    for (const auto& add : request.files) add_files(j,plan,add,{});
    auto doc = take(manifest::parse_manifest(j.dump(),impl_->options.codec_limits));
    auto result = guard<ChangeResult>([&] { return commit_update(*impl_,ref,std::move(doc),std::move(plan),nullptr,op,"asset_created",Json{{"creation_kind",request.creation_kind}},failure_report,nullptr,request.asset_id.has_value()); });
    if (!result && result.error().message_id == "am.commit.base_changed") fail(Errc::conflict,request.asset_id ? "am.repo.asset_id_exists" : "am.repo.asset_exists","Asset appeared during creation");
    return take(std::move(result));
  });
}
Result<CopyResult> AssetRepository::copy_independent(const CopyRequest& request) {
  return guard<CopyResult>([&] {
    impl_->require_write();
    auto source = take(snapshot(request.source));
    if (!source.writable()) fail(Errc::unsupported,"am.repo.write_unsupported_version","Format is read-only");
    const auto& current = source.manifest().known();
    if (current.contains("extensions") && !current["extensions"].empty())
      fail(Errc::unsupported,"am.repo.copy_extensions_unsupported","Extension clone contracts are unavailable");
    if (!array(current,"historical_files").empty())
      fail(Errc::unsupported,"am.repo.copy_history_unsupported","Historical files cannot be retained in revision one");
    std::set<std::string> files;
    for (const auto& f : array(current,"files")) files.insert(string(f,"id"));
    for (const auto& f : array(current,"files")) for (const auto& r : array(f,"derived_from"))
      if (!files.contains(string(r,"file_id")))
        fail(Errc::unsupported,"am.repo.copy_history_unsupported","Derived file is not current");
    if (!opaque_paths(current,source.archive()).empty())
      fail(Errc::unsupported,"am.repo.copy_opaque_entries","Unregistered ZIP entries cannot be remapped");
    const AssetRef ref{impl_->options.library_id,identifier(request.new_asset_id)};
    Impl::Lease lock(*impl_,ref);
    if (request.new_asset_id) impl_->require_absent(ref);
    if (std::filesystem::exists(paths::native(impl_->path(ref))) ||
        std::filesystem::exists(paths::native(impl_->root / "trash" / (ref.asset_id+".zip"))))
      fail(Errc::conflict,"am.repo.asset_exists","Asset already exists");
    const auto op = identifier(request.op_id), now = timestamp(impl_->options.clock());
    auto document = take(manifest::parse_manifest(source.manifest_text(),impl_->options.codec_limits));
    auto& j = document.known();
    j["history"] = {{"entries",Json::array()}};
    j["asset"]["revision"] = 1; j["asset"]["created_at"] = now; j["asset"]["modified_at"] = now;
    // Readers accept omitted membership; new writers always emit the array.
    if (auto groups = j.find("groups"); groups != j.end())
      for (auto& group : *groups) if (!group.contains("file_refs")) group["file_refs"] = Json::array();
    CopyRemap remap(take(manifest::write_manifest(document)),impl_->options.codec_limits);
    remap.collect();
    // Removed history IDs still participate in the completeness check.
    for (const auto& h : array(current["history"],"entries")) remap.ids.emplace(string(h,"operation_id"),std::string{});
    auto copy_doc = take(manifest::parse_manifest(remap.render(request.source.asset_id,ref.asset_id),impl_->options.codec_limits));
    IndependentCopy copy{source,std::move(remap.ids),{}};
    const auto old_files = registered(current), new_files = registered(copy_doc.known());
    for (std::size_t i = 0; i < old_files.size(); ++i) {
      const auto& name = old_files[i].path;
      auto found = std::find_if(source.archive().entries().begin(),source.archive().entries().end(),[&](const auto& e) { return e.raw_name == name; });
      if (found == source.archive().entries().end()) fail(Errc::corrupt_data,"am.manifest.missing_stream",name);
      copy.entries.emplace(new_files[i].path,found->index);
    }
    const Json details{{"creation_kind","independent_copy"},{"source_asset",{{"asset_id",request.source.asset_id},{"revision",source.revision()}}}};
    auto result = guard<ChangeResult>([&] {
      return commit_update(*impl_,ref,std::move(copy_doc),Plan{},nullptr,op,"asset_created",details,nullptr,&copy,request.new_asset_id.has_value());
    });
    if (!result && result.error().message_id == "am.commit.base_changed")
      fail(Errc::conflict,request.new_asset_id ? "am.repo.asset_id_exists" : "am.repo.asset_exists","Asset appeared during creation");
    return CopyResult{ref,source.revision(),take(std::move(result))};
  });
}
Result<ChangeResult> AssetRepository::change(const ChangeRequest& request, VerificationReport* failure_report) {
  if (failure_report) *failure_report = {};
  return guard<ChangeResult>([&] {
    impl_->require_write(); (void)impl_->path(request.ref); Impl::Lease lock(*impl_,request.ref);
    bool metadata = false, files = false;
    for (const auto& op : request.ops) { if (std::holds_alternative<SetMetadata>(op)) metadata = true; else files = true; }
    if (metadata && files) fail(Errc::invalid_argument,"am.repo.mixed_operations","A transaction must have one operation category");
    const auto op = identifier(request.op_id);
    for (int attempt = 0; attempt < 2; ++attempt) {
      auto old = take(snapshot(request.ref)); if (!old.writable()) fail(Errc::unsupported,"am.repo.write_unsupported_version","Format is read-only");
      if (request.op_id) for (const auto& entry : array(old.manifest().known()["history"],"entries"))
        if (string(entry,"operation_id") == op) fail(Errc::conflict,"am.repo.duplicate_operation_id","OperationID already exists in history");
      rebase(old,request,impl_->options.codec_limits);
      auto doc = take(manifest::parse_manifest(old.manifest_text(),impl_->options.codec_limits)); Plan plan;
      apply(doc.known(),plan,request.ops,old.revision()+1,opaque_paths(old.manifest().known(),old.archive()));
      if (doc.known() == old.manifest().known() && plan.sources.empty()) {
        ChangeResult result; result.ref = request.ref; result.revision = old.revision(); result.no_op = true;
        asset_zip::ReadBudget budget(impl_->options.read_budget); result.verification = impl_->check(request.ref,old.archive(),old.manifest(),VerifyLevel::quick,{},budget); return result;
      }
      doc.known()["asset"]["revision"] = old.revision()+1; doc.known()["asset"]["modified_at"] = timestamp(impl_->options.clock());
      Json details = Json::object(); if (request.reason) details["reason"] = *request.reason;
      auto result = guard<ChangeResult>([&] { return commit_update(*impl_,request.ref,std::move(doc),std::move(plan),&old,op,metadata ? "metadata_changed" : "files_changed",details,failure_report); });
      if (result) { if (request.expected_rev < old.revision()) result->rebased_from = request.expected_rev; return std::move(result).value(); }
      if (result.error().message_id != "am.commit.base_changed") throw result.error();
      if (attempt == 1) revision_conflict(old.revision());
    }
    fail(Errc::internal,"am.repo.invalid_operation","Retry exhausted");
  });
}
} // namespace am::repository
