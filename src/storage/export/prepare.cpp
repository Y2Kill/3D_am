#include "am/storage/export/prepare.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

#include "am/foundation/unicode.hpp"
#include "am/foundation/uuid.hpp"
#include "am/storage/fs/publish.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace am::storage::exporting {
namespace {
namespace az = asset_zip;
using Path = std::filesystem::path;
using U64 = std::uint64_t;
Error error(Errc code, std::string_view id, std::string detail = {}) {
  auto result = Error::make(code, "Asset export failed", std::move(detail)).in("storage.export").id(std::string(id));
  if (code == Errc::insufficient_space) result.actions = {ErrorAction::free_space,ErrorAction::abort};
  return result;
}
template <class T> T take(Result<T> result) { if (!result) throw result.error(); return std::move(*result); }
void take(Result<void> result) { if (!result) throw result.error(); }
template <class T, class F> Result<T> guard(F&& function) {
  try { return function(); }
  catch (const Error& exception) { return exception; }
  catch (const std::bad_alloc&) { return error(Errc::resource_exhausted, "am.export.out_of_memory"); }
  catch (const std::exception& exception) { return error(Errc::io_error, "am.export.io_error", exception.what()); }
  catch (...) { return error(Errc::internal,"am.export.io_error"); }
}
void check_stop(std::stop_token stop) {
  if (stop.stop_requested()) throw error(Errc::cancelled, "am.export.cancelled");
}
std::string hex(const az::Sha256Digest& digest) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : digest) {
    const auto value = std::to_integer<unsigned>(byte);
    result += digits[value >> 4]; result += digits[value & 15];
  }
  return result;
}
std::string hash(const az::RandomAccessSource& source, az::ReadBudget& budget, std::stop_token stop,
                 az::RandomAccessSink* sink = nullptr, const ExportHooks* hooks = nullptr) {
  auto digest = take(az::Sha256::create());
  std::array<std::byte, 65536> block{};
  for (U64 offset = 0; offset < source.size();) {
    check_stop(stop);
    const auto wanted = static_cast<std::size_t>(std::min<U64>(block.size(), source.size() - offset));
    const auto read = take(source.read_at(offset, std::span(block).first(wanted)));
    if (!read || read > wanted) throw error(Errc::io_error, "am.export.io_error", "Unexpected source EOF");
    if (!budget.consume(read)) throw error(Errc::resource_exhausted, "am.export.read_budget_exceeded");
    const auto bytes = std::span<const std::byte>(block).first(read);
    take(digest.update(bytes));
    if (sink) take(sink->write_at(offset, bytes));
    offset += read;
    if (hooks && hooks->on_read) hooks->on_read(read);
  }
  check_stop(stop);
  return hex(take(digest.finish()));
}
void valid(const repository::VerificationReport& report, std::string_view message = "am.export.source_invalid",
           bool allow_newer_minor = false) {
  if (report.error) {
    if (report.error->code == Errc::cancelled || report.error->code == Errc::resource_exhausted) throw *report.error;
    throw error(Errc::corrupt_data, message).caused_by(*report.error);
  }
  const bool compatible_minor = allow_newer_minor && report.format_support == repository::FormatSupport::newer_minor &&
                                report.v2 == repository::CheckStatus::not_applicable_newer_minor;
  if (report.v1 != repository::CheckStatus::passed ||
      (report.v2 != repository::CheckStatus::passed && !compatible_minor) ||
      report.v3 == repository::CheckStatus::failed ||
      std::any_of(report.v4.begin(), report.v4.end(), [](const auto& stream) {
        return stream.status == repository::StreamStatus::failed;
      }))
    throw error(Errc::corrupt_data, message);
}
U64 integer(const repository::Json& value) {
  const auto& text = value.get_ref<const std::string&>();
  U64 result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw error(Errc::corrupt_data, "am.export.source_invalid");
  return result;
}
class StoppableSource final : public az::RandomAccessSource {
 public:
  std::shared_ptr<const az::RandomAccessSource> source;
  std::stop_token stop;
  const ExportHooks* hooks;
  StoppableSource(std::shared_ptr<const az::RandomAccessSource> input, std::stop_token token, const ExportHooks* h)
      : source(std::move(input)), stop(token), hooks(h) {}
  U64 size() const noexcept override { return source->size(); }
  Result<std::size_t> read_at(U64 offset, std::span<std::byte> bytes) const override {
    return guard<std::size_t>([&] {
      check_stop(stop);
      const auto count = take(source->read_at(offset, bytes));
      if (hooks && hooks->on_read) hooks->on_read(count);
      return count;
    });
  }
};
struct TemporaryLease {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~TemporaryLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
  int handle = -1;
  ~TemporaryLease() { if (handle != -1) ::close(handle); }
#endif
};
std::shared_ptr<void> lease_temporary(const Path& temporary) {
  auto lease = std::make_shared<TemporaryLease>();
#ifdef _WIN32
  // Compatible with our writer and atomic publication, incompatible with the
  // exclusive DELETE handle used by orphan cleanup. Fault hooks may still write.
  lease->handle = CreateFileW(temporary.c_str(),GENERIC_READ,FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  if (lease->handle == INVALID_HANDLE_VALUE)
    throw error(Errc::io_error,"am.export.io_error","Temporary lease OS error="+std::to_string(GetLastError()));
#else
  lease->handle = ::open(temporary.c_str(),O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (lease->handle == -1 || ::flock(lease->handle,LOCK_EX | LOCK_NB) != 0)
    throw error(Errc::io_error,"am.export.io_error","Temporary lease OS error="+std::to_string(errno));
#endif
  return lease;
}
void remove_temporary(PreparedExport& prepared) noexcept {
  if (prepared.owned) {
    std::error_code ignored;
    std::filesystem::remove(prepared.temporary, ignored);
    prepared.owned = false;
  }
}
}  // namespace

PreparedExport::PreparedExport() = default;
PreparedExport::PreparedExport(PreparedExport&& other) noexcept
    : temporary(std::move(other.temporary)), sha256(std::move(other.sha256)), size(other.size), owned(other.owned),
      lease_(std::move(other.lease_)) {
  other.owned = false;
}
PreparedExport& PreparedExport::operator=(PreparedExport&& other) noexcept {
  if (this != &other) {
    remove_temporary(*this);
    temporary = std::move(other.temporary); sha256 = std::move(other.sha256); size = other.size; owned = other.owned;
    lease_ = std::move(other.lease_);
    other.owned = false;
  }
  return *this;
}
PreparedExport::~PreparedExport() { remove_temporary(*this); }

Result<void> validate_target(const Path& library, const Path& target) {
  return guard<void>([&]() -> Result<void> {
    const auto info = paths::node_info(target);
    if (!info || info->kind != paths::NodeKind::directory || info->cloud_placeholder)
      return error(Errc::invalid_argument, "am.export.target_unavailable");
    const auto normalized = std::filesystem::weakly_canonical(target);
    const auto base = std::filesystem::weakly_canonical(library);
    auto left = normalized.begin(), right = base.begin();
    for (; right != base.end() && left != normalized.end(); ++left, ++right) {
#ifdef _WIN32
      const auto a = left->generic_u8string(), b = right->generic_u8string();
      if (unicode::path_collision_key(std::string(a.begin(), a.end())) !=
          unicode::path_collision_key(std::string(b.begin(), b.end()))) break;
#else
      if (*left != *right) break;
#endif
    }
    if (right == base.end()) return error(Errc::invalid_argument, "am.export.target_inside_library");
    return {};
  });
}

Result<PreparedExport> prepare(const repository::AssetSnapshot& snapshot, const PrepareOptions& options) {
  return guard<PreparedExport>([&] {
    check_stop(options.stop);
    if (snapshot.in_trash()) throw error(Errc::not_found, "am.export.asset_in_trash");
    repository::ContainerVerifyOptions verification;
    verification.read_budget = options.read_budget;
    verification.level = options.full ? repository::VerifyLevel::full : repository::VerifyLevel::quick;
    verification.streams = options.full;
    az::ReadBudget budget(options.read_budget);
    {
      const auto source_report = take(repository::verify_snapshot(snapshot, verification, options.stop, &budget));
      valid(source_report, "am.export.source_invalid", options.full);
      if (options.hooks.on_container_verification) options.hooks.on_container_verification(false,source_report);
    }
    check_stop(options.stop);
    const auto source = snapshot.archive().source();
    U64 estimate = source->size();
    if (!options.full) {
      estimate = 128;
      if (options.files.empty()) throw error(Errc::invalid_argument, "am.export.selection_empty");
      for (const auto& file : options.files) {
        const auto& original = snapshot.manifest().known().at("files").at(file.file_index);
        const auto path = original.at("storage_path").get<std::string>();
        const auto entries = snapshot.archive().entries();
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.raw_name == path; });
        if (entry == entries.end()) throw error(Errc::corrupt_data, "am.export.source_invalid");
        const U64 overhead = 512 + 2 * static_cast<U64>(file.target_name.size());
        if (entry->compressed_size > (std::numeric_limits<U64>::max)() - overhead ||
            estimate > (std::numeric_limits<U64>::max)() - overhead - entry->compressed_size)
          throw error(Errc::insufficient_space, "am.export.insufficient_space");
        estimate += overhead + entry->compressed_size;
      }
    }
    take(storage::fs::cleanup_export_orphans(options.target_dir));
    const auto available = options.hooks.available_space ? options.hooks.available_space(options.target_dir)
                                                         : std::filesystem::space(paths::native(options.target_dir)).available;
    const auto capabilities = repository::detect_storage_capabilities(options.target_dir);
    const auto maximum = options.hooks.max_file_size ? options.hooks.max_file_size
        : capabilities ? capabilities->max_file_size : std::optional<U64>{};
    if (available < estimate || (maximum && estimate > *maximum))
      throw error(Errc::insufficient_space, "am.export.insufficient_space");
    PreparedExport prepared;
    prepared.temporary = paths::native(options.target_dir / (".am-export-" + Uuid::v4().to_string() + ".tmp"));
    auto sink = take(options.hooks.create_sink ? options.hooks.create_sink(prepared.temporary) : az::file_sink(prepared.temporary));
    prepared.owned = true;
    prepared.lease_ = lease_temporary(prepared.temporary);
    if (options.hooks.before_copy) options.hooks.before_copy();
    if (options.full) {
      prepared.size = source->size();
      prepared.sha256 = hash(*source, budget, options.stop, sink.get(), &options.hooks);
      take(sink->flush()); take(sink->close());
    } else {
      az::WriterOptions writer_options;
      writer_options.path_profile = paths::ContainerPathProfile::plain;
      writer_options.zip_limits.max_name_bytes = 65535;
      if (maximum) writer_options.limits.max_file_size = *maximum;
      auto writer = take(az::ZipWriter::create(sink, writer_options));
      const auto retained = std::make_shared<StoppableSource>(source, options.stop, &options.hooks);
      auto input = take(az::ZipArchive::open(retained));
      for (const auto& file : options.files) {
        check_stop(options.stop);
        const auto& original = snapshot.manifest().known().at("files").at(file.file_index);
        const auto path = original.at("storage_path").get<std::string>();
        const auto entries = input.entries();
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.raw_name == path; });
        if (entry == entries.end()) throw error(Errc::corrupt_data, "am.export.source_invalid");
        az::RawCopyOptions copy; copy.target_name = file.target_name; copy.drop_unlisted = true;
        auto copied = writer.copy_raw(input, entry->index, budget, copy);
        if (!copied && (copied.error().code == Errc::corrupt_data || copied.error().code == Errc::unsupported))
          throw error(Errc::corrupt_data,"am.export.source_invalid").caused_by(copied.error());
        const auto written = take(std::move(copied));
        if (written.uncompressed_size != integer(original.at("size_bytes")) || !written.sha256 ||
            hex(*written.sha256) != original.at("sha256").get<std::string>())
          throw error(Errc::corrupt_data, "am.export.source_invalid");
      }
      const auto summary = take(writer.finish());
      prepared.size = summary.file_size;
      auto output = take(az::file_source(prepared.temporary));
      prepared.sha256 = hash(*output, budget, options.stop);
    }
    if (options.hooks.after_write) options.hooks.after_write(prepared.temporary);
    check_stop(options.stop);
    auto output = take(az::file_source(prepared.temporary));
    if (output->size() != prepared.size || hash(*output, budget, options.stop) != prepared.sha256)
      throw error(Errc::corrupt_data, "am.export.verification_failed");
    if (options.full) {
      verification.level = options.verify_full ? repository::VerifyLevel::full : repository::VerifyLevel::quick;
      verification.streams = options.verify_full;
      verification.read_budget = budget.remaining();
      repository::ContainerContents retained;
      const auto report = take(repository::verify_container(prepared.temporary, verification, &retained));
      valid(report,"am.export.verification_failed",options.full);
      if (options.hooks.on_container_verification) options.hooks.on_container_verification(true,report);
      if (retained.budget) take(budget.consume(verification.read_budget - retained.budget->remaining())
          ? Result<void>{} : Result<void>{error(Errc::resource_exhausted, "am.export.read_budget_exceeded")});
    } else {
      az::ZipLimits limits; limits.max_name_bytes = 65535;
      auto archive = take(az::ZipArchive::open(std::make_shared<StoppableSource>(output, options.stop, nullptr), limits));
      if (archive.entries().size() != options.files.size() || archive.summary().comment_length != 0)
        throw error(Errc::corrupt_data, "am.export.verification_failed");
      for (std::size_t i = 0; i < options.files.size(); ++i) {
        check_stop(options.stop);
        const auto& file = options.files[i];
        const auto& original = snapshot.manifest().known().at("files").at(file.file_index);
        auto verified = archive.verify_entry(i, budget, true);
        if (!verified && (verified.error().code == Errc::corrupt_data || verified.error().code == Errc::unsupported))
          throw error(Errc::corrupt_data,"am.export.verification_failed").caused_by(verified.error());
        const auto actual = take(std::move(verified));
        if (archive.entries()[i].raw_name != file.target_name || !archive.entries()[i].utf8_flag ||
            actual.size != integer(original.at("size_bytes")) || !actual.sha256 ||
            hex(*actual.sha256) != original.at("sha256").get<std::string>())
          throw error(Errc::corrupt_data, "am.export.verification_failed");
      }
    }
    check_stop(options.stop);
    return prepared;
  });
}

Result<bool> matches(const Path& path, U64 size, std::string_view sha256, std::stop_token stop) {
  return guard<bool>([&] {
    const auto node = paths::node_info(path);
    if (!node || node->kind != paths::NodeKind::file || node->cloud_placeholder) return false;
    auto source = take(az::file_source(path));
    if (source->size() != size) return false;
    az::ReadBudget budget(size);
    return hash(*source, budget, stop) == sha256;
  });
}
}  // namespace am::storage::exporting
