#include <gtest/gtest.h>
#include <sqlite3.h>
#include <fstream>
#include <iostream>
#include "am/storage/db/index_store.hpp"
namespace {
template<class T> T checked(am::Result<T> r) { if(!r) throw std::runtime_error(am::to_log_string(r.error())); return std::move(*r); }
void checked(am::Result<void> r) { if(!r) throw std::runtime_error(am::to_log_string(r.error())); }
struct Database {
 sqlite3* handle=nullptr;
 Database(){ if(sqlite3_open(":memory:",&handle)!=SQLITE_OK) throw std::runtime_error("Open failed"); }
 ~Database(){sqlite3_close(handle);}
};
int profile(unsigned,void* context,void* statement,void*) {
 *static_cast<long long*>(context)+=sqlite3_stmt_status(static_cast<sqlite3_stmt*>(statement),SQLITE_STMTSTATUS_VM_STEP,1); return 0;
}
std::pair<long long,long long> cost(int count) {
 Database database; auto connection=am::db::Connection::borrow(database.handle);
 std::ifstream input(AM_INDEX_SCHEMA); std::string schema{std::istreambuf_iterator<char>(input),{}}; checked(connection.exec(schema));
 am::db::IndexStore store(database.handle);
 {
  auto transaction=checked(store.begin_rebuild());
  for(int i=0;i<count;++i) {
   am::db::IndexRecord r; r.summary.asset_id="scale-"+std::to_string(i); r.summary.revision=1; r.summary.health="ok";
   r.summary.title="Indexed title"; r.rel_path=r.summary.asset_id+".zip"; r.zip_size="0"; r.manifest_json="{}";r.names={"model.stl"};
   checked(transaction.insert(r));
  }
  checked(transaction.commit("2026-11-20T00:00:00Z"));
 }
 for (auto* statement=sqlite3_next_stmt(database.handle,nullptr);statement;statement=sqlite3_next_stmt(database.handle,statement))
   (void)sqlite3_stmt_status(statement,SQLITE_STMTSTATUS_VM_STEP,1);
 long long steps=0;sqlite3_trace_v2(database.handle,SQLITE_TRACE_PROFILE,profile,&steps);
 checked(store.remove("scale-0")); const auto erase=steps;steps=0;
 am::db::IndexRecord r; r.summary.asset_id="scale-new";r.summary.revision=1;r.summary.health="ok";r.rel_path="new.zip";r.zip_size="0";r.manifest_json="{}";r.names={"model.stl"};
 checked(store.replace(r));const auto put=steps;sqlite3_trace_v2(database.handle,0,nullptr,nullptr);
 return {erase,put};
}
}
TEST(IndexScale, EraseAndPutDoNotScanOtherAssets) {
 RecordProperty("req","K-112");
 const auto small=cost(100),large=cost(5000);
 std::cout<<"VM erase 100="<<small.first<<" 5000="<<large.first<<"; put 100="<<small.second<<" 5000="<<large.second<<'\n';
 // SQLite 3.53.4 measured erase=613/613 and put=429/429 at 100/5000 rows.
 // Allow 128 extra bookkeeping steps across SQLite builds; an asset_id scan adds 117600.
 constexpr long long bookkeeping=128;
 EXPECT_LE(large.first,small.first+bookkeeping);
 EXPECT_LE(large.second,small.second+bookkeeping);
}

TEST(IndexScale, ReplaceAndRemoveLeaveNoOrphanFtsRows) {
 RecordProperty("req","K-112");
 Database database;auto connection=am::db::Connection::borrow(database.handle);
 std::ifstream input(AM_INDEX_SCHEMA);checked(connection.exec(std::string{std::istreambuf_iterator<char>(input),{}}));
 am::db::IndexStore store(database.handle);am::db::IndexRecord r;
 r.summary.asset_id="target";r.summary.revision=1;r.summary.health="ok";r.summary.title="Searchable asset";
 r.rel_path="target.zip";r.zip_size="0";r.manifest_json="{}";r.names={"searchable.stl","searchable.obj"};
 checked(store.replace(r));
 checked(connection.exec("INSERT INTO fts_docs_owner(fts_rowid,asset_id) VALUES(71,'target'); "
                         "INSERT INTO fts_docs(rowid,asset_id,file_id,body) VALUES(71,'target','file','searchable document')"));
 auto count=[&](const std::string& table) {auto s=checked(connection.prepare("SELECT count(*) FROM "+table));checked(s.step());return s.integer(0);};
 EXPECT_EQ(count("fts_assets WHERE fts_assets MATCH 'Searchable'"),1);
 EXPECT_EQ(count("fts_names WHERE fts_names MATCH 'searchable'"),2);
 EXPECT_EQ(count("fts_docs WHERE fts_docs MATCH 'searchable'"),1);
 checked(store.replace(r)); EXPECT_EQ(count("fts_assets"),1);EXPECT_EQ(count("fts_names"),2);EXPECT_EQ(count("fts_docs"),0);
 EXPECT_EQ(count("fts_docs_owner"),0);EXPECT_EQ(count("fts_names_owner"),2);
 checked(store.remove("target"));
 for(const auto* table:{"fts_assets","fts_names","fts_docs","fts_names_owner","fts_docs_owner"}) EXPECT_EQ(count(table),0);
}
