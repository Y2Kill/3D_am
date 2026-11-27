#include <gtest/gtest.h>
#include "am/application/library_service.hpp"
#include "fault_world.hpp"

namespace {
using namespace fault_test;
bool warned(const am::catalog::Catalog& c, std::string_view id) {
  for (const auto& e:c.open_report().warnings) if (e.message_id==id) return true;
  return false;
}
bool retained(const am::catalog::Catalog& c,const fs::path& path,std::string_view reason) {
  for (const auto& item:c.open_report().recovered)
    if (item.outcome==am::commit::RecoveryOutcome::skipped && item.detail==reason &&
        fs::exists(item.path) && fs::exists(path) && fs::equivalent(item.path,path)) return true;
  return false;
}
void remove_state(const World& w) {
  for (const auto* tail:{"","-wal","-shm"}) fs::remove(w.root/".am"/(std::string("state.sqlite")+tail));
}
void corrupt_log_page(const World& w) {
  const auto path=w.root/".am/state.sqlite";
  std::int64_t page=0,size=0;
  {
    auto db=take(am::db::Connection::open(path));
    auto q=take(db.prepare("SELECT rootpage FROM sqlite_schema WHERE name='op_log'"));
    take(q.step()); page=q.integer(0);
    auto p=take(db.prepare("PRAGMA page_size")); take(p.step()); size=p.integer(0);
  }
  if (page<=1 || size<=0) throw std::runtime_error("Invalid corruption fixture page");
  std::fstream file(am::paths::native(path),std::ios::in|std::ios::out|std::ios::binary);
  file.seekp((page-1)*size); file.put('\0');
  if (!file) throw std::runtime_error("Cannot corrupt synthetic data page");
}
}

TEST(FaultState, MissingStateWarnsPersistsHistoryAndRetainsOnlyThisOpening) {
  RecordProperty("req","K-117|K-122|ADR-0007");
  World w;
  const auto old=hash(w.zip()),userdata=hash(w.root/".am/userdata.sqlite");
  const auto orphan=w.orphan();
  remove_state(w);
  {
    auto c=take(am::catalog::Catalog::open(w.root,writable()));
    EXPECT_EQ(c.library().database_info(am::library::DatabaseKind::state).reason,"recreated");
    EXPECT_TRUE(warned(c,"am.library.state_recreated"));
    EXPECT_TRUE(retained(c,orphan,"journal_recreated"));
    auto entries=take(c.operation_log().query());
    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(entries.front().code,"am.library.state_recreated");
    EXPECT_FALSE(entries.front().at.empty());
  }
  const auto info=take(am::application::inspect_library(w.root)).value.at("databases").at("state");
  EXPECT_EQ(info.at("state"),"ok");
  EXPECT_EQ(info.at("reason"),"recreated");
  EXPECT_TRUE(info.at("recreated_at").is_string());
  EXPECT_TRUE(fs::exists(orphan));
  {
    auto c=take(am::catalog::Catalog::open(w.root,writable()));
    EXPECT_FALSE(warned(c,"am.library.state_recreated"));
    EXPECT_FALSE(fs::exists(orphan));
  }
  EXPECT_EQ(hash(w.zip()),old);
  EXPECT_EQ(hash(w.root/".am/userdata.sqlite"),userdata);
}

TEST(FaultState, ClosedUnclosedWalAndNoWalUseDistinctOrphanPolicies) {
  RecordProperty("req","K-117|K-122");
  for (const auto mode:{"closed","wal","no_wal"}) {
    SCOPED_TRACE(mode);
    World w;
    if (std::string_view(mode)!="closed") w.unclose();
    const auto orphan=w.orphan();
    std::optional<am::db::Connection> holder;
    if (std::string_view(mode)=="wal") {
      holder.emplace(take(am::db::Connection::open(w.root/".am/state.sqlite")));
      take(holder->exec("PRAGMA wal_autocheckpoint=0; INSERT INTO op_log(at,level,code,message) VALUES('2020-01-01T00:00:00Z','info','fixture','confirmed')"));
      ASSERT_GT(fs::file_size(w.root/".am/state.sqlite-wal"),0u);
    } else {
      EXPECT_FALSE(fs::exists(w.root/".am/state.sqlite-wal"));
    }
    {
      auto c=take(am::catalog::Catalog::open(w.root,writable()));
      const bool keep=std::string_view(mode)=="no_wal";
      EXPECT_EQ(fs::exists(orphan),keep);
      EXPECT_EQ(retained(c,orphan,"journal_may_be_lost"),keep);
      EXPECT_FALSE(warned(c,"am.library.state_recreated"));
    }
    holder.reset();
    if (std::string_view(mode)=="no_wal") {
      auto c=take(am::catalog::Catalog::open(w.root,writable()));
      EXPECT_FALSE(fs::exists(orphan));
    }
  }
}

TEST(FaultState, InvalidHeaderEmptyAndMissingSchemaNeverRecreateOrChangeProtectedFiles) {
  RecordProperty("req","K-117|K-122|ADR-0007");
  for (const auto mode:{"not_sqlite","empty","schema"}) {
    SCOPED_TRACE(mode);
    World w;
    const auto state=w.root/".am/state.sqlite";
    if (std::string_view(mode)=="schema") {
      auto db=take(am::db::Connection::open(state)); take(db.exec("DROP TABLE jobs"));
    } else write(state,std::string_view(mode)=="empty" ? "" : "Not a SQLite database");
    const auto original=bytes(state);
    const auto zip=hash(w.zip()),index=hash(w.root/".am/index.sqlite"),user=hash(w.root/".am/userdata.sqlite"),lock=hash(w.root/".am/library.lock");
    auto c=am::catalog::Catalog::open(w.root,writable());
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().message_id,"am.library.db_error");
    EXPECT_EQ(bytes(state),original);
    EXPECT_EQ(hash(w.zip()),zip);
    EXPECT_EQ(hash(w.root/".am/index.sqlite"),index);
    EXPECT_EQ(hash(w.root/".am/userdata.sqlite"),user);
    EXPECT_EQ(hash(w.root/".am/library.lock"),lock);
    auto ro=writable(); ro.mode=am::library::Mode::read_only;
    EXPECT_TRUE(am::catalog::Catalog::open(w.root,ro));
    EXPECT_EQ(bytes(state),original);
  }
}

TEST(FaultState, UnclosedCorruptDataPageFailsBeforeWriterChanges) {
  RecordProperty("req","K-117|K-122|ADR-0007");
  World w; w.unclose(); corrupt_log_page(w);
  const auto state=hash(w.root/".am/state.sqlite"),index=hash(w.root/".am/index.sqlite"),user=hash(w.root/".am/userdata.sqlite"),zip=hash(w.zip()),lock=hash(w.root/".am/library.lock");
  auto c=am::catalog::Catalog::open(w.root,writable());
  ASSERT_FALSE(c);
  EXPECT_EQ(c.error().message_id,"am.library.db_error");
  EXPECT_EQ(hash(w.root/".am/state.sqlite"),state);
  EXPECT_EQ(hash(w.root/".am/index.sqlite"),index);
  EXPECT_EQ(hash(w.root/".am/userdata.sqlite"),user);
  EXPECT_EQ(hash(w.root/".am/library.lock"),lock);
  EXPECT_EQ(hash(w.zip()),zip);
}

TEST(FaultState, ClosedCorruptPageIsExplicitAtUseRollsBackAndVerifyFindsIt) {
  RecordProperty("req","K-117|K-122|K-22");
  World w; corrupt_log_page(w);
  const auto zip=hash(w.zip()),state=hash(w.root/".am/state.sqlite");
  auto c=take(am::catalog::Catalog::open(w.root,writable()));
  auto query=c.operation_log().query();
  ASSERT_FALSE(query);
  EXPECT_EQ(query.error().code,am::Errc::corrupt_data);
  {
    auto db=am::db::Connection::borrow(c.library().database(am::library::DatabaseKind::state));
    auto tx=take(am::db::Transaction::begin(db));
    take(db.exec("INSERT INTO snapshots(asset_id,revision,pid,opened_at) VALUES('rollback',1,1,'2020-01-01T00:00:00Z')"));
    auto failed=db.exec("INSERT INTO op_log(at,level,code,message) VALUES('2020-01-01T00:00:00Z','info','bad','bad')");
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code,am::Errc::corrupt_data);
  }
  auto db=am::db::Connection::borrow(c.library().database(am::library::DatabaseKind::state));
  auto q=take(db.prepare("SELECT count(*) FROM snapshots WHERE asset_id='rollback'"));
  take(q.step()); EXPECT_EQ(q.integer(0),0);
  for (const auto level:{am::catalog::LibraryVerifyLevel::quick,am::catalog::LibraryVerifyLevel::full}) {
    am::catalog::LibraryVerifyOptions o; o.level=level;
    const auto report=take(c.verify(o));
    EXPECT_TRUE(report.database_failed);
    bool found=false;
    for (const auto& d:report.databases) if (d.database==am::library::DatabaseKind::state && d.status=="corrupt") found=true;
    EXPECT_TRUE(found);
  }
  EXPECT_EQ(hash(w.zip()),zip);
  EXPECT_EQ(hash(w.root/".am/state.sqlite"),state);
}

TEST(FaultState, WriterTransactionRefusesOpeningPromptlyAndReadOnlyWorks) {
  RecordProperty("req","K-117|K-122|ADR-0007");
  World w;
  auto holder=take(am::db::Connection::open(w.root/".am/state.sqlite"));
  auto tx=take(am::db::Transaction::begin(holder));
  const auto zip=hash(w.zip());
  const auto started=std::chrono::steady_clock::now();
  auto c=am::catalog::Catalog::open(w.root,writable());
  ASSERT_FALSE(c);
  EXPECT_EQ(c.error().code,am::Errc::locked);
  EXPECT_LT(std::chrono::steady_clock::now()-started,std::chrono::seconds(5)); // hypothesis: test guard
  auto options=writable(); options.mode=am::library::Mode::read_only;
  EXPECT_TRUE(am::catalog::Catalog::open(w.root,options));
  EXPECT_EQ(hash(w.zip()),zip);
}
