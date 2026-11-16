#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "am/storage/db/sqlite.hpp"

namespace am::db {
struct IndexedFile {
  std::string id, storage_path, name, size, sha256, origin_kind;
  std::optional<std::string> type_id;
};
struct IndexedEntry {
  std::string id, input_id, object_id, path, name, kind, result;
  std::optional<std::string> size, crc32, file_id;
};
struct IndexedObject {
  std::string id, input_id, kind, name;
  std::int64_t volume_index = -1;
  std::optional<std::string> size, sha256;
};
struct IndexedSource {
  std::string id, kind;
  std::optional<std::string> name, url;
};
struct IndexedField {
  std::string scope, owner_id, field_id;
  std::optional<std::string> type_id, txt, unit;
  std::optional<double> num;
};
struct IndexSummary {
  std::string asset_id, health;
  std::int64_t revision = 0, file_count = 0;
  std::optional<std::string> title, content_bytes;
  bool in_trash = false;
  bool operator==(const IndexSummary&) const = default;
};
struct IndexRecord {
  IndexSummary summary;
  std::string rel_path, zip_size, manifest_sha256, created_at, modified_at, formats, manifest_json;
  std::int64_t zip_mtime_ns = 0, format_major = 1, format_minor = 0;
  bool incomplete = false;
  std::optional<std::string> description, health_detail, primary_preview;
  std::string authors_text, tags_text, sources_text;
  std::vector<IndexedFile> files;
  std::vector<IndexedEntry> entries;
  std::vector<IndexedObject> objects;
  std::vector<IndexedSource> sources;
  std::vector<std::string> tags, categories, names;
  std::vector<IndexedField> fields;
};
struct IndexState {
  std::string asset_id, rel_path, zip_size, health;
  std::int64_t zip_mtime_ns = 0;
};
struct IndexStat {
  std::string zip_size;
  std::int64_t zip_mtime_ns = 0;
};
struct IndexMark {
  std::string asset_id, health, health_detail;
  std::optional<IndexStat> stat;
};
struct TrashSummary {
  std::uint64_t assets = 0;
  std::optional<std::string> bytes;
};
Result<TrashSummary> summarize_trash(const std::vector<std::string>& zip_sizes);
class IndexStore;
class IndexTransaction {
 public:
  IndexTransaction(IndexTransaction&&) noexcept = default;
  IndexTransaction& operator=(IndexTransaction&&) noexcept = default;
  IndexTransaction(const IndexTransaction&) = delete;
  IndexTransaction& operator=(const IndexTransaction&) = delete;
  Result<void> insert(const IndexRecord&);
  Result<void> mark(const IndexMark&);
  Result<void> commit();
  Result<void> commit(std::string_view rebuilt_at);

 private:
  IndexStore* store_;
  Transaction transaction_;
  IndexTransaction(IndexStore&, Transaction&&);
  friend class IndexStore;
};
using IndexRebuild = IndexTransaction;
// Borrows Library's index connection. The Library must outlive this store and
// any rebuild session. SQL and SQLite-specific policy stay at the storage level.
class IndexStore {
 public:
  explicit IndexStore(sqlite3* db) noexcept : db_(Connection::borrow(db)) {}
  IndexStore(const IndexStore&) = delete;
  IndexStore& operator=(const IndexStore&) = delete;
  Result<void> replace(const IndexRecord&);
  Result<void> remove(std::string_view asset_id);
  Result<IndexTransaction> begin_rebuild();
  Result<IndexTransaction> begin_sync();
  Result<std::vector<IndexState>> sync_state() const;
  Result<void> mark(const IndexMark&);
  Result<std::vector<IndexSummary>> list(bool include_trash = false) const;
  Result<std::int64_t> count() const;
  Result<TrashSummary> trash_summary() const;
  Result<bool> rebuild_required() const;
  Result<void> mark_rebuild_required();

 private:
  Connection db_;
  friend class IndexTransaction;
};
}  // namespace am::db
