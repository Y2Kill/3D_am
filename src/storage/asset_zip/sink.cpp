#include "am/storage/asset_zip/sink.hpp"
#include "am/foundation/paths.hpp"
#include "writer_internal.hpp"
#include <cerrno>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace am::asset_zip {
using namespace detail;
MemorySink::MemorySink() = default;
MemorySink::~MemorySink() = default;
Result<void> MemorySink::write_at(U64 offset, Bytes bytes) {
  try {
    if (discarded_) return invalid_state();
    if (offset > bytes_.max_size() || bytes.size() > bytes_.max_size() - offset)
      return write_error(Errc::resource_exhausted, "am.zip.write.file_too_large", "Memory sink offset is not representable");
    const auto end = static_cast<std::size_t>(offset) + bytes.size();
    if (end > bytes_.size()) bytes_.resize(end);
    if (!bytes.empty()) std::memcpy(bytes_.data() + static_cast<std::size_t>(offset), bytes.data(), bytes.size());
    return {};
  } catch (const std::bad_alloc&) { return write_oom(); }
  catch (const std::exception& e) { return write_error(Errc::io_error, "am.zip.io_error", e.what()); }
}
Result<void> MemorySink::flush() { if (discarded_) return invalid_state(); return {}; }
Result<void> MemorySink::discard() { bytes_.clear(); discarded_ = true; return {}; }
U64 MemorySink::size() const noexcept { return bytes_.size(); }
std::span<const std::byte> MemorySink::bytes() const noexcept { return bytes_; }
Result<std::vector<std::byte>> MemorySink::take_bytes() { return std::move(bytes_); }
Result<std::shared_ptr<MemorySink>> memory_sink() {
  try { return std::make_shared<MemorySink>(); }
  catch (const std::bad_alloc&) { return write_oom(); }
}
namespace {
Error os_error(unsigned long code, std::string operation) {
#ifdef _WIN32
  const bool full = code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL;
  const bool exists = code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS;
#else
  const bool full = code == ENOSPC;
  const bool exists = code == EEXIST;
#endif
  return write_error(full ? Errc::insufficient_space : exists ? Errc::conflict : Errc::io_error,
      full ? "am.zip.write.insufficient_space" : exists ? "am.zip.write.target_exists" : "am.zip.io_error",
      operation + " OS error=" + std::to_string(code));
}
class FileSink final : public RandomAccessSink {
 public:
  std::filesystem::path path;
  U64 length = 0;
  std::mutex mutex;
  bool discarded = false;
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  bool opened() const { return handle != INVALID_HANDLE_VALUE; }
  ~FileSink() override { if (opened()) CloseHandle(handle); }
#else
  int handle = -1;
  bool opened() const { return handle != -1; }
  ~FileSink() override { if (opened()) ::close(handle); }
#endif
  U64 size() const noexcept override { return length; }
  Result<void> write_at(U64 offset, Bytes bytes) override {
    try {
      std::lock_guard lock(mutex);
      if (!opened()) return invalid_state();
      if (offset > static_cast<U64>(std::numeric_limits<std::int64_t>::max()) || bytes.size() > static_cast<U64>(std::numeric_limits<std::int64_t>::max()) - offset)
        return write_error(Errc::resource_exhausted, "am.zip.write.file_too_large", "File offset is not representable");
      std::size_t done = 0;
      while (done < bytes.size()) {
#ifdef _WIN32
        LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset + done);
        if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) return os_error(GetLastError(), "SetFilePointerEx");
        const auto n = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + done, n, &written, nullptr)) return os_error(GetLastError(), "WriteFile");
#else
        const auto written = ::pwrite(handle, bytes.data() + done, std::min<std::size_t>(bytes.size() - done, std::numeric_limits<ssize_t>::max()), static_cast<off_t>(offset + done));
        if (written < 0) { if (errno == EINTR) continue; return os_error(errno, "pwrite"); }
#endif
        if (!written) return write_error(Errc::io_error, "am.zip.io_error", "Zero-progress sink write");
        done += static_cast<std::size_t>(written);
        length = std::max<U64>(length, offset + done);
      }
      return {};
    } catch (const std::bad_alloc&) { return write_oom(); }
    catch (const std::exception& e) { return write_error(Errc::io_error, "am.zip.io_error", e.what()); }
  }
  Result<void> flush() override {
    try {
      std::lock_guard lock(mutex);
      if (!opened()) return invalid_state();
#ifdef _WIN32
      if (!FlushFileBuffers(handle)) return os_error(GetLastError(), "FlushFileBuffers");
#else
      if (::fsync(handle) != 0) return os_error(errno, "fsync");
#endif
      return {};
    } catch (const std::bad_alloc&) { return write_oom(); }
    catch (const std::exception& e) { return write_error(Errc::io_error, "am.zip.io_error", e.what()); }
  }
  Result<void> close() override {
    if (!opened()) return {};
#ifdef _WIN32
    const auto current = handle; handle = INVALID_HANDLE_VALUE;
    if (!CloseHandle(current)) return os_error(GetLastError(), "CloseHandle");
#else
    const auto current = handle; handle = -1;
    if (::close(current) != 0) return os_error(errno, "close");
#endif
    return {};
  }
  Result<void> discard() override {
    try {
      if (discarded) return {};
      auto closed = close(); if (!closed) return closed;
#ifdef _WIN32
      if (!DeleteFileW(path.c_str())) return os_error(GetLastError(), "DeleteFileW");
#else
      if (::unlink(path.c_str()) != 0) return os_error(errno, "unlink");
#endif
      discarded = true; length = 0; return {};
    } catch (const std::bad_alloc&) { return write_oom(); }
    catch (const std::exception& e) { return write_error(Errc::io_error, "am.zip.io_error", e.what()); }
  }
};
}
Result<std::shared_ptr<RandomAccessSink>> file_sink(const std::filesystem::path& path) {
  try {
    auto sink = std::make_shared<FileSink>(); sink->path = paths::native(std::filesystem::absolute(path));
#ifdef _WIN32
    sink->handle = CreateFileW(sink->path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!sink->opened()) return os_error(GetLastError(), "CreateFileW");
#else
    sink->handle = ::open(sink->path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (!sink->opened()) return os_error(errno, "open");
#endif
    return std::shared_ptr<RandomAccessSink>(std::move(sink));
  } catch (const std::bad_alloc&) { return write_oom(); }
  catch (const std::exception& e) { return write_error(Errc::io_error, "am.zip.io_error", e.what()); }
}
}  // namespace am::asset_zip
