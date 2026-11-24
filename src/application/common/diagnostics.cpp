#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include "am/application/asset_service.hpp"
#include "am/application/logging.hpp"
#include "am/foundation/json_text.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/unicode.hpp"
#include "am/foundation/uuid.hpp"
#include "am/foundation/version.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/storage/asset_zip/writer.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <pwd.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace am::application {
namespace {
namespace fs = std::filesystem;
Error failure(std::string id, Errc code = Errc::io_error) {
  return Error::make(code, "Diagnostic export failed").id(std::move(id));
}
template <class T>
T take(Result<T> value) {
  if (!value) throw value.error();
  return std::move(*value);
}
void take(Result<void> value) {
  if (!value) throw value.error();
}
std::string utf8(const fs::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}
Result<paths::NodeInfo> source_info(const fs::path& path, const paths::NodeDataHook& hook) {
  if (!hook) return paths::node_info(path);
  auto data = hook(path);
  if (!data) return data.error();
  return paths::classify_windows_node(*data);
}
bool local_path(const fs::path& input, const paths::NodeDataHook& hook) {
  auto text = utf8(input);
  std::replace(text.begin(), text.end(), '\\', '/');
  if (text.starts_with("//?/")) text.erase(0, 4);
  if (text.starts_with("//") || text.starts_with("UNC/") || text.find("://") != std::string::npos) return false;
#ifdef _WIN32
  const auto absolute = fs::absolute(fs::path(std::u8string(text.begin(), text.end()))).lexically_normal();
  const auto drive = GetDriveTypeW(absolute.root_path().c_str());
  if (drive == DRIVE_REMOTE || drive == DRIVE_UNKNOWN || drive == DRIVE_NO_ROOT_DIR) return false;
  auto current = absolute.root_path();
  for (const auto& part : absolute.relative_path()) {
    current /= part;
    const auto info = source_info(current, hook);
    if (!info) break;
    if (info->kind == paths::NodeKind::link) return false;
  }
#else
  const auto absolute = fs::absolute(input).lexically_normal();
  std::ifstream mounts("/proc/self/mountinfo");
  std::string line;
  while (std::getline(mounts, line)) {
    const auto split = line.find(" - ");
    if (split == std::string::npos) continue;
    std::istringstream left(line.substr(0, split)), right(line.substr(split + 3));
    std::string id, parent, device, root, mount, kind;
    left >> id >> parent >> device >> root >> mount;
    right >> kind;
    for (const auto& [encoded, decoded] : std::array<std::pair<std::string_view, std::string_view>, 3>{
             {{"\\040", " "}, {"\\011", "\t"}, {"\\134", "\\"}}}) {
      std::size_t at = 0;
      while ((at = mount.find(encoded, at)) != std::string::npos) {
        mount.replace(at, encoded.size(), decoded);
        at += decoded.size();
      }
    }
    const auto base = fs::path(mount);
    const auto relative = absolute.lexically_relative(base);
    if (relative.empty() || (!relative.empty() && *relative.begin() == "..")) continue;
    if (kind == "nfs" || kind == "nfs4" || kind == "cifs" || kind == "smb3" || kind == "smbfs" ||
        kind == "fuse.sshfs" || kind == "fuse.rclone" || kind == "fuse.davfs")
      return false;
  }
  auto current = absolute.root_path();
  for (const auto& part : absolute.relative_path()) {
    current /= part;
    const auto info = source_info(current, hook);
    if (!info) break;
    if (info->kind == paths::NodeKind::link) return false;
  }
#endif
  return true;
}
std::string now() {
  const auto sample = log::serialize(log::Record{});
  return Json::parse(sample)["ts"].get<std::string>();
}
std::string folded(std::string_view s) {
  auto value = unicode::simple_casefold(s);
  return value ? *value : std::string(s);
}
#include "log_word_ranges.inc"
bool word_at(std::string_view text, std::size_t at) {
  if (at >= text.size()) return false;
  while (at && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) --at;
  const auto lead = static_cast<unsigned char>(text[at++]);
  char32_t point = lead;
  if (lead >= 0xc0) {
    const auto count = lead < 0xe0 ? 2u : lead < 0xf0 ? 3u : 4u;
    point = lead & (0x7fu >> count);
    for (unsigned i = 1; i < count && at < text.size(); ++i)
      point = (point << 6) | (static_cast<unsigned char>(text[at++]) & 0x3f);
  }
  const auto found = std::lower_bound(std::begin(word_ranges), std::end(word_ranges), point,
                                      [](const auto& range, char32_t scalar) { return range.second < scalar; });
  return found != std::end(word_ranges) && found->first <= point;
}
std::optional<std::string> environment_value(const char* key) {
#ifdef _WIN32
  const auto name = fs::path(key).wstring();
  const auto required = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
  if (!required) return {};
  std::wstring text(required, L'\0');
  const auto size = GetEnvironmentVariableW(name.c_str(), text.data(), required);
  if (!size || size >= required) return {};
  text.resize(size);
  return utf8(fs::path(text));
#else
  if (const auto* value = std::getenv(key); value && *value) return std::string(value);
  return {};
#endif
}
void environment(std::vector<std::string>& to, const char* key) {
  if (auto value = environment_value(key)) to.push_back(std::move(*value));
}
struct Redactor {
  DiagnosticReport& report;
  std::array<unsigned char, 32> salt{};
  std::vector<std::string> users, hosts, roots;
  struct Identity {
    std::string value, target;
    bool host;
  };
  std::vector<Identity> identities;
  explicit Redactor(const DiagnosticOptions& options, DiagnosticReport& out)
      : report(out), users(options.users), hosts(options.hosts), roots(options.roots) {
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) throw failure("am.diag.random_failed");
    for (const auto* key : {"USERNAME", "USER"}) environment(users, key);
    for (const auto* key : {"COMPUTERNAME", "HOSTNAME"}) environment(hosts, key);
    for (const auto* key : {"HOME", "USERPROFILE", "TMP", "TEMP", "TMPDIR"}) environment(roots, key);
#ifdef _WIN32
    std::array<wchar_t, 32768> name{};
    DWORD size = static_cast<DWORD>(name.size());
    if (GetUserNameW(name.data(), &size)) users.push_back(utf8(fs::path(name.data())));
    size = static_cast<DWORD>(name.size());
    if (GetComputerNameW(name.data(), &size)) hosts.push_back(utf8(fs::path(name.data())));
#else
    if (auto* user = getpwuid(getuid())) {
      if (user->pw_name) users.emplace_back(user->pw_name);
      if (user->pw_dir) roots.emplace_back(user->pw_dir);
    }
    std::array<char, 1024> name{};
    if (gethostname(name.data(), name.size() - 1) == 0) hosts.emplace_back(name.data());
#endif
    if (options.library) roots.push_back(utf8(fs::absolute(*options.library)));
    for (const auto& value : hosts)
      if (!value.empty()) identities.push_back({value, folded(value), true});
    for (const auto& value : users)
      if (!value.empty()) identities.push_back({value, folded(value), false});
    std::stable_sort(identities.begin(), identities.end(), [](const auto& a, const auto& b) {
      return a.target.size() != b.target.size() ? a.target.size() > b.target.size() : a.host > b.host;
    });
    std::set<std::string> seen;
    std::erase_if(identities, [&](const auto& identity) { return !seen.insert(identity.target).second; });
  }
  std::string token(std::string_view value, std::string_view kind) {
    std::array<unsigned char, 32> digest{};
    std::size_t written = 0;
    if (!EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, salt.data(), salt.size(),
                   reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data(), digest.size(),
                   &written) ||
        written != digest.size())
      throw failure("am.diag.random_failed");
    std::string result = "<" + std::string(kind) + ":";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 4; ++i) {
      result += hex[digest[i] >> 4];
      result += hex[digest[i] & 15];
    }
    return result + ">";
  }
  void replace_names(std::string& text) {
    for (const auto& identity : identities) {
      const auto& value = identity.value;
      const auto& target = identity.target;
      const std::string_view marker = identity.host ? "<host>" : "<user>";
      auto& count = identity.host ? report.host_replacements : report.user_replacements;
      // Walk original UTF-8 boundaries; case folding can change byte lengths.
      for (std::size_t at = 0; at < text.size();) {
        if (text.compare(at, 6, "<host>") == 0 || text.compare(at, 6, "<user>") == 0) {
          at += 6;
          continue;
        }
        if (at && word_at(text, at - 1)) {
          ++at;
          continue;
        }
        bool matched = false;
        for (std::size_t end = at + 1; end <= text.size(); ++end) {
          if (end < text.size() && word_at(text, end)) continue;
          if (folded(std::string_view(text).substr(at, end - at)) == target) {
            text.replace(at, end - at, marker);
            at += marker.size();
            ++count;
            matched = true;
            break;
          }
          if (end - at > value.size() * 4 + 4) break;
        }
        if (!matched) ++at;
      }
    }
  }
  std::string string(std::string value, log::FieldKind kind = log::FieldKind::plain) {
    if (value.empty()) return value;
    static const std::regex absolute(R"((^|[^A-Za-z0-9_/])([A-Za-z]:[\\/]|\\\\|/[^/]))");
    bool path = kind == log::FieldKind::path || value == "/" || std::regex_search(value, absolute);
    const auto lower = folded(value);
    for (const auto& root : roots)
      if (!root.empty() && lower.find(folded(root)) != std::string::npos) path = true;
    if (kind == log::FieldKind::name) {
      ++report.name_replacements;
      std::string extension;
      const auto dot = value.find_last_of('.');
      const auto slash = value.find_last_of("/\\");
      if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        extension = value.substr(dot);
        if (std::any_of(extension.begin(), extension.end(), [](unsigned char c) { return c < 32; })) extension.clear();
      }
      replace_names(extension);
      return token(value, "name") + extension;
    }
    if (path) {
      ++report.path_replacements;
      return token(value, "path");
    }
    replace_names(value);
    return value;
  }
  std::string json(std::string_view input) {
    const auto tree = take(am::json::parse_strict(input));
    std::map<std::string, log::FieldKind> kinds;
    for (const auto& node : tree.nodes)
      if (node.parent == 0 && node.edge == "kinds" && node.kind == am::json::Kind::object)
        for (auto i : node.children) {
          const auto& item = tree.nodes[i];
          if (item.kind == am::json::Kind::string && (item.decoded == "path" || item.decoded == "name"))
            kinds[item.edge] = item.decoded == "path" ? log::FieldKind::path : log::FieldKind::name;
        }
    std::vector<std::string> output(tree.nodes.size());
    for (std::size_t n = tree.nodes.size(); n-- > 0;) {
      const auto& node = tree.nodes[n];
      if (node.kind == am::json::Kind::string) {
        auto kind = log::FieldKind::plain;
        if (node.parent != am::json::no_node && tree.nodes[node.parent].edge == "fields" && kinds.contains(node.edge))
          kind = kinds[node.edge];
        if (node.edge == "detail" || node.edge == "path" || node.edge == "root" || node.edge == "device_id")
          kind = log::FieldKind::path;
        if (node.edge == "name" && n > 0 && tree.nodes[node.parent].edge != "logs") kind = log::FieldKind::name;
        const bool identity =
            node.edge == "asset_id" || node.edge == "job_id" || node.edge == "library_id" || node.edge == "op_id";
        output[n] =
            Json(identity && Uuid::parse_canonical(node.decoded) ? node.decoded : string(node.decoded, kind)).dump();
      } else if (node.kind == am::json::Kind::object || node.kind == am::json::Kind::array) {
        const bool object = node.kind == am::json::Kind::object;
        auto& out = output[n];
        out = object ? "{" : "[";
        bool first = true;
        for (auto i : node.children) {
          if (!first) out += ',';
          first = false;
          if (object) out += Json(string(tree.nodes[i].edge)).dump() + ":";
          out += output[i];
          output[i].clear();
        }
        out += object ? "}" : "]";
      } else
        output[n] = input.substr(node.begin, node.end - node.begin);
    }
    return output[0];
  }
};
// Comparison uses UTC seconds plus fractional digits, so offsets and fractional
// precision cannot alter --since ordering.
struct Timestamp {
  std::int64_t seconds;
  std::string fraction;
};
std::optional<Timestamp> timestamp(std::string_view value) {
  static const std::regex pattern(
      R"(^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})(\.([0-9]+))?(Z|[+-][0-9]{2}:[0-9]{2})$)");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_match(value.begin(), value.end(), match, pattern)) return {};
  auto number = [&](unsigned i) { return std::stoi(match[i].str()); };
  const std::chrono::year_month_day date{std::chrono::year(number(1)),
                                         std::chrono::month(static_cast<unsigned>(number(2))),
                                         std::chrono::day(static_cast<unsigned>(number(3)))};
  if (!date.ok() || number(4) > 23 || number(5) > 59 || number(6) > 59) return {};
  auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::sys_days(date).time_since_epoch()).count() +
      number(4) * 3600 + number(5) * 60 + number(6);
  const auto zone = match[9].str();
  if (zone != "Z") {
    const auto hours = std::stoi(zone.substr(1, 2)), minutes = std::stoi(zone.substr(4, 2));
    if (hours > 23 || minutes > 59) return {};
    seconds -= (zone[0] == '+' ? 1 : -1) * (hours * 3600 + minutes * 60);
  }
  return Timestamp{seconds, match[8].str()};
}
bool before(const Timestamp& a, const Timestamp& b) {
  if (a.seconds != b.seconds) return a.seconds < b.seconds;
  auto left = a.fraction, right = b.fraction;
  left.resize(std::max(left.size(), right.size()), '0');
  right.resize(left.size(), '0');
  return left < right;
}
Json system_info() {
  Json system;
  system["cpu_count"] = std::thread::hardware_concurrency();
#ifdef _WIN32
  system["os"] = "windows";
  system["os_version"] = "Windows";
  using RtlVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
  if (auto module = GetModuleHandleW(L"ntdll.dll"))
    if (auto address = GetProcAddress(module, "RtlGetVersion")) {
      auto version = std::bit_cast<RtlVersion>(address);
      OSVERSIONINFOW info{};
      info.dwOSVersionInfoSize = sizeof(info);
      if (version(&info) == 0)
        system["os_version"] = std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
                               std::to_string(info.dwBuildNumber);
    }
  SYSTEM_INFO info{};
  GetNativeSystemInfo(&info);
  system["arch"] = info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64   ? "x86_64"
                   : info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
                                                                                 : "other";
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) system["memory_bytes"] = memory.ullTotalPhys;
#else
  struct utsname info{};
  if (uname(&info) == 0) {
    system["os"] = "linux";
    system["os_version"] = info.release;
    system["arch"] = info.machine;
  } else
    system = {{"os", "linux"}, {"os_version", "unknown"}, {"arch", "unknown"}};
  struct sysinfo memory{};
  if (sysinfo(&memory) == 0) system["memory_bytes"] = static_cast<std::uint64_t>(memory.totalram) * memory.mem_unit;
#endif
  return system;
}
std::string library_summary(const DiagnosticOptions& options, Redactor& redact) {
  library::OpenOptions configuration;
  configuration.mode = library::Mode::read_only;
  catalog::IndexerHooks hooks;
  hooks.before_read = [&](const fs::path& path) -> Result<void> {
    if (options.on_read) options.on_read(path);
    return {};
  };
  auto opened = take(catalog::Catalog::open(*options.library, configuration, hooks));
  auto& lib = opened.library();
  Json out{{"library_id", lib.config().library_id}, {"databases", Json::object()}, {"assets", Json::array()}};
  for (const auto kind :
       {library::DatabaseKind::index, library::DatabaseKind::userdata, library::DatabaseKind::state}) {
    const auto& info = lib.database_info(kind);
    out["databases"][kind == library::DatabaseKind::index      ? "index"
                     : kind == library::DatabaseKind::userdata ? "userdata"
                                                               : "state"] = {
        {"user_version", info.user_version ? Json(*info.user_version) : Json(nullptr)}, {"read_mode", info.read_mode}};
  }
  if (lib.database(library::DatabaseKind::index)) {
    auto query = take(db::Connection::borrow(lib.database(library::DatabaseKind::index))
                          .prepare("SELECT health,in_trash,COUNT(*) FROM assets GROUP BY health,in_trash"));
    while (take(query.step()) == db::Step::row)
      out["assets"].push_back(
          {{"health", take(query.text(0))}, {"in_trash", query.integer(1) != 0}, {"count", query.integer(2)}});
  }
  const auto& caps = lib.capabilities();
  out["capabilities"] = {{"mode", caps.mode == repository::StorageMode::full                ? "full"
                                  : caps.mode == repository::StorageMode::write_with_limits ? "write_with_limits"
                                                                                            : "read_only_default"},
                         {"max_file_size", caps.max_file_size},
                         {"readable", caps.readable},
                         {"writable", caps.writable},
                         {"remote", caps.remote},
                         {"filesystem", caps.filesystem},
                         {"durability", caps.durability == repository::Durability::verified  ? "verified"
                                        : caps.durability == repository::Durability::assumed ? "assumed"
                                                                                             : "unknown"}};
  out["capabilities"]["posix_rename"] = caps.posix_rename ? Json(*caps.posix_rename) : Json(nullptr);
  out["capabilities"]["replace_file"] = caps.replace_file ? Json(*caps.replace_file) : Json(nullptr);
  out["capabilities"]["locks_reliable"] = caps.locks_reliable ? Json(*caps.locks_reliable) : Json(nullptr);
  const auto kind = repository::detect_media(*options.library).kind;
  out["media"] = {{"kind", kind == repository::MediaKind::hdd         ? "hdd"
                           : kind == repository::MediaKind::ssd       ? "ssd"
                           : kind == repository::MediaKind::network   ? "network"
                           : kind == repository::MediaKind::removable ? "removable"
                                                                      : "unknown"}};
  db::LogQuery query;
  query.limit = 1000;
  auto anchor = take(db::Connection::borrow(lib.database(library::DatabaseKind::state))
                         .prepare("SELECT seq FROM op_log ORDER BY seq DESC LIMIT 1 OFFSET 999"));
  if (take(anchor.step()) == db::Step::row) query.after_seq = anchor.integer(0) - 1;
  auto records = take(opened.operation_log().query(query));
  std::string encoded = out.dump();
  encoded.pop_back();
  encoded += ",\"op_log\":[";
  bool first = true;
  for (const auto& row : records) {
    if (!first) encoded += ',';
    first = false;
    Json item{{"seq", row.seq},
              {"at", row.at},
              {"level", row.level},
              {"code", row.code},
              {"message", row.message},
              {"job_id", row.job_id ? Json(*row.job_id) : Json(nullptr)},
              {"asset_id", row.asset_id ? Json(*row.asset_id) : Json(nullptr)}};
    auto text = item.dump();
    text.pop_back();
    encoded += text + ",\"detail\":" + row.detail_json.value_or("null") + "}";
  }
  encoded += "]}";
  return redact.json(encoded);
}
void publish(const fs::path& temporary, const fs::path& output) {
#ifdef _WIN32
  if (!MoveFileW(paths::native(temporary).c_str(), paths::native(output).c_str()))
    throw failure("am.diag.publish_failed");
#else
  if (::link(temporary.c_str(), output.c_str()) != 0) throw failure("am.diag.publish_failed");
  if (::unlink(temporary.c_str()) != 0) throw failure("am.diag.publish_failed");
#endif
}
}  // namespace
bool valid_diagnostic_timestamp(std::string_view value) noexcept {
  try {
    return timestamp(value).has_value();
  } catch (...) {
    return false;
  }
}
Result<DiagnosticReport> export_diagnostics(const DiagnosticOptions& options) {
  fs::path temporary;
  try {
    if (options.output.empty()) return failure("am.diag.arguments_invalid", Errc::invalid_argument);
    if (!local_path(options.output, options.node_data) ||
        (options.library && !local_path(*options.library, options.node_data)))
      return failure("am.diag.local_only", Errc::unsupported);
    const auto since = options.since ? timestamp(*options.since) : std::nullopt;
    if (options.since && !since) return failure("am.diag.arguments_invalid", Errc::invalid_argument);
    if (fs::exists(paths::native(options.output))) return failure("am.diag.output_exists", Errc::conflict);
    DiagnosticReport report;
    Redactor redact(options, report);
    std::map<std::string, std::string> entries;
    Json metadata{{"format", "am-diagnostics"},
                  {"version", 1},
                  {"created_at", now()},
                  {"app", {{"version", kAppVersion}, {"build", "C++20"}}},
                  {"system", Json::parse(redact.json(system_info().dump()))},
                  {"logs", Json::array()},
                  {"redaction",
                   {{"salted", true},
                    {"rules", {"path_hmac_sha256", "name_hmac_sha256", "user_words", "host_words", "error_detail"}}}}};
    auto directory = options.log_directory;
    if (!directory)
      if (auto env = environment_value("AM_LOG_DIR")) directory = fs::path(std::u8string(env->begin(), env->end()));
    if (directory) {
      std::error_code directory_error;
      if (!local_path(*directory, options.node_data) || !fs::is_directory(paths::native(*directory), directory_error)) {
        if (options.log_directory) return failure("am.diag.logs_unavailable");
        directory.reset();
      }
    }
    if (directory) {
      report.logs_available = true;
      const std::regex pattern(R"(^am-[0-9]{8}T[0-9]{6}Z-[0-9]+(\.[0-9]+)?\.jsonl$)");
      std::vector<fs::path> files;
      for (const auto& entry : fs::directory_iterator(paths::native(*directory)))
        if (std::regex_match(entry.path().filename().string(), pattern)) files.push_back(entry.path());
      std::sort(files.begin(), files.end());
      for (const auto& file : files) {
        if (!local_path(file, options.node_data)) continue;
        const auto info = take(source_info(file, options.node_data));
        if (info.kind != paths::NodeKind::file) continue;
        if (info.cloud_placeholder) {
          report.unavailable.push_back({"log", file.filename().string(), "cloud_placeholder"});
          continue;
        }
        if (options.on_read) options.on_read(file);
        std::ifstream input(paths::native(file), std::ios::binary);
        if (!input) throw failure("am.diag.logs_unavailable");
        std::uint64_t records = 0, dropped = 0;
        std::string content, line;
        while (std::getline(input, line)) {
          const auto parsed = am::json::parse_strict(line);
          if (!parsed) {
            ++dropped;
            continue;
          }
          if (since) {
            std::optional<Timestamp> record_time;
            for (const auto& node : parsed->nodes)
              if (node.parent == 0 && node.edge == "ts" && node.kind == am::json::Kind::string)
                record_time = timestamp(node.decoded);
            if (!record_time) {
              ++dropped;
              continue;
            }
            if (before(*record_time, *since)) continue;
          }
          content += redact.json(line) + "\n";
          ++records;
        }
        if (input.bad()) throw failure("am.diag.logs_unavailable");
        const auto name = file.filename().string();
        entries["logs/" + name] = std::move(content);
        metadata["logs"].push_back({{"name", name}, {"records", records}, {"dropped_lines", dropped}});
      }
    }
    if (options.library) {
      bool unavailable = false;
      std::vector<fs::path> readable_sources;
      auto check = [&](const fs::path& file, const char* source_kind) {
        const auto info = source_info(file, options.node_data);
        if (info && info->kind == paths::NodeKind::link) throw failure("am.diag.local_only", Errc::unsupported);
        if (info && info->cloud_placeholder) {
          report.unavailable.push_back({source_kind, file.filename().string(), "cloud_placeholder"});
          unavailable = true;
        } else if (info && info->kind == paths::NodeKind::file) {
          readable_sources.push_back(file);
        }
      };
      check(*options.library / "am-library.toml", "library");
      check(*options.library / ".am/library.lock", "library");
      for (const auto* database : {"index.sqlite", "userdata.sqlite", "state.sqlite"})
        for (const auto* suffix : {"", "-wal", "-shm"})
          check(*options.library / ".am" / (std::string(database) + suffix), "database");
      if (!unavailable) {
        if (options.on_read)
          for (const auto& source : readable_sources) options.on_read(source);
        entries["library.json"] = library_summary(options, redact);
        metadata["library"] = {{"entry", "library.json"}};
      }
    }
    if (!report.unavailable.empty()) {
      metadata["unavailable"] = Json::array();
      for (const auto& item : report.unavailable)
        metadata["unavailable"].push_back(
            {{"source_kind", item.source_kind}, {"name", item.name}, {"reason", item.reason}});
      report.skipped_summary = message_text("am.diag.skipped");
      report.skipped_summary.replace(report.skipped_summary.find("{count}"), 7,
                                     std::to_string(report.unavailable.size()));
      report.skipped_summary.replace(report.skipped_summary.find("{reason}"), 8, report.unavailable.front().reason);
    }
    entries["diagnostics.json"] = metadata.dump();
    for (const auto& [name, value] : entries) {
      (void)value;
      report.entries.push_back(name);
    }
    if (options.dry_run) return report;
    const auto output = fs::absolute(options.output);
    temporary = output.parent_path() / (".am-diag-" + Uuid::v4().to_string() + ".tmp");
    auto sink = take(asset_zip::file_sink(temporary));
    auto writer = take(asset_zip::ZipWriter::create(sink));
    for (const auto& [name, content] : entries) {
      asset_zip::EntryOptions config;
      config.uncompressed_size = content.size();
      auto entry = take(writer.begin_entry(name, config));
      take(entry.write(std::as_bytes(std::span(content.data(), content.size()))));
      (void)take(entry.finish());
    }
    (void)take(writer.finish());
    publish(temporary, output);
    temporary.clear();
    return report;
  } catch (const Error& e) {
    if (!temporary.empty()) {
      std::error_code ignored;
      fs::remove(paths::native(temporary), ignored);
    }
    return e;
  } catch (const std::exception& e) {
    if (!temporary.empty()) {
      std::error_code ignored;
      fs::remove(paths::native(temporary), ignored);
    }
    return Error::make(Errc::io_error, "Diagnostic export failed", e.what()).id("am.diag.export_failed");
  } catch (...) {
    if (!temporary.empty()) {
      std::error_code ignored;
      fs::remove(paths::native(temporary), ignored);
    }
    return failure("am.diag.export_failed");
  }
}
}  // namespace am::application
