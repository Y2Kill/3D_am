#include "am/storage/commit/protocol.hpp"
#include "am/storage/asset_zip/probe.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
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

namespace am::commit {
namespace {
using U64 = std::uint64_t;
using Path = std::filesystem::path;
Error error(Errc c, std::string_view id, std::string detail) {
  return Error::make(c, "ZIP commit operation failed", std::move(detail)).in("storage.commit").op("zip.commit").id(std::string(id));
}
[[noreturn]] void fail(Errc c, std::string_view id, std::string detail) { throw error(c, id, std::move(detail)); }
Error oom() { return error(Errc::resource_exhausted, "am.commit.out_of_memory", "Allocation failed"); }
Error io(std::string detail) { return error(Errc::io_error, "am.commit.io_error", std::move(detail)); }
Error base_changed() { return error(Errc::conflict, "am.commit.base_changed", "Target no longer matches the expected file"); }
template<class T> T take(Result<T> result) { if (!result) throw result.error(); return std::move(result).value(); }
void take(Result<void> result) { if (!result) throw result.error(); }
void hit(const FaultHooks& hooks, FaultPoint point) { if (hooks.point) take(hooks.point(point)); }
bool uuid4(std::string_view id) { auto value = Uuid::parse_canonical(id); return value && value->version() == 4; }
bool component_equal(const Path& a, const Path& b) {
#ifdef _WIN32
  auto x = a.wstring(), y = b.wstring();
  return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE) == CSTR_EQUAL;
#else
  return a == b;
#endif
}
bool inside(const Path& root, const Path& p) {
  auto a = root.begin(), b = p.begin();
  for (; a != root.end(); ++a, ++b) if (b == p.end() || !component_equal(*a, *b)) return false;
  return b != p.end();
}
Path normalized(const Path& p) {
  auto result = std::filesystem::weakly_canonical(std::filesystem::absolute(p));
#ifdef _WIN32
  auto value = result.wstring();
  if (value.starts_with(L"\\\\?\\UNC\\")) result = Path(L"\\\\" + value.substr(8));
  else if (value.starts_with(L"\\\\?\\")) result = Path(value.substr(4));
#endif
  return result;
}
struct PathLess {
  bool operator()(const Path& a, const Path& b) const {
#ifdef _WIN32
    const auto& x = a.native(); const auto& y = b.native();
    return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE) == CSTR_LESS_THAN;
#else
    return a < b;
#endif
  }
};
struct Layout { Path root, target, temp_dir, temp; };
Layout layout(const Path& root, const Path& target, std::string_view asset, std::string_view op) {
  if (!uuid4(asset) || !uuid4(op)) fail(Errc::invalid_argument, "am.commit.invalid_path", "AssetID and OpID must be canonical UUIDv4");
  Layout paths; paths.root = normalized(root); paths.target = normalized(target.is_absolute() ? target : paths.root / target);
  paths.temp_dir = normalized(paths.root / ".am/.am-tmp");
  if (!inside(paths.root, paths.target) || !inside(paths.root, paths.temp_dir) || paths.target == paths.temp_dir)
    fail(Errc::invalid_argument, "am.commit.invalid_path", "Target or temporary directory is outside library");
  paths.temp = paths.temp_dir / (std::string(asset) + "." + std::string(op) + ".zip");
  if (component_equal(paths.temp, paths.target)) fail(Errc::invalid_argument, "am.commit.invalid_path", "Target equals temporary file");
  std::filesystem::create_directories(am::paths::native(paths.temp_dir));
  std::filesystem::create_directories(am::paths::native(paths.target.parent_path()));
#ifdef _WIN32
  auto volume = [](const Path& path) {
    std::array<wchar_t, 32768> out{};
    if (!GetVolumePathNameW(am::paths::native(path).c_str(), out.data(), static_cast<DWORD>(out.size()))) throw io("GetVolumePathNameW error=" + std::to_string(GetLastError()));
    std::array<wchar_t, 32768> name{};
    if (!GetVolumeNameForVolumeMountPointW(out.data(), name.data(), static_cast<DWORD>(name.size()))) throw io("GetVolumeNameForVolumeMountPointW error=" + std::to_string(GetLastError()));
    return std::wstring(name.data());
  };
  if (volume(paths.target.parent_path()) != volume(paths.temp_dir)) fail(Errc::invalid_argument, "am.commit.invalid_path", "Target and temporary directory are on different volumes");
#else
  struct stat a{}, b{};
  if (::stat(paths.target.parent_path().c_str(), &a) || ::stat(paths.temp_dir.c_str(), &b)) throw io("stat volume failed");
  if (a.st_dev != b.st_dev) fail(Errc::invalid_argument, "am.commit.invalid_path", "Target and temporary directory are on different volumes");
#endif
  return paths;
}
bool file_exists(const Path& p) { return std::filesystem::exists(am::paths::native(p)); }
void remove_file(const Path& p) { std::filesystem::remove(am::paths::native(p)); }
std::string hex(const asset_zip::Sha256Digest& value) {
  constexpr char digits[] = "0123456789abcdef"; std::string out;
  for (auto byte : value) { auto v = std::to_integer<unsigned>(byte); out += digits[v >> 4]; out += digits[v & 15]; } return out;
}
class NativeFile final : public asset_zip::RandomAccessSource {
 public:
  Path path;
  U64 length = 0;
  std::int64_t mtime_ns = 0;
  mutable std::mutex mutex;
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~NativeFile() override { close(); }
  void close() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; } }
#else
  int handle = -1;
  ~NativeFile() override { close(); }
  void close() { if (handle != -1) { ::close(handle); handle = -1; } }
#endif
  static std::shared_ptr<NativeFile> open(const Path& path, bool deletion = false, bool exclusive = false) {
    auto file = std::make_shared<NativeFile>(); file->path = path;
#ifdef _WIN32
    file->handle = CreateFileW(am::paths::native(path).c_str(), GENERIC_READ | (deletion ? DELETE : 0), exclusive ? 0 : FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file->handle == INVALID_HANDLE_VALUE) {
      auto code = GetLastError();
      throw error(code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? Errc::not_found : code == ERROR_SHARING_VIOLATION ? Errc::locked : Errc::io_error,
          "am.commit.io_error", "CreateFileW error=" + std::to_string(code));
    }
    LARGE_INTEGER size{}; FILE_BASIC_INFO basic{};
    if (!GetFileSizeEx(file->handle, &size) || size.QuadPart < 0 || !GetFileInformationByHandleEx(file->handle, FileBasicInfo, &basic, sizeof(basic))) throw io("File information query failed");
    file->length = static_cast<U64>(size.QuadPart);
    constexpr auto epoch = 116444736000000000LL;
    const auto ticks = basic.LastWriteTime.QuadPart - epoch;
    if (ticks > INT64_MAX / 100 || ticks < INT64_MIN / 100) throw io("File timestamp exceeds nanosecond representation");
    file->mtime_ns = ticks * 100;
#else
    (void)deletion;
    file->handle = ::open(path.c_str(), O_RDONLY);
    if (file->handle < 0) throw error(errno == ENOENT ? Errc::not_found : Errc::io_error, "am.commit.io_error", "open error=" + std::to_string(errno));
    if (exclusive && ::flock(file->handle, LOCK_EX | LOCK_NB)) throw error(Errc::locked, "am.commit.io_error", "File is busy");
    struct stat info{}; if (::fstat(file->handle, &info)) throw io("fstat failed"); file->length = static_cast<U64>(info.st_size);
    file->mtime_ns = static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
#endif
    return file;
  }
  U64 size() const noexcept override { return length; }
  Result<std::size_t> read_at(U64 offset, std::span<std::byte> bytes) const override {
    try {
      std::lock_guard lock(mutex); if (offset >= length || bytes.empty()) return std::size_t{0};
      const auto n = static_cast<std::size_t>(std::min<U64>({length - offset, bytes.size(), 0xffffffffULL}));
#ifdef _WIN32
      LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
      if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) throw io("SetFilePointerEx failed");
      DWORD count = 0; if (!ReadFile(handle, bytes.data(), static_cast<DWORD>(n), &count, nullptr)) throw io("ReadFile failed"); return static_cast<std::size_t>(count);
#else
      const auto count = ::pread(handle, bytes.data(), n, static_cast<off_t>(offset)); if (count < 0) throw io("pread failed"); return static_cast<std::size_t>(count);
#endif
    } catch (const Error& e) { return e; } catch (const std::bad_alloc&) { return oom(); } catch (const std::exception& e) { return io(e.what()); }
  }
};
std::string file_hash(const std::shared_ptr<NativeFile>& source) {
  auto hash = take(asset_zip::Sha256::create()); std::array<std::byte, 65536> buffer{};
  for (U64 offset = 0; offset < source->size();) {
    auto n = take(source->read_at(offset, buffer)); if (!n) throw io("Unexpected EOF while hashing file");
    take(hash.update(std::span(buffer).first(n))); offset += n;
  }
  return hex(take(hash.finish()));
}
std::string manifest_hash(const std::shared_ptr<NativeFile>& source, asset_zip::ReadBudget& budget, const asset_zip::ZipLimits& limits) {
  auto archive = take(asset_zip::ZipArchive::open(source, limits));
  auto item = std::find_if(archive.entries().begin(), archive.entries().end(), [](const auto& e) { return e.raw_name == "manifest.json"; });
  if (item == archive.entries().end()) fail(Errc::corrupt_data, "am.commit.verification_failed", "Root manifest.json is missing");
  return hex(*take(archive.verify_entry(item->index, budget, true)).sha256);
}
bool matches_base(const Path& target, const std::optional<db::BaseSnapshot>& base, asset_zip::ReadBudget& budget, const asset_zip::ZipLimits& limits) {
  if (!base) return !file_exists(target);
  if (!file_exists(target)) return false;
  auto source = NativeFile::open(target);
  if (source->size() != base->size || source->mtime_ns != base->mtime_ns) return false;
  return manifest_hash(source, budget, limits) == base->manifest_sha256;
}
bool identical(const std::shared_ptr<NativeFile>& source, const db::NewFileEvidence& expected) {
  return source->size() == expected.size && file_hash(source) == expected.file_sha256;
}
Replacement replace_file(const std::shared_ptr<NativeFile>& source, const Path& target, const db::NewFileEvidence& expected, const FaultHooks& hooks, bool& moved) {
#ifdef _WIN32
  DWORD code = hooks.rename_ex_error.value_or(0);
  if (!code) {
    const auto name = am::paths::native(target).wstring(); const auto bytes = name.size() * sizeof(wchar_t);
    // Win32's DOS-path conversion also reads the terminator, independently of FileNameLength.
    std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data()); rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->RootDirectory = nullptr; rename->FileNameLength = static_cast<DWORD>(bytes); std::memcpy(rename->FileName, name.data(), bytes);
    if (SetFileInformationByHandle(source->handle, FileRenameInfoEx, rename, static_cast<DWORD>(storage.size()))) { moved = true; return Replacement::posix_rename; }
    code = GetLastError();
  }
  if (code != ERROR_INVALID_PARAMETER && code != ERROR_NOT_SUPPORTED && code != ERROR_INVALID_FUNCTION)
    fail(Errc::io_error, "am.commit.replace_failed", "POSIX rename OS error=" + std::to_string(code));
  source->close(); if (hooks.before_fallback) take(hooks.before_fallback(source->path));
  if (!MoveFileExW(am::paths::native(source->path).c_str(), am::paths::native(target).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const auto next = GetLastError();
    fail(next == ERROR_INVALID_PARAMETER || next == ERROR_NOT_SUPPORTED || next == ERROR_INVALID_FUNCTION ? Errc::unsupported : Errc::io_error,
         next == ERROR_INVALID_PARAMETER || next == ERROR_NOT_SUPPORTED || next == ERROR_INVALID_FUNCTION ? "am.commit.replace_unsupported" : "am.commit.replace_failed", "MoveFileExW OS error=" + std::to_string(next));
  }
  moved = true; auto result = NativeFile::open(target);
  if (!identical(result, expected)) fail(Errc::io_error, "am.commit.replace_failed", "Moved file differs from verified SHA-256");
  return Replacement::move_file_ex;
#else
  (void)hooks;
  // DrvFS cannot resolve the new name while an old-name descriptor remains open.
  // POSIX rename uses a pathname, so check the published identity after closing it.
  source->close();
  if (::rename(source->path.c_str(), target.c_str())) fail(Errc::io_error, "am.commit.replace_failed", "rename failed");
  moved = true; const auto directory = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (directory < 0) fail(Errc::io_error, "am.commit.replace_failed", "open directory failed");
  const auto status = ::fsync(directory); ::close(directory);
  if (status) fail(Errc::io_error, "am.commit.replace_failed", "fsync directory failed");
  if (!identical(NativeFile::open(target), expected))
    fail(Errc::io_error, "am.commit.replace_failed", "Renamed file differs from verified SHA-256");
  return Replacement::rename;
#endif
}
std::shared_ptr<db::CommitJournal> journal_for(const Path& root, std::shared_ptr<db::CommitJournal> value) { return value ? value : take(db::StateJournal::open(root)); }
U64 free_space(const Path& directory) {
#ifdef _WIN32
  ULARGE_INTEGER free{}; if (!GetDiskFreeSpaceExW(am::paths::native(directory).c_str(), &free, nullptr, nullptr)) throw io("GetDiskFreeSpaceExW failed"); return free.QuadPart;
#else
  return std::filesystem::space(directory).available;
#endif
}
class HookSink final : public asset_zip::RandomAccessSink {
 public:
  std::shared_ptr<asset_zip::RandomAccessSink> inner;
  const FaultHooks* hooks = nullptr;
  U64 high = 0, threshold = 0;
  bool producer_active = true, fired = false;
  Result<void> write_at(U64 offset, std::span<const std::byte> bytes) override {
    auto result = inner->write_at(offset, bytes); if (!result) return result;
    high = std::max(high, offset + bytes.size());
    if (producer_active && !fired && high >= threshold) { fired = true; hit(*hooks, FaultPoint::mid_write); }
    return {};
  }
  Result<void> flush() override { return inner->flush(); }
  Result<void> close() override { return inner->close(); }
  Result<void> discard() override { return inner->discard(); }
  U64 size() const noexcept override { return inner->size(); }
};
Error verification_error(const Error& cause) { return Error::make(Errc::corrupt_data, "New ZIP verification failed", cause.ctx.detail).in("storage.commit").id("am.commit.verification_failed").caused_by(cause); }
}
std::string_view point_name(FaultPoint p) noexcept {
  switch (p) {
    case FaultPoint::after_lock: return "after_lock"; case FaultPoint::after_prepared: return "after_prepared"; case FaultPoint::mid_write: return "mid_write";
    case FaultPoint::after_finish: return "after_finish"; case FaultPoint::after_verify: return "after_verify"; case FaultPoint::after_verified_journal: return "after_verified_journal";
    case FaultPoint::before_replace: return "before_replace"; case FaultPoint::after_replace: return "after_replace"; case FaultPoint::after_committed_journal: return "after_committed_journal";
    case FaultPoint::after_indexed_journal: return "after_indexed_journal";
  } return "";
}
std::string_view replacement_name(Replacement v) noexcept { switch (v) { case Replacement::posix_rename: return "posix_rename"; case Replacement::move_file_ex: return "move_file_ex"; case Replacement::rename: return "rename"; } return ""; }
std::string_view outcome_name(RecoveryOutcome v) noexcept {
  switch (v) { case RecoveryOutcome::rolled_back: return "rolled_back"; case RecoveryOutcome::completed: return "completed"; case RecoveryOutcome::already_committed: return "already_committed";
    case RecoveryOutcome::cleaned: return "cleaned"; case RecoveryOutcome::skipped_locked: return "skipped_locked"; case RecoveryOutcome::skipped: return "skipped"; case RecoveryOutcome::failed: return "failed"; } return "";
}
struct AssetLock::Impl {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~Impl() { if (handle != INVALID_HANDLE_VALUE) { OVERLAPPED overlap{}; UnlockFileEx(handle, 0, 1, 0, &overlap); CloseHandle(handle); } }
#else
  int handle = -1;
  ~Impl() { if (handle >= 0) { ::flock(handle, LOCK_UN); ::close(handle); } }
#endif
};
AssetLock::AssetLock(std::unique_ptr<Impl> value) : impl_(std::move(value)) {}
AssetLock::AssetLock(AssetLock&&) noexcept = default;
AssetLock& AssetLock::operator=(AssetLock&&) noexcept = default;
AssetLock::~AssetLock() = default;
Result<AssetLock> AssetLock::acquire(const Path& root, std::string_view id, std::chrono::milliseconds timeout) {
  try {
    if (!uuid4(id)) return error(Errc::invalid_argument, "am.commit.invalid_path", "AssetID must be canonical UUIDv4");
    const auto canonical_root = normalized(root), directory = normalized(canonical_root / ".am/locks");
    if (!inside(canonical_root, directory)) return error(Errc::invalid_argument, "am.commit.invalid_path", "Lock directory is outside library");
    std::filesystem::create_directories(am::paths::native(directory)); const auto file = am::paths::native(directory / (std::string(id) + ".lock")); auto impl = std::make_unique<Impl>();
#ifdef _WIN32
    impl->handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl->handle == INVALID_HANDLE_VALUE) throw io("Create lock file failed");
#else
    impl->handle = ::open(file.c_str(), O_CREAT | O_RDWR, 0666); if (impl->handle < 0) throw io("Open lock file failed");
#endif
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
#ifdef _WIN32
      OVERLAPPED overlap{};
      if (LockFileEx(impl->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) return AssetLock(std::move(impl));
      if (GetLastError() != ERROR_LOCK_VIOLATION) throw io("LockFileEx failed");
#else
      if (::flock(impl->handle, LOCK_EX | LOCK_NB) == 0) return AssetLock(std::move(impl));
      if (errno != EWOULDBLOCK && errno != EAGAIN) throw io("flock failed");
#endif
      if (std::chrono::steady_clock::now() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Polling granularity, not a policy timeout.
    } while (true);
    return error(Errc::locked, "am.commit.locked", "Asset lock is busy");
  } catch (const Error& e) { return e; } catch (const std::bad_alloc&) { return oom(); } catch (const std::exception& e) { return io(e.what()); }
}
Result<db::BaseSnapshot> inspect_zip(const Path& path, asset_zip::ReadBudget& budget, const asset_zip::ZipLimits& limits) {
  try { auto source = NativeFile::open(normalized(path)); return db::BaseSnapshot{source->size(), source->mtime_ns, manifest_hash(source, budget, limits), {}}; }
  catch (const Error& e) { return e; } catch (const std::bad_alloc&) { return oom(); } catch (const std::exception& e) { return io(e.what()); }
}
Result<db::BaseSnapshot> inspect_copy_source(const Path& path) {
  try {
    auto source = NativeFile::open(normalized(path));
    return db::BaseSnapshot{source->size(), source->mtime_ns, {}, {}};
  } catch (const Error& e) {
    return e;
  } catch (const std::bad_alloc&) {
    return oom();
  } catch (const std::exception& e) {
    return io(e.what());
  }
}
Result<CommitResult> commit_zip(const CommitRequest& r) {
  std::shared_ptr<db::CommitJournal> journal; db::PendingOperation row; bool inserted = false, moved = false; std::shared_ptr<NativeFile> protected_file;
  // This lock deliberately outlives cleanup and every journal transition.
  std::optional<AssetLock> lock;
  auto cleanup = [&] { protected_file.reset(); if (inserted && !moved) { if (file_exists(row.temp_path)) remove_file(row.temp_path); take(journal->remove(row.op_id)); } };
  try {
    row.op_id = r.op_id.empty() ? Uuid::v4().to_string() : r.op_id; row.asset_id = r.asset_id; row.kind = r.kind; row.base = r.base; row.expected_rev = r.expected_rev; row.job_id = r.job_id;
    lock.emplace(take(AssetLock::acquire(r.library_root, r.asset_id, r.lock_timeout))); hit(r.hooks, FaultPoint::after_lock);
    const auto paths = layout(r.library_root, r.target_path, r.asset_id, row.op_id); row.target_path = paths.target; row.temp_path = paths.temp;
    if (!r.producer && !r.copy_source) throw io("Producer is missing");
    if (r.producer && r.copy_source)
      fail(Errc::invalid_argument, "am.repo.invalid_operation", "Exactly one commit content source is required");
    if (r.copy_source && r.copy_source->prepare) row.base = take(r.copy_source->prepare());
    if (!r.copy_source && !matches_base(paths.target, r.base, r.budget, r.zip_limits)) throw base_changed();
    const auto available = r.hooks.free_space ? take(r.hooks.free_space(paths.temp_dir)) : free_space(paths.temp_dir);
    const auto margin = std::max<U64>(r.estimated_size / 20 + (r.estimated_size % 20 != 0 ? 1 : 0), 256ULL * 1024 * 1024); // hypothesis, O-17
    if (r.estimated_size > UINT64_MAX - margin || available < r.estimated_size + margin) fail(Errc::insufficient_space, "am.commit.insufficient_space", "Free space is below estimate plus hypothesis reserve");
    journal = journal_for(paths.root, r.journal); take(journal->insert(row)); inserted = true; hit(r.hooks, FaultPoint::after_prepared);
    auto sink = r.hooks.create_sink ? take(r.hooks.create_sink(paths.temp)) : take(asset_zip::file_sink(paths.temp));
    auto hooked = std::make_shared<HookSink>(); hooked->inner = sink; hooked->hooks = &r.hooks; hooked->threshold = r.hooks.mid_write_after_bytes.value_or(std::max<U64>(1, r.estimated_size / 2));
    asset_zip::WriteSummary summary;
    std::optional<std::string> copied_hash;
    if (r.copy_source) {
      auto source = NativeFile::open(normalized(r.copy_source->path));
      const auto initial_size = source->size();
      const auto initial_mtime = source->mtime_ns;
      auto changed_source = [] {
        return error(Errc::conflict, "am.repo.import_source_changed", "Import source changed while copying");
      };
      if (r.copy_source->expected &&
          (initial_size != r.copy_source->expected->size || initial_mtime != r.copy_source->expected->mtime_ns))
        throw changed_source();
      if (initial_size > r.capabilities.max_file_size)
        fail(Errc::insufficient_space, "am.repo.file_too_large", "Import exceeds destination file size limit");
      auto hash = take(asset_zip::Sha256::create());
      std::array<std::byte, 65536> buffer{};
      for (U64 offset = 0; offset < initial_size;) {
        const auto n = take(source->read_at(offset, buffer));
        if (!n) throw changed_source();
        const auto block = std::span(buffer).first(n);
        take(hash.update(block));
        take(hooked->write_at(offset, block));
        offset += n;
      }
      copied_hash = hex(take(hash.finish()));
      summary.file_size = initial_size;
      summary.zip_limits = r.zip_limits;
      hooked->producer_active = false;
      take(hooked->flush());
      take(hooked->close());
      source->close();
      hit(r.hooks, FaultPoint::after_finish);
      auto after = inspect_copy_source(r.copy_source->path);
      if (!after || after->size != initial_size || after->mtime_ns != initial_mtime) throw changed_source();
    } else {
      asset_zip::WriterOptions writer_options;
      writer_options.zip_limits = r.zip_limits;
      writer_options.limits.max_file_size = r.capabilities.max_file_size;
      auto writer = take(asset_zip::ZipWriter::create(hooked, writer_options));
      take(r.producer(writer));
      hooked->producer_active = false;
      summary = take(writer.finish());
      hit(r.hooks, FaultPoint::after_finish);
    }
    protected_file = NativeFile::open(paths.temp, true);
    const auto hash_start = std::chrono::steady_clock::now(); const auto digest = file_hash(protected_file);
    const auto hash_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - hash_start).count();
    std::string manifest;
    try {
      if (r.copy_source) {
        if (protected_file->size() != summary.file_size || !copied_hash || digest != *copied_hash)
          fail(Errc::corrupt_data, "am.repo.import_invalid", "Temporary copy differs from copied bytes");
      } else
        take(asset_zip::verify_written(protected_file, summary, r.budget));
      auto probe = take(asset_zip::probe_container(protected_file, r.zip_limits));
      if (probe.outcome != asset_zip::ProbeOutcome::own) { if (probe.error) throw *probe.error; throw error(Errc::corrupt_data, "am.commit.verification_failed", "Container probe is not own"); }
      auto archive = take(asset_zip::ZipArchive::open(protected_file, r.zip_limits)); if (r.verifier) take(r.verifier(archive));
      manifest = manifest_hash(protected_file, r.budget, r.zip_limits);
    } catch (const Error& e) { throw verification_error(e); }
    catch (const std::bad_alloc&) { throw verification_error(oom()); }
    catch (const std::exception& e) { throw verification_error(io(e.what())); }
    catch (...) { throw verification_error(io("Unknown verifier exception")); }
    hit(r.hooks, FaultPoint::after_verify);
    db::NewFileEvidence evidence{protected_file->size(), digest, manifest};
    if (r.copy_source && r.copy_source->publish && !take(r.copy_source->publish(evidence))) {
      cleanup();
      inserted = false;
      CommitResult result;
      result.no_op = true;
      return result;
    }
    take(journal->advance(row.op_id, db::CommitState::prepared, db::CommitState::verified, evidence)); row.state = db::CommitState::verified; row.verified = evidence;
    hit(r.hooks, FaultPoint::after_verified_journal);
    if (!matches_base(paths.target, row.base, r.budget, r.zip_limits)) throw base_changed();
    hit(r.hooks, FaultPoint::before_replace);
    if (r.copy_source && !matches_base(paths.target, row.base, r.budget, r.zip_limits)) throw base_changed();
    const auto method = replace_file(protected_file, paths.target, evidence, r.hooks, moved); hit(r.hooks, FaultPoint::after_replace);
    take(journal->advance(row.op_id, db::CommitState::verified, db::CommitState::committed)); row.state = db::CommitState::committed; hit(r.hooks, FaultPoint::after_committed_journal);
    if (r.on_committed) take(r.on_committed(row));
    take(journal->advance(row.op_id, db::CommitState::committed, db::CommitState::indexed)); hit(r.hooks, FaultPoint::after_indexed_journal); take(journal->remove(row.op_id));
    auto actual = NativeFile::open(paths.target);
    return CommitResult{row.op_id, evidence.size, manifest, actual->mtime_ns, method, std::move(summary), hash_seconds};
  } catch (const Error& e) { try { cleanup(); } catch (...) { } return e; }
  catch (const std::bad_alloc&) { try { cleanup(); } catch (...) { } return oom(); }
  catch (const std::exception& e) { try { cleanup(); } catch (...) { } return io(e.what()); }
  catch (...) { try { cleanup(); } catch (...) { } return io("Unknown commit callback exception"); }
}
namespace {
bool lifecycle_kind(std::string_view kind) { return kind == "trash" || kind == "restore" || kind == "purge"; }
struct LifecyclePaths {
  Path source, target, asset;
};
LifecyclePaths lifecycle_paths(const Path& library, std::string_view asset, std::string_view kind) {
  const auto id = Uuid::parse_canonical(asset);
  if (!id || id->version() != 4 || (id->bytes()[8] & 0xc0) != 0x80 || !lifecycle_kind(kind))
    fail(Errc::invalid_argument, "am.commit.invalid_path", "Invalid lifecycle operation or AssetID");
  const auto root = normalized(library);
  const auto active = root / "assets" / std::string(asset.substr(34)) / (std::string(asset) + ".zip");
  const auto trash = root / "trash" / (std::string(asset) + ".zip");
  LifecyclePaths out{kind == "trash" ? active : trash, kind == "restore" ? active : trash, active};
  for (const auto& p : {out.source, out.target})
    if (!inside(root, p) || !component_equal(normalized(p.parent_path()), p.parent_path()))
      fail(Errc::invalid_argument, "am.commit.invalid_path", "Lifecycle parent is outside the library or is a symlink");
  return out;
}
bool location_exists(const Path& p) {
  return std::filesystem::symlink_status(paths::native(p)).type() != std::filesystem::file_type::not_found;
}
db::BaseSnapshot location_stat(const Path& p) {
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(paths::native(p))))
    fail(Errc::not_found, "am.repo.asset_not_found", "Source is not a regular asset file");
  db::BaseSnapshot out;
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(paths::native(p).c_str(), GetFileExInfoStandard, &data)) throw io("Lifecycle stat failed");
  out.size = (U64(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  const auto ticks = (U64(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
  out.mtime_ns = (static_cast<std::int64_t>(ticks) - 116444736000000000LL) * 100;
#else
  struct stat data{};
  if (::lstat(p.c_str(), &data) || !S_ISREG(data.st_mode)) throw io("Lifecycle stat failed");
  out.size = static_cast<U64>(data.st_size);
  out.mtime_ns = static_cast<std::int64_t>(data.st_mtim.tv_sec) * 1000000000LL + data.st_mtim.tv_nsec;
#endif
  return out;
}
void lifecycle_conflict(std::string_view kind) {
  fail(Errc::conflict, kind == "trash" ? "am.repo.trash_conflict" : "am.repo.restore_conflict",
       "Destination already exists");
}
void move_no_replace(const LifecyclePaths& p, std::string_view kind, bool& changed) {
#ifdef _WIN32
  if (!MoveFileExW(paths::native(p.source).c_str(), paths::native(p.target).c_str(), MOVEFILE_WRITE_THROUGH)) {
    const auto code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) lifecycle_conflict(kind);
    throw io("MoveFileExW without replacement error=" + std::to_string(code));
  }
  changed = true;
#else
#ifdef SYS_renameat2
  if (::syscall(SYS_renameat2, AT_FDCWD, p.source.c_str(), AT_FDCWD, p.target.c_str(), 1U) == 0) {
    changed = true;
    return;
  }
  if (errno == EEXIST) lifecycle_conflict(kind);
  if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) throw io("renameat2 without replacement failed");
#endif
  if (::link(p.source.c_str(), p.target.c_str())) {
    if (errno == EEXIST) lifecycle_conflict(kind);
    throw io("link without replacement failed");
  }
  changed = true;  // A crash before unlink is the explicit two-files recovery case.
  if (::unlink(p.source.c_str())) throw io("unlink after link failed");
#endif
}
RecoveryItem recover_lifecycle(const Path& root, db::PendingOperation row, db::CommitJournal& journal,
                               const RecoveryOptions& options) {
  RecoveryItem item;
  item.op_id = row.op_id;
  item.path = row.target_path;
  const auto p = lifecycle_paths(root, row.asset_id, row.kind);
  auto normalized_target = row.target_path.lexically_normal();
  normalized_target.make_preferred();
  auto source_path = row.temp_path.lexically_normal();
  source_path.make_preferred();
  if (!component_equal(normalized_target, p.target) ||
      (row.kind == "purge" ? !row.temp_path.empty() : !component_equal(source_path, p.source)))
    fail(Errc::invalid_argument, "am.commit.invalid_path",
         "Lifecycle journal paths do not match AssetID and operation");
  if (row.state == db::CommitState::failed) {
    item.outcome = RecoveryOutcome::failed;
    item.detail = "Lifecycle operation requires manual examination";
    return item;
  }
  const bool source = location_exists(p.source);
  const bool target = location_exists(p.target);
  if ((row.kind == "purge" && target) || (row.kind != "purge" && source && !target)) {
    take(journal.remove(row.op_id));
    item.outcome = RecoveryOutcome::rolled_back;
    return item;
  }
  if (row.kind != "purge" && source == target) {
    take(journal.advance(row.op_id, row.state, db::CommitState::failed));
    item.outcome = RecoveryOutcome::failed;
    item.error = base_changed();
    item.detail = source ? "Both lifecycle paths exist; files left untouched"
                         : "Neither lifecycle path exists; files left untouched";
    return item;
  }
  if (row.state == db::CommitState::prepared) {
    take(journal.advance(row.op_id, row.state, db::CommitState::committed));
    row.state = db::CommitState::committed;
  }
  // Notifications may repeat after a crash. Index updates/removals are idempotent.
  if (options.on_committed) take(options.on_committed(row));
  if (row.state == db::CommitState::committed)
    take(journal.advance(row.op_id, row.state, db::CommitState::indexed));
  else if (row.state != db::CommitState::indexed)
    throw io("Unexpected lifecycle journal state");
  take(journal.remove(row.op_id));
  item.outcome = RecoveryOutcome::completed;
  return item;
}
}  // namespace
Result<std::string> commit_lifecycle(const LifecycleRequest& r) {
  auto journal = r.journal;
  db::PendingOperation row;
  bool inserted = false, changed = false;
  std::optional<AssetLock> lock;
  auto cleanup = [&] {
    if (inserted && !changed) take(journal->remove(row.op_id));
  };
  try {
    lock.emplace(take(AssetLock::acquire(r.library_root, r.asset_id, r.lock_timeout)));
    hit(r.hooks, FaultPoint::after_lock);
    const auto p = lifecycle_paths(r.library_root, r.asset_id, r.kind);
    if (!location_exists(p.source)) {
      if (r.kind == "purge" && location_exists(p.asset))
        fail(Errc::not_found, "am.repo.not_in_trash", "Asset is outside trash");
      fail(Errc::not_found, "am.repo.asset_not_found", "Source asset does not exist");
    }
    if (r.kind != "purge" && location_exists(p.target)) lifecycle_conflict(r.kind);
    row.op_id = Uuid::v4().to_string();
    row.asset_id = r.asset_id;
    row.kind = r.kind;
    row.target_path = p.target;
    if (r.kind != "purge") row.temp_path = p.source;
    row.base = location_stat(p.source);
    if (r.kind == "restore") std::filesystem::create_directories(paths::native(p.target.parent_path()));
    journal = journal_for(normalized(r.library_root), journal);
    take(journal->insert(row));
    inserted = true;
    hit(r.hooks, FaultPoint::after_prepared);
    hit(r.hooks, FaultPoint::before_replace);
    db::BaseSnapshot actual;
    try {
      actual = location_stat(p.source);
    } catch (const Error& e) {
      if (e.code == Errc::not_found) throw base_changed();
      throw;
    }
    if (actual.size != row.base->size || actual.mtime_ns != row.base->mtime_ns) throw base_changed();
    if (r.kind == "purge") {
#ifdef _WIN32
      if (!DeleteFileW(paths::native(p.source).c_str())) throw io("DeleteFileW from trash failed");
#else
      if (::unlink(p.source.c_str())) throw io("unlink from trash failed");
#endif
      changed = true;
    } else
      move_no_replace(p, r.kind, changed);
    hit(r.hooks, FaultPoint::after_replace);
    take(journal->advance(row.op_id, db::CommitState::prepared, db::CommitState::committed));
    row.state = db::CommitState::committed;
    hit(r.hooks, FaultPoint::after_committed_journal);
    if (r.on_committed) take(r.on_committed(row));
    take(journal->advance(row.op_id, db::CommitState::committed, db::CommitState::indexed));
    hit(r.hooks, FaultPoint::after_indexed_journal);
    take(journal->remove(row.op_id));
    return row.op_id;
  } catch (const Error& e) {
    try {
      cleanup();
    } catch (...) {
    }
    return e;
  } catch (const std::bad_alloc&) {
    try {
      cleanup();
    } catch (...) {
    }
    return oom();
  } catch (const std::exception& e) {
    try {
      cleanup();
    } catch (...) {
    }
    return io(e.what());
  } catch (...) {
    try {
      cleanup();
    } catch (...) {
    }
    return io("Unknown lifecycle callback exception");
  }
}
Result<std::vector<RecoveryItem>> recover(const Path& root, const RecoveryOptions& options) {
  try {
    auto journal = journal_for(normalized(root), options.journal); auto rows = take(journal->list()); std::vector<RecoveryItem> report;
    for (auto row : rows) {
      RecoveryItem item; item.op_id = row.op_id; item.path = row.target_path;
      try {
        auto locked = AssetLock::acquire(root, row.asset_id, options.lock_timeout);
        if (!locked) { if (locked.error().code == Errc::locked) { item.outcome = RecoveryOutcome::skipped_locked; report.push_back(std::move(item)); continue; } throw locked.error(); }
        auto lock = std::move(locked).value();
        if (lifecycle_kind(row.kind)) {
          report.push_back(recover_lifecycle(root, row, *journal, options));
          continue;
        }
        const auto paths = layout(root, row.target_path, row.asset_id, row.op_id);
        if (!component_equal(normalized(row.temp_path), paths.temp)) fail(Errc::invalid_argument, "am.commit.invalid_path", "Journal temporary path does not match operation");
        auto rollback = [&] { if (file_exists(paths.temp)) remove_file(paths.temp); take(journal->remove(row.op_id)); item.outcome = RecoveryOutcome::rolled_back; };
        auto finish = [&](db::CommitState state) {
          if (state == db::CommitState::verified) { take(journal->advance(row.op_id, state, db::CommitState::committed)); state = db::CommitState::committed; }
          if (state == db::CommitState::committed) { row.state = state; if (options.on_committed) take(options.on_committed(row)); take(journal->advance(row.op_id, state, db::CommitState::indexed)); }
          take(journal->remove(row.op_id));
        };
        if (row.state == db::CommitState::prepared) rollback();
        else if (row.state == db::CommitState::indexed) { finish(row.state); item.outcome = RecoveryOutcome::cleaned; }
        else {
          if (!row.verified) throw io("Verified evidence is missing");
          if (row.state == db::CommitState::committed) {
            if (file_exists(paths.target) && identical(NativeFile::open(paths.target), *row.verified)) { finish(row.state); item.outcome = RecoveryOutcome::completed; }
            else { take(journal->remove(row.op_id)); item.outcome = RecoveryOutcome::failed; item.error = base_changed(); item.detail = "Committed target changed; target left untouched"; }
          } else if (file_exists(paths.temp)) {
            auto source = NativeFile::open(paths.temp, true); asset_zip::ReadBudget budget(UINT64_MAX);
            if (identical(source, *row.verified) && matches_base(paths.target, row.base, budget, options.zip_limits)) {
              bool moved = false; replace_file(source, paths.target, *row.verified, options.hooks, moved); finish(row.state); item.outcome = RecoveryOutcome::completed;
            } else { source.reset(); item.detail = "Temporary file identity or base differs; target left untouched"; rollback(); }
          } else if (file_exists(paths.target) && identical(NativeFile::open(paths.target), *row.verified)) { finish(row.state); item.outcome = RecoveryOutcome::already_committed; }
          else { item.detail = "Target is not the verified new file; target left untouched"; rollback(); }
        }
      } catch (const Error& e) { item.outcome = RecoveryOutcome::failed; item.error = e; }
      catch (const std::bad_alloc&) { item.outcome = RecoveryOutcome::failed; item.error = oom(); }
      catch (const std::exception& e) { item.outcome = RecoveryOutcome::failed; item.error = io(e.what()); }
      catch (...) { item.outcome = RecoveryOutcome::failed; item.error = io("Unknown recovery callback exception"); }
      report.push_back(std::move(item));
    }
    std::set<Path, PathLess> active;
    for (const auto& row : take(journal->list())) active.insert(normalized(row.temp_path));
    const auto directory = normalized(root / ".am/.am-tmp");
    if (!inside(normalized(root), directory)) fail(Errc::invalid_argument, "am.commit.invalid_path", "Temporary directory is outside library");
    if (file_exists(directory)) for (const auto& entry : std::filesystem::directory_iterator(am::paths::native(directory))) {
      if (entry.path().extension() != ".zip" || active.contains(normalized(entry.path()))) continue;
      RecoveryItem item; item.path = entry.path();
      try {
        auto file = NativeFile::open(entry.path(), true, true);
        const auto modified = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(file->mtime_ns)));
        if (options.now - modified < options.orphan_age) { item.outcome = RecoveryOutcome::skipped; item.detail = "Orphan is younger than retention age"; }
        else {
#ifdef _WIN32
          FILE_DISPOSITION_INFO disposition{TRUE}; if (!SetFileInformationByHandle(file->handle, FileDispositionInfo, &disposition, sizeof(disposition))) throw io("Delete orphan failed"); file->close();
#else
          remove_file(entry.path());
#endif
          item.outcome = RecoveryOutcome::cleaned;
        }
      } catch (const Error& e) { item.outcome = e.code == Errc::locked ? RecoveryOutcome::skipped : RecoveryOutcome::failed; item.error = e; }
      catch (const std::exception& e) { item.outcome = RecoveryOutcome::failed; item.error = io(e.what()); }
      report.push_back(std::move(item));
    }
    return report;
  } catch (const Error& e) { return e; } catch (const std::bad_alloc&) { return oom(); } catch (const std::exception& e) { return io(e.what()); }
  catch (...) { return io("Unknown recovery journal exception"); }
}
}  // namespace am::commit
