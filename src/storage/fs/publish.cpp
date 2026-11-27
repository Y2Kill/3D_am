#include "am/storage/fs/publish.hpp"

#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace am::storage::fs {
namespace {
Error error(Errc code, std::string_view id, std::string detail = {}) {
  auto result = Error::make(code, "Export filesystem operation failed", std::move(detail))
      .in("storage.fs").id(std::string(id));
  if (code == Errc::insufficient_space) result.actions = {ErrorAction::free_space,ErrorAction::abort};
  return result;
}
Error io(unsigned long code) {
#ifdef _WIN32
  const bool full = code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL;
#else
  const bool full = code == ENOSPC || code == EDQUOT;
#endif
  return error(full ? Errc::insufficient_space : Errc::io_error,
               full ? "am.export.insufficient_space" : "am.export.io_error", "OS error=" + std::to_string(code));
}
bool orphan_name(const std::filesystem::path& path) {
  const auto name = path.filename().u8string();
  const std::string text(name.begin(), name.end());
  constexpr std::string_view prefix = ".am-export-", suffix = ".tmp";
  if (text.size() != prefix.size() + 36 + suffix.size() || !text.starts_with(prefix) || !text.ends_with(suffix)) return false;
  return Uuid::parse_canonical(std::string_view(text).substr(prefix.size(), 36)).has_value();
}
}  // namespace

Result<void> publish_no_replace(const std::filesystem::path& temporary, const std::filesystem::path& target) {
  try {
#ifdef _WIN32
    if (!MoveFileExW(paths::native(temporary).c_str(), paths::native(target).c_str(), MOVEFILE_WRITE_THROUGH)) {
      const auto code = GetLastError();
      if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS || paths::node_info(target))
        return error(Errc::conflict, "am.export.target_exists");
      return io(code);
    }
#else
    bool moved = false;
#ifdef SYS_renameat2
    if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, target.c_str(), 1U) == 0) moved = true;
    else {
      const auto code = errno;
      if (code == EEXIST) return error(Errc::conflict, "am.export.target_exists");
      if (code != ENOSYS && code != EINVAL && code != EOPNOTSUPP) return io(code);
    }
#endif
    if (!moved) {
      if (::link(temporary.c_str(), target.c_str()) != 0) {
        const auto code = errno;
        if (code == EEXIST) return error(Errc::conflict, "am.export.target_exists");
        return io(code);
      }
      if (::unlink(temporary.c_str()) != 0) return io(errno);
    }
    const int directory = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory == -1) return io(errno);
    const int flushed = ::fsync(directory), code = errno;
    ::close(directory);
    if (flushed != 0) return io(code);
#endif
    return {};
  } catch (const std::bad_alloc&) { return error(Errc::resource_exhausted, "am.export.out_of_memory"); }
  catch (const std::exception& exception) { return error(Errc::io_error, "am.export.io_error", exception.what()); }
}

Result<std::size_t> cleanup_export_orphans(const std::filesystem::path& directory, const OrphanOptions& options) {
  try {
    std::size_t removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(paths::native(directory))) {
      if (!orphan_name(entry.path())) continue;
      const auto info = paths::node_info(entry.path());
      if (!info || info->kind != paths::NodeKind::file || info->cloud_placeholder) continue;
      std::error_code ec;
      const auto modified = entry.last_write_time(ec);
      if (ec || modified >= options.now || options.now - modified <= options.age) continue;
#ifdef _WIN32
      const auto handle = CreateFileW(entry.path().c_str(), DELETE | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle == INVALID_HANDLE_VALUE) continue;
      BY_HANDLE_FILE_INFORMATION attributes{};
      if (GetFileInformationByHandle(handle, &attributes) &&
          !(attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        // Recheck age from the exclusively held object, not the earlier name.
        ULARGE_INTEGER ticks{};
        ticks.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
        ticks.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
        // FILETIME counts 100 ns since 1601; convert through system_clock so
        // the file_clock epoch is an implementation detail.
        using WindowsTicks = std::chrono::duration<std::int64_t, std::ratio<1,10000000>>;
        const auto unix_time = WindowsTicks(static_cast<std::int64_t>(ticks.QuadPart) - 116444736000000000LL);
        const auto current = std::chrono::clock_cast<std::chrono::file_clock>(std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(unix_time)));
        if (current < options.now && options.now - current > options.age) {
          FILE_DISPOSITION_INFO disposition{TRUE};
          if (SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition))) ++removed;
        }
      }
      CloseHandle(handle);
#else
      const int handle = ::open(entry.path().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
      if (handle == -1) continue;
      struct stat opened{}, named{};
      if (::flock(handle, LOCK_EX | LOCK_NB) == 0 && ::fstat(handle, &opened) == 0 && S_ISREG(opened.st_mode) &&
          ::lstat(entry.path().c_str(), &named) == 0 && opened.st_dev == named.st_dev && opened.st_ino == named.st_ino) {
        const auto current = std::filesystem::last_write_time(entry.path(), ec);
        if (!ec && current < options.now && options.now - current > options.age && ::unlink(entry.path().c_str()) == 0)
          ++removed;
      }
      ::close(handle);
#endif
    }
    return removed;
  } catch (const std::bad_alloc&) { return error(Errc::resource_exhausted, "am.export.out_of_memory"); }
  catch (const std::exception& exception) { return error(Errc::io_error, "am.export.io_error", exception.what()); }
}
}  // namespace am::storage::fs
