#pragma once
#include "am/storage/db/index_store.hpp"
#include "am/storage/library/library.hpp"

namespace am::catalog {
struct Location {
  std::string rel_path, zip_size;
  std::int64_t zip_mtime_ns = 0;
  bool in_trash = false;
};
struct SkippedAsset {
  std::string path, reason, message_id;
};
struct RebuildReport {
  std::size_t indexed = 0;
  std::vector<SkippedAsset> skipped;
  std::vector<std::string> ignored;
  std::int64_t duration_ms = 0;
};
struct SyncOptions {
  bool full = false;
};
struct SyncReport {
  std::size_t added = 0, updated = 0, unchanged = 0;
  std::vector<std::string> missing;
  std::vector<SkippedAsset> skipped;
  std::vector<std::string> ignored;
  std::int64_t duration_ms = 0;
  bool full = false;
};
struct IndexerHooks {
  // Per-instance failure injection. No global state; empty in production.
  std::function<Result<void>(std::string_view)> before_update;
  std::function<Result<void>(std::size_t)> after_rebuild_insert;
  std::function<Result<void>(const std::filesystem::path&)> before_read;
  std::function<Result<void>(std::size_t)> after_sync_change;
};
std::string tag_search_key(std::string_view);
class Indexer {
 public:
  Indexer(std::filesystem::path root, library::Library&, IndexerHooks = {}) noexcept;
  // original is the source text corresponding to the document, including opaque values.
  static Result<db::IndexRecord> map(const manifest::ManifestDocument&, std::string_view original, const Location&,
                                     const repository::VerificationReport&);
  Result<RebuildReport> rebuild();
  Result<SyncReport> sync(const SyncOptions& = {});
  Result<void> update(const repository::AssetRef&, bool in_trash = false);
  Result<void> remove(std::string_view asset_id);
  Result<std::vector<db::IndexSummary>> list(bool include_trash = false) const;
  Result<bool> rebuild_required() const;
  Result<std::int64_t> count() const;
  Result<void> mark_rebuild_required();

 private:
  std::filesystem::path root_;
  library::Library& library_;
  db::IndexStore store_;
  IndexerHooks hooks_;
  Result<db::IndexRecord> read(const std::filesystem::path&, const std::string&, bool) const;
};
struct CatalogChange {
  repository::ChangeResult change;
  std::vector<Error> warnings;
};
struct CatalogLifecycle {
  repository::LifecycleResult change;
  std::vector<Error> warnings;
};
// Owns the write owner and its observer. The stable Impl also keeps callbacks
// valid when Catalog is moved. Index failures never undo a committed ZIP.
class Catalog {
 public:
  static Result<Catalog> open(const std::filesystem::path&, library::OpenOptions = {}, IndexerHooks = {});
  Catalog(Catalog&&) noexcept;
  Catalog& operator=(Catalog&&) noexcept;
  ~Catalog();
  Catalog(const Catalog&) = delete;
  Catalog& operator=(const Catalog&) = delete;
  library::Library& library() noexcept;
  Indexer& indexer() noexcept;
  Result<SyncReport> sync(const SyncOptions& = {});
  Result<CatalogChange> create(const repository::CreateRequest&);
  Result<CatalogChange> change(const repository::ChangeRequest&);
  Result<CatalogLifecycle> trash(const repository::AssetRef&);
  Result<CatalogLifecycle> restore(const repository::AssetRef&);
  Result<CatalogLifecycle> purge(const repository::AssetRef&);
  Result<repository::ImportReadyResult> import_ready(const repository::ImportReadyRequest&,
                                                     repository::ImportReadyResult* failure_details = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit Catalog(std::unique_ptr<Impl>);
};
}  // namespace am::catalog
