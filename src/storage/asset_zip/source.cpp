#include "am/storage/asset_zip/source.hpp"
#include "am/foundation/paths.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#ifdef _WIN32
#include <windows.h>
#endif
namespace am::asset_zip {
namespace {
Error error(std::string detail, bool oom = false) {
  return Error::make(oom ? Errc::resource_exhausted : Errc::io_error, "ZIP source access failed", std::move(detail))
      .in("storage.asset_zip").op("zip.source").id(oom ? "am.zip.out_of_memory" : "am.zip.io_error");
}
struct MemorySource final : RandomAccessSource {
  std::vector<std::byte> bytes;
  explicit MemorySource(std::vector<std::byte> data) : bytes(std::move(data)) {}
  std::uint64_t size() const noexcept override { return bytes.size(); }
  Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> out) const override {
    if (offset >= bytes.size()) return std::size_t{0};
    const auto n = std::min(out.size(), bytes.size() - static_cast<std::size_t>(offset));
    if (n) std::memcpy(out.data(), bytes.data() + static_cast<std::size_t>(offset), n);
    return n;
  }
};
struct FileSource final : RandomAccessSource {
  std::uint64_t length = 0;
  mutable std::mutex mutex;
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~FileSource() override { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
  mutable std::ifstream file;
#endif
  std::uint64_t size() const noexcept override { return length; }
  Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> out) const override {
    try {
      std::lock_guard lock(mutex);
      if (offset >= length || out.empty()) return std::size_t{0};
#ifdef _WIN32
      if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) return error("File offset is not representable");
      LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
      if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) return error("SetFilePointerEx error=" + std::to_string(GetLastError()));
      const auto count = static_cast<DWORD>(std::min<std::uint64_t>({out.size(), length - offset, MAXDWORD}));
      DWORD done = 0;
      if (!ReadFile(handle, out.data(), count, &done, nullptr)) return error("ReadFile error=" + std::to_string(GetLastError()));
      return static_cast<std::size_t>(done);
#else
      if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) return error("File offset is not representable");
      file.clear();
      file.seekg(static_cast<std::streamoff>(offset));
      if (!file) return error("Seek failed");
      const auto count = std::min<std::uint64_t>({out.size(), length - offset, static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())});
      file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count));
      if (file.bad()) return error("Read failed");
      return static_cast<std::size_t>(file.gcount());
#endif
    } catch (const std::bad_alloc&) { return error("Allocation failed", true); }
    catch (const std::exception& e) { return error(e.what()); }
  }
};
}
Result<std::shared_ptr<const RandomAccessSource>> memory_source(std::vector<std::byte> bytes) {
  try {
    std::shared_ptr<const RandomAccessSource> source = std::make_shared<MemorySource>(std::move(bytes));
    return source;
  } catch (const std::bad_alloc&) { return error("Allocation failed", true); }
}
Result<std::shared_ptr<const RandomAccessSource>> file_source(const std::filesystem::path& path) {
  return file_source(path, false);
}
Result<std::shared_ptr<const RandomAccessSource>> file_source(const std::filesystem::path& path, bool share_delete) {
  try {
    auto source = std::make_shared<FileSource>();
    const auto native = paths::native(std::filesystem::absolute(path));
#ifdef _WIN32
    source->handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | (share_delete ? FILE_SHARE_DELETE : 0), nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (source->handle == INVALID_HANDLE_VALUE) return error("CreateFileW error=" + std::to_string(GetLastError()));
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(source->handle, &size) || size.QuadPart < 0) return error("GetFileSizeEx failed");
    source->length = static_cast<std::uint64_t>(size.QuadPart);
#else
    (void)share_delete;
    source->file.open(native, std::ios::binary);
    if (!source->file) return error("Cannot open file");
    source->file.seekg(0, std::ios::end);
    const auto size = source->file.tellg();
    if (size < 0) return error("Cannot determine file size");
    source->length = static_cast<std::uint64_t>(size);
#endif
    std::shared_ptr<const RandomAccessSource> result = std::move(source);
    return result;
  } catch (const std::bad_alloc&) { return error("Allocation failed", true); }
  catch (const std::exception& e) { return error(e.what()); }
}
}  // namespace am::asset_zip
