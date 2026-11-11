#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "am/foundation/result.hpp"

namespace am::db {
enum class CommitState { prepared, verified, committed, indexed };
std::string_view state_name(CommitState state) noexcept;
struct BaseSnapshot {
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;
  std::string manifest_sha256;
  std::optional<std::int64_t> revision;
};
struct NewFileEvidence {
  std::uint64_t size = 0;
  std::string file_sha256, manifest_sha256;
};
struct PendingOperation {
  std::string op_id, asset_id, kind;
  std::filesystem::path target_path, temp_path;
  std::optional<BaseSnapshot> base;
  CommitState state = CommitState::prepared;
  std::optional<std::int64_t> expected_rev;
  std::string started_at, updated_at;
  std::optional<std::string> job_id;
  std::optional<NewFileEvidence> verified;
};
class CommitJournal {
 public:
  virtual ~CommitJournal() = default;
  virtual Result<void> insert(const PendingOperation& operation) = 0;
  // Evidence is mandatory for prepared -> verified and is stored atomically.
  virtual Result<void> advance(std::string_view op_id, CommitState from, CommitState to,
                               const std::optional<NewFileEvidence>& evidence = {}) = 0;
  virtual Result<void> remove(std::string_view op_id) = 0;
  virtual Result<std::vector<PendingOperation>> list() = 0;
  virtual Result<std::optional<PendingOperation>> find(std::string_view op_id) = 0;
};
class StateJournal final : public CommitJournal {
 public:
  ~StateJournal() override;
  static Result<std::shared_ptr<StateJournal>> open(const std::filesystem::path& library_root);
  Result<void> insert(const PendingOperation& operation) override;
  Result<void> advance(std::string_view op_id, CommitState from, CommitState to,
                       const std::optional<NewFileEvidence>& evidence = {}) override;
  Result<void> remove(std::string_view op_id) override;
  Result<std::vector<PendingOperation>> list() override;
  Result<std::optional<PendingOperation>> find(std::string_view op_id) override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit StateJournal(std::unique_ptr<Impl> impl);
};
}  // namespace am::db
