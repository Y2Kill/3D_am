#pragma once
#include <chrono>
#include <compare>
#include <functional>
#include <variant>
#include "am/domain/manifest/codec.hpp"
#include "am/domain/manifest/semantics.hpp"
#include "am/storage/commit/protocol.hpp"

namespace am::repository {
using Json = nlohmann::ordered_json;
using StorageCapabilities = commit::StorageCapabilities;
using StorageMode = commit::StorageMode;
using Durability = commit::Durability;
struct AssetRef {
  std::string library_id, asset_id;
  auto operator<=>(const AssetRef&) const = default;
};
enum class LibraryMode { read_write, read_only };
enum class FormatSupport { supported, newer_minor };
enum class CheckStatus { not_performed, passed, partial, failed, not_applicable_newer_minor };
enum class StreamStatus { verified_sha256, verified_crc_size_only, unverifiable, unsupported, failed };
enum class ContainerVerification { full, partial };
enum class VerifyLevel { quick, selective, full };
struct StreamReport { std::string id, path; StreamStatus status = StreamStatus::unverifiable; std::optional<Error> error; };
struct VerificationReport {
  FormatSupport format_support = FormatSupport::supported;
  CheckStatus v1 = CheckStatus::not_performed, v2 = CheckStatus::not_performed, v3 = CheckStatus::not_performed;
  std::vector<std::string> v3_rules;
  bool v3_complete = false;
  std::vector<manifest::Finding> v3_findings;
  std::vector<manifest::DeferredRule> v3_deferred_rules;
  std::vector<Error> violations;
  std::vector<StreamReport> v4;
  CheckStatus plugin_validation = CheckStatus::not_performed;
  ContainerVerification container_verification = ContainerVerification::partial;
  std::optional<Error> error;
};
Result<Json> verification_json(const VerificationReport& report);
struct SemanticContext { AssetRef ref; std::optional<std::string> new_operation_id; };
struct SemanticResult : manifest::SemanticReport { std::vector<Error> violations; };
class SemanticValidator {
 public:
  virtual ~SemanticValidator() = default;
  virtual Result<SemanticResult> validate(const manifest::ManifestDocument&, const asset_zip::ZipArchive&, const SemanticContext&) const = 0;
};
// Test-only partial validator. Production defaults include domain semantics.
Result<std::shared_ptr<const SemanticValidator>> repository_invariants();
struct ContainerVerifyOptions {
  VerifyLevel level = VerifyLevel::full;
  std::vector<std::string> file_ids;
  bool streams = true;
  bool semantics = true; // inspect requests V1/V2 only.
  std::uint64_t read_budget = 64ULL * 1024 * 1024 * 1024; // hypothesis, O-17
  asset_zip::ZipLimits zip_limits;
  manifest::CodecLimits codec_limits;
  std::shared_ptr<const SemanticValidator> semantic_validator;
};
// Optional retained read view: consumers use the same open ZIP and job budget.
struct ContainerContents {
  std::optional<asset_zip::ZipArchive> archive;
  std::optional<manifest::ManifestDocument> document;
  std::string manifest_text;
  std::shared_ptr<asset_zip::ReadBudget> budget;
};
Result<VerificationReport> verify_container(const std::filesystem::path&, const ContainerVerifyOptions& = {},
                                           ContainerContents* contents = nullptr);
enum class DriveKind { unknown, fixed, removable, ramdisk, remote, optical };
Result<StorageCapabilities> classify_storage_capabilities(DriveKind drive, std::string_view filesystem);
Result<StorageCapabilities> detect_storage_capabilities(const std::filesystem::path& root);

class AssetSnapshot {
 public:
  AssetSnapshot(AssetSnapshot&&) noexcept;
  AssetSnapshot& operator=(AssetSnapshot&&) noexcept;
  ~AssetSnapshot();
  AssetSnapshot(const AssetSnapshot&) = delete;
  AssetSnapshot& operator=(const AssetSnapshot&) = delete;
  const AssetRef& ref() const noexcept;
  const asset_zip::ZipArchive& archive() const noexcept;
  const manifest::ManifestDocument& manifest() const noexcept;
  const std::string& manifest_text() const noexcept;
  const db::BaseSnapshot& base() const noexcept;
  std::uint64_t revision() const noexcept;
  FormatSupport format_support() const noexcept;
  bool writable() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit AssetSnapshot(std::unique_ptr<Impl>);
  friend class AssetRepository;
};
struct FileInput {
  std::string path;
  std::filesystem::path source;
  std::string origin_kind = "user_added";
  std::optional<std::string> file_id;
  std::optional<Json> metadata;
};
struct AddFiles { Json input; std::vector<FileInput> files; };
struct RemoveFile { std::string file_id; };
struct ReplaceFileContent { std::string file_id; std::filesystem::path source; };
struct SetMetadata { std::string scope = "asset", field; std::optional<Json> value; };
using ChangeOperation = std::variant<AddFiles, RemoveFile, ReplaceFileContent, SetMetadata>;
struct ChangeRequest {
  AssetRef ref;
  std::uint64_t expected_rev = 0;
  std::vector<ChangeOperation> ops;
  std::optional<std::string> op_id, reason;
};
struct CreateRequest {
  std::optional<std::string> asset_id, op_id;
  std::string creation_kind = "user";
  std::optional<Json> metadata;
  std::vector<AddFiles> files;
};
struct IndexedEvent { AssetRef ref; std::uint64_t revision; std::string op_id, kind; };
// Test-only fault injection. Empty in normal use. Mutators run before writing;
// verifier_barrier runs within the protected-file verifier, after all checks.
struct RepositoryTestHooks {
  commit::FaultHooks commit;
  std::function<void()> after_preflight;
  std::function<void(manifest::ManifestDocument&)> mutate_manifest;
  std::function<void()> verifier_barrier;
};
struct RepositoryOptions {
  std::string library_id, application_version;
  LibraryMode mode = LibraryMode::read_write;
  // Test/diagnostic override, not a declaration that an FS was verified (M2).
  std::optional<StorageCapabilities> capabilities;
  std::function<std::chrono::system_clock::time_point()> clock = [] { return std::chrono::system_clock::now(); };
  std::uint64_t read_budget = 64ULL * 1024 * 1024 * 1024; // hypothesis, O-17
  asset_zip::ZipLimits zip_limits;
  manifest::CodecLimits codec_limits;
  std::chrono::milliseconds lock_timeout{};
  std::function<void(const IndexedEvent&)> on_indexed;
  std::shared_ptr<const SemanticValidator> semantic_validator;
  RepositoryTestHooks test_hooks;
};
struct ChangeResult {
  AssetRef ref;
  std::uint64_t revision = 0;
  std::string op_id;
  bool no_op = false;
  std::optional<std::uint64_t> rebased_from;
  VerificationReport verification;
  std::vector<manifest::OpaqueKey> removed_opaque_owners;
  std::optional<commit::CommitResult> commit;
};
class AssetRepository {
 public:
  static Result<AssetRepository> open(const std::filesystem::path&, RepositoryOptions);
  AssetRepository(AssetRepository&&) noexcept;
  AssetRepository& operator=(AssetRepository&&) noexcept;
  ~AssetRepository();
  Result<AssetSnapshot> snapshot(const AssetRef&) const;
  // Optional output receives the actual verification report even on failure.
  Result<ChangeResult> create(const CreateRequest&, VerificationReport* failure_report = nullptr);
  Result<ChangeResult> change(const ChangeRequest&, VerificationReport* failure_report = nullptr);
  Result<VerificationReport> verify(const AssetRef&, VerifyLevel = VerifyLevel::full, const std::vector<std::string>& file_ids = {}) const;
  Result<std::vector<commit::RecoveryItem>> recover();
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  explicit AssetRepository(std::shared_ptr<Impl>);
  Result<AssetSnapshot> load(const AssetRef&, bool require_v2) const;
};
}  // namespace am::repository
