#include "am/storage/db/index_store.hpp"

#include <exception>
#include <variant>

namespace am::db {
namespace {
template <class T>
T take(Result<T> r) {
  if (!r) throw r.error();
  return std::move(r).value();
}
void take(Result<void> r) {
  if (!r) throw r.error();
}
template <class T, class F>
Result<T> guard(F&& f) {
  try {
    return f();
  } catch (const Error& e) {
    return e;
  } catch (const std::bad_alloc&) {
    return Error::make(Errc::resource_exhausted, "Index allocation failed").id("am.db.out_of_memory");
  } catch (const std::exception& e) {
    return Error::make(Errc::io_error, "Index operation failed", e.what()).id("am.db.index");
  } catch (...) {
    return Error::make(Errc::internal, "Index operation failed").id("am.db.index");
  }
}
using Cell = std::variant<std::monostate, std::string, std::int64_t, double>;
Cell cell(const std::optional<std::string>& s) { return s ? Cell(*s) : Cell{}; }
Cell cell(const std::optional<double>& d) { return d ? Cell(*d) : Cell{}; }
void insert(Connection& c, std::string_view table, std::string_view columns, std::initializer_list<Cell> cells) {
  // Identifiers are internal constants; every data value is bound separately.
  std::string sql = "INSERT INTO " + std::string(table) + "(" + std::string(columns) + ") VALUES(";
  for (std::size_t i = 0; i < cells.size(); ++i) sql += i == 0 ? "?" : ",?";
  auto s = take(c.prepare(sql + ")"));
  int index = 0;
  for (const auto& v : cells) {
    ++index;
    std::visit(
        [&](const auto& x) {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, std::monostate>)
            take(s.bind_null(index));
          else if constexpr (std::is_same_v<T, std::string>)
            take(s.bind(index, std::string_view(x)));
          else
            take(s.bind(index, x));
        },
        v);
  }
  take(s.step());
}
void erase(Connection& c, std::string_view id) {
  // Foreign keys cascade the ordinary child rows; FTS tables have no FK.
  for (const auto* table : {"fts_assets", "fts_names", "fts_docs", "assets"}) {
    auto s = take(c.prepare("DELETE FROM " + std::string(table) + " WHERE asset_id=?"));
    take(s.bind(1, id));
    take(s.step());
  }
}
void put(Connection& c, const IndexRecord& r) {
  const auto& a = r.summary;
  const auto& id = a.asset_id;
  erase(c, id);
  insert(c, "assets",
         "asset_id,revision,rel_path,in_trash,zip_size,zip_mtime_ns,manifest_sha256,format_major,format_minor,title,"
         "description,created_at,modified_at,incomplete,health,health_detail,primary_preview,file_count,content_bytes,"
         "formats,manifest_json",
         {id,
          a.revision,
          r.rel_path,
          std::int64_t(a.in_trash),
          r.zip_size,
          r.zip_mtime_ns,
          r.manifest_sha256,
          r.format_major,
          r.format_minor,
          cell(a.title),
          cell(r.description),
          r.created_at,
          r.modified_at,
          std::int64_t(r.incomplete),
          a.health,
          cell(r.health_detail),
          cell(r.primary_preview),
          a.file_count,
          cell(a.content_bytes),
          r.formats,
          r.manifest_json});
  for (const auto& f : r.files)
    insert(c, "files", "asset_id,file_id,storage_path,name,size,sha256,origin_kind,type_id",
           {id, f.id, f.storage_path, f.name, f.size, f.sha256, f.origin_kind, cell(f.type_id)});
  for (const auto& e : r.entries)
    insert(c, "input_entries",
           "asset_id,entry_id,input_id,object_id,src_path,src_name,entry_kind,size,crc32,result,file_id",
           {id, e.id, e.input_id, e.object_id, e.path, e.name, e.kind, cell(e.size), cell(e.crc32), e.result,
            cell(e.file_id)});
  for (const auto& o : r.objects)
    insert(c, "input_objects", "asset_id,object_id,input_id,kind,volume_index,original_name,size,sha256",
           {id, o.id, o.input_id, o.kind, o.volume_index, o.name, cell(o.size), cell(o.sha256)});
  for (const auto& s : r.sources)
    insert(c, "sources", "asset_id,source_id,kind,name,url", {id, s.id, s.kind, cell(s.name), cell(s.url)});
  for (const auto& t : r.tags) insert(c, "asset_tags", "asset_id,tag", {id, t});
  for (const auto& p : r.categories) insert(c, "asset_categories", "asset_id,path", {id, p});
  for (const auto& f : r.fields)
    insert(c, "asset_fields", "asset_id,scope,owner_id,field_id,type_id,num,txt,unit",
           {id, f.scope, f.owner_id, f.field_id, cell(f.type_id), cell(f.num), cell(f.txt), cell(f.unit)});
  insert(c, "fts_assets", "asset_id,title,description,authors,tags,sources",
         {id, cell(a.title), cell(r.description), r.authors_text, r.tags_text, r.sources_text});
  for (const auto& n : r.names) insert(c, "fts_names", "asset_id,name", {id, n});
}
void meta(Connection& c, std::string_view key, std::string_view value) {
  auto s =
      take(c.prepare("INSERT INTO meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
  take(s.bind(1, key));
  take(s.bind(2, value));
  take(s.step());
}
void mark_row(Connection& c, const IndexMark& mark) {
  auto s =
      take(c.prepare(mark.stat ? "UPDATE assets SET health=?,health_detail=?,zip_size=?,zip_mtime_ns=? WHERE asset_id=?"
                               : "UPDATE assets SET health=?,health_detail=? WHERE asset_id=?"));
  take(s.bind(1, mark.health));
  take(s.bind(2, mark.health_detail));
  if (mark.stat) {
    take(s.bind(3, mark.stat->zip_size));
    take(s.bind(4, mark.stat->zip_mtime_ns));
    take(s.bind(5, mark.asset_id));
  } else
    take(s.bind(3, mark.asset_id));
  take(s.step());
}
}  // namespace
Result<void> IndexStore::replace(const IndexRecord& r) {
  return guard<void>([&]() -> Result<void> {
    auto tx = take(Transaction::begin(db_));
    put(db_, r);
    take(tx.commit());
    return {};
  });
}
Result<void> IndexStore::remove(std::string_view id) {
  return guard<void>([&]() -> Result<void> {
    auto tx = take(Transaction::begin(db_));
    erase(db_, id);
    take(tx.commit());
    return {};
  });
}
Result<IndexTransaction> IndexStore::begin_rebuild() {
  return guard<IndexTransaction>([&]() -> Result<IndexTransaction> {
    auto tx = take(Transaction::begin(db_));
    take(db_.exec("DELETE FROM fts_assets; DELETE FROM fts_names; DELETE FROM fts_docs; DELETE FROM assets"));
    return IndexTransaction(*this, std::move(tx));
  });
}
Result<IndexTransaction> IndexStore::begin_sync() {
  return guard<IndexTransaction>(
      [&]() -> Result<IndexTransaction> { return IndexTransaction(*this, take(Transaction::begin(db_))); });
}
IndexTransaction::IndexTransaction(IndexStore& s, Transaction&& t) : store_(&s), transaction_(std::move(t)) {}
Result<void> IndexTransaction::insert(const IndexRecord& r) {
  return guard<void>([&]() -> Result<void> {
    put(store_->db_, r);
    return {};
  });
}
Result<void> IndexTransaction::mark(const IndexMark& mark) {
  return guard<void>([&]() -> Result<void> {
    mark_row(store_->db_, mark);
    return {};
  });
}
Result<void> IndexTransaction::commit() { return transaction_.commit(); }
Result<void> IndexTransaction::commit(std::string_view time) {
  return guard<void>([&]() -> Result<void> {
    meta(store_->db_, "rebuild_required", "0");
    meta(store_->db_, "rebuilt_at", time);
    take(transaction_.commit());
    return {};
  });
}
Result<void> IndexStore::mark_rebuild_required() {
  return guard<void>([&]() -> Result<void> {
    meta(db_, "rebuild_required", "1");
    return {};
  });
}
Result<void> IndexStore::mark(const IndexMark& mark) {
  return guard<void>([&]() -> Result<void> {
    auto tx = take(Transaction::begin(db_));
    mark_row(db_, mark);
    take(tx.commit());
    return {};
  });
}
Result<std::vector<IndexState>> IndexStore::sync_state() const {
  return guard<std::vector<IndexState>>([&]() -> Result<std::vector<IndexState>> {
    auto statement =
        take(db_.prepare("SELECT asset_id,rel_path,zip_size,health,zip_mtime_ns FROM assets ORDER BY asset_id"));
    std::vector<IndexState> rows;
    while (take(statement.step()) == Step::row)
      rows.push_back({take(statement.text(0)), take(statement.text(1)), take(statement.text(2)),
                      take(statement.text(3)), statement.integer(4)});
    return rows;
  });
}
Result<bool> IndexStore::rebuild_required() const {
  return guard<bool>([&]() -> Result<bool> {
    auto s = take(db_.prepare("SELECT value FROM meta WHERE key='rebuild_required'"));
    return take(s.step()) != Step::row || take(s.text(0)) != "0";
  });
}
Result<std::int64_t> IndexStore::count() const {
  return guard<std::int64_t>([&]() -> Result<std::int64_t> {
    auto s = take(db_.prepare("SELECT count(*) FROM assets"));
    take(s.step());
    return s.integer(0);
  });
}
Result<std::vector<IndexSummary>> IndexStore::list(bool trash) const {
  return guard<std::vector<IndexSummary>>([&]() -> Result<std::vector<IndexSummary>> {
    auto s =
        take(db_.prepare("SELECT asset_id,revision,title,health,file_count,content_bytes,in_trash FROM assets WHERE ? "
                         "OR in_trash=0 ORDER BY asset_id"));
    take(s.bind(1, std::int64_t(trash)));
    std::vector<IndexSummary> rows;
    while (take(s.step()) == Step::row) {
      IndexSummary r;
      r.asset_id = take(s.text(0));
      r.revision = s.integer(1);
      if (!s.is_null(2)) r.title = take(s.text(2));
      r.health = take(s.text(3));
      r.file_count = s.integer(4);
      if (!s.is_null(5)) r.content_bytes = take(s.text(5));
      r.in_trash = s.integer(6) != 0;
      rows.push_back(std::move(r));
    }
    return rows;
  });
}
}  // namespace am::db
