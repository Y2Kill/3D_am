#include "am/application/logging.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <limits>
#include <mutex>
#include <regex>
#include <thread>

#include "am/foundation/paths.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace am::application {
namespace {
namespace fs = std::filesystem;
unsigned long process_id() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<unsigned long>(::getpid());
#endif
}
std::string basename(std::time_t time) {
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char text[32]{};
  std::strftime(text, sizeof(text), "am-%Y%m%dT%H%M%SZ-", &tm);
  return std::string(text) + std::to_string(process_id()) + ".jsonl";
}
void quota(const LoggingOptions& options) {
  std::error_code error;
  const auto root = fs::weakly_canonical(paths::native(options.directory), error);
  if (error) return;
  const std::regex pattern(R"(^am-[0-9]{8}T[0-9]{6}Z-([0-9]+)(\.[0-9]+)?\.jsonl$)");
  struct Item {
    fs::path path;
    std::uint64_t size;
    std::string name;
    bool own;
    fs::file_time_type modified;
  };
  std::vector<Item> files;
  std::uint64_t total = 0;
  const fs::directory_iterator end;
  fs::directory_iterator cursor(paths::native(root), error);
  for (; !error && cursor != end; cursor.increment(error)) {
    const auto status = cursor->symlink_status(error);
    if (error) {
      error.clear();
      continue;
    }
    if (!fs::is_regular_file(status)) continue;
    const auto encoded = cursor->path().filename().u8string();
    const std::string name(encoded.begin(), encoded.end());
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) continue;
    const auto size = cursor->file_size(error);
    if (error) {
      error.clear();
      continue;
    }
    const auto modified = cursor->last_write_time(error);
    if (error) {
      error.clear();
      continue;
    }
    total = size > (std::numeric_limits<std::uint64_t>::max)() - total ? (std::numeric_limits<std::uint64_t>::max)()
                                                                       : total + size;
    files.push_back({cursor->path(), size, name, match[1].str() == std::to_string(process_id()), modified});
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
    return a.modified == b.modified ? a.name < b.name : a.modified < b.modified;
  });
  for (const auto& file : files) {
    if (total <= options.max_total_bytes) break;
    if (file.own) continue;
    const auto checked = fs::weakly_canonical(paths::native(file.path), error);
    if (error || checked.parent_path() != root) {
      error.clear();
      continue;
    }
    const auto status = fs::symlink_status(paths::native(file.path), error);
    if (error || fs::is_symlink(status)) {
      error.clear();
      continue;
    }
    if (options.before_remove && !options.before_remove(checked)) continue;
    if (fs::remove(paths::native(checked), error)) total -= std::min(total, file.size);
    error.clear();
  }
}
class FileLog final : public log::Sink {
 public:
  LoggingOptions options;
  std::shared_ptr<spdlog::logger> logger;
  std::mutex mutex;
  std::condition_variable changed;
  bool ending = false, failed = false;
  std::uint64_t dropped = 0;
  std::thread timer;
  explicit FileLog(LoggingOptions value) : options(std::move(value)) {}
  ~FileLog() override {
    {
      const std::lock_guard lock(mutex);
      ending = true;
    }
    changed.notify_all();
    if (timer.joinable()) timer.join();
    try {
      const std::lock_guard lock(mutex);
      if (logger) {
        notice();
        logger->flush();
      }
    } catch (...) {
    }
  }
  log::Level min_level() const noexcept override { return options.level; }
  bool emit(const log::Record& record) {
    if (options.before_write && !options.before_write()) return false;
    auto encoded = log::serialize(record);
    if (encoded.empty()) return false;
    encoded.pop_back();
    failed = false;
    logger->log(static_cast<spdlog::level::level_enum>(record.level),
                spdlog::string_view_t(encoded.data(), encoded.size()));
    return !failed;
  }
  bool notice() {
    if (!dropped) return true;
    const log::Field field{"count", dropped};
    const log::Record notice{
        log::Level::warning, "logging", "log.dropped", "Log records dropped", std::span(&field, 1), nullptr, {}};
    if (!emit(notice)) return false;
    dropped = 0;
    return true;
  }
  void write(const log::Record& record) noexcept override {
    try {
      const std::lock_guard lock(mutex);
      if (!notice() || !emit(record)) ++dropped;
    } catch (...) {
      try {
        const std::lock_guard lock(mutex);
        ++dropped;
      } catch (...) {
      }
    }
  }
};
}  // namespace
Result<std::shared_ptr<log::Sink>> open_file_log(const LoggingOptions& options) {
  try {
    if (options.directory.empty() || !options.max_file_bytes || !options.max_files_per_process ||
        !options.max_total_bytes || options.flush_interval.count() <= 0 ||
        options.max_file_bytes > (std::numeric_limits<std::size_t>::max)())
      return Error::make(Errc::invalid_argument, "Invalid logging configuration").id("am.log.options_invalid");
    fs::create_directories(paths::native(options.directory));
    quota(options);
    auto name = basename(std::time(nullptr));
    auto time = std::time(nullptr);
    while (fs::exists(paths::native(options.directory / name))) name = basename(++time);
    auto target = std::make_shared<FileLog>(options);
    const auto native = paths::native(options.directory / name);
#ifdef SPDLOG_WCHAR_FILENAMES
    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        native.wstring(), static_cast<std::size_t>(options.max_file_bytes), options.max_files_per_process - 1);
#else
    const auto utf8 = native.u8string();
    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(std::string(utf8.begin(), utf8.end()),
                                                                       static_cast<std::size_t>(options.max_file_bytes),
                                                                       options.max_files_per_process - 1);
#endif
    file->set_pattern("%v");
    target->logger = std::make_shared<spdlog::logger>("am-file", file);
    target->logger->set_level(spdlog::level::trace);
    target->logger->flush_on(spdlog::level::warn);
    auto* pointer = target.get();
    target->logger->set_error_handler([pointer](const std::string&) { pointer->failed = true; });
    target->timer = std::thread([pointer] {
      std::unique_lock lock(pointer->mutex);
      while (!pointer->changed.wait_for(lock, pointer->options.flush_interval, [&] { return pointer->ending; })) {
        try {
          pointer->logger->flush();
        } catch (...) {
        }
      }
    });
    log::set_sink(target);
    return std::shared_ptr<log::Sink>(std::move(target));
  } catch (const std::exception& e) {
    return Error::make(Errc::io_error, "File logging unavailable", e.what())
        .with_severity(Severity::warning)
        .id("am.log.open_failed");
  } catch (...) {
    return Error::make(Errc::io_error, "File logging unavailable")
        .with_severity(Severity::warning)
        .id("am.log.open_failed");
  }
}
}  // namespace am::application
