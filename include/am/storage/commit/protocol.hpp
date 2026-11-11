#pragma once
#include <chrono>
#include <functional>
#include "am/storage/db/commit_journal.hpp"
#include "am/storage/asset_zip/writer.hpp"

namespace am::commit {
enum class FaultPoint { after_lock, after_prepared, mid_write, after_finish, after_verify,
  after_verified_journal, before_replace, after_replace, after_committed_journal, after_indexed_journal };
std::string_view point_name(FaultPoint point) noexcept;
enum class Replacement { posix_rename, move_file_ex, rename };
std::string_view replacement_name(Replacement method) noexcept;
struct StorageCapabilities { std::uint64_t max_file_size = UINT64_MAX; };
// Optional injection surfaces; empty in normal operation. Mid-write counts sink
// bytes emitted while the producer is active, excluding header backpatches.
struct FaultHooks {
  std::function<Result<void>(FaultPoint)> point;
  std::optional<std::uint64_t> mid_write_after_bytes;
  std::function<Result<std::uint64_t>(const std::filesystem::path&)> free_space;
  std::function<Result<std::shared_ptr<asset_zip::RandomAccessSink>>(const std::filesystem::path&)> create_sink;
  std::optional<std::uint32_t> rename_ex_error;
  std::function<Result<void>(const std::filesystem::path&)> before_fallback;
};
class AssetLock {
 public:
  ~AssetLock();
  AssetLock(AssetLock&&) noexcept;
  AssetLock& operator=(AssetLock&&) noexcept;
  static Result<AssetLock> acquire(const std::filesystem::path& library_root, std::string_view asset_id,
                                    std::chrono::milliseconds timeout = {});
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit AssetLock(std::unique_ptr<Impl> impl);
};
struct CommitRequest {
  explicit CommitRequest(asset_zip::ReadBudget& b) : budget(b) {}
  asset_zip::ReadBudget& budget;
  std::filesystem::path library_root, target_path;
  std::string asset_id, kind = "update", op_id;
  std::optional<db::BaseSnapshot> base;
  std::optional<std::int64_t> expected_rev;
  std::optional<std::string> job_id;
  std::uint64_t estimated_size = 0;
  StorageCapabilities capabilities;
  asset_zip::ZipLimits zip_limits;
  std::shared_ptr<db::CommitJournal> journal;
  std::chrono::milliseconds lock_timeout{};
  std::function<Result<void>(asset_zip::ZipWriter&)> producer;
  std::function<Result<void>(const asset_zip::ZipArchive&)> verifier;
  std::function<Result<void>(const db::PendingOperation&)> on_committed;
  FaultHooks hooks;
};
struct CommitResult {
  std::string op_id;
  std::uint64_t size = 0;
  std::string manifest_sha256;
  std::int64_t mtime_ns = 0;
  Replacement replacement = Replacement::posix_rename;
  asset_zip::WriteSummary summary;
  double file_hash_seconds = 0;
};
Result<CommitResult> commit_zip(const CommitRequest& request);
Result<db::BaseSnapshot> inspect_zip(const std::filesystem::path& path, asset_zip::ReadBudget& budget,
                                   const asset_zip::ZipLimits& limits = {});
enum class RecoveryOutcome { rolled_back, completed, already_committed, cleaned, skipped_locked, skipped, failed };
std::string_view outcome_name(RecoveryOutcome outcome) noexcept;
struct RecoveryItem {
  std::string op_id;
  std::filesystem::path path;
  RecoveryOutcome outcome = RecoveryOutcome::failed;
  bool verifier_skipped = false;
  std::optional<Error> error;
  std::string detail;
};
struct RecoveryOptions {
  std::shared_ptr<db::CommitJournal> journal;
  std::function<Result<void>(const db::PendingOperation&)> on_committed;
  std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
  std::chrono::seconds orphan_age = std::chrono::hours(24);
  std::chrono::milliseconds lock_timeout{};
  asset_zip::ZipLimits zip_limits;
  FaultHooks hooks;
};
Result<std::vector<RecoveryItem>> recover(const std::filesystem::path& library_root, const RecoveryOptions& options = {});
}  // namespace am::commit
