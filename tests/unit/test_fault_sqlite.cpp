#include <gtest/gtest.h>
#include <filesystem>
#include "am/foundation/uuid.hpp"
#include "am/storage/library/library.hpp"
#include "am/storage/db/sqlite.hpp"
#include "fault_vfs.hpp"

namespace {
namespace fs = std::filesystem;
template<class T> T take(am::Result<T> r) { if (!r) throw std::runtime_error(am::to_log_string(r.error())); return std::move(*r); }
void take(am::Result<void> r) { if (!r) throw std::runtime_error(am::to_log_string(r.error())); }
fs::path fresh() {
  auto root=fs::path(AM_FAULT_TEST_ROOT)/am::Uuid::v4().to_string();
  fs::create_directories(root.parent_path());
  return root;
}
void space(const am::Error& e) {
  EXPECT_EQ(e.code,am::Errc::insufficient_space) << am::to_log_string(e);
  EXPECT_EQ(e.message_id,"am.db.insufficient_space");
  EXPECT_TRUE(e.allows(am::ErrorAction::free_space));
  EXPECT_TRUE(e.allows(am::ErrorAction::abort));
}
}

TEST(FaultSqlite, AllThreeDatabasesRollbackRealSqliteFull) {
  RecordProperty("req","K-117|K-122|ADR-0007|ADR-0033");
  for (const auto* name : {"index","userdata","state"}) {
    SCOPED_TRACE(name);
    fault_test::FullVfs vfs(std::string(name)+".sqlite");
    const auto root=fresh();
    take(am::library::Library::init(root));
    auto db=take(am::db::Connection::open(root/".am"/(std::string(name)+".sqlite")));
    take(db.exec("PRAGMA journal_mode=WAL; CREATE TABLE fault_probe(value TEXT); INSERT INTO fault_probe VALUES('confirmed')"));
    {
      auto tx=take(am::db::Transaction::begin(db));
      take(db.exec("INSERT INTO fault_probe VALUES('unconfirmed')"));
      vfs.arm();
      const auto committed=tx.commit();
      ASSERT_FALSE(committed);
      space(committed.error());
      EXPECT_GT(vfs.failures(),0);
      vfs.disarm();
    }
    auto query=take(db.prepare("SELECT value FROM fault_probe"));
    ASSERT_EQ(take(query.step()),am::db::Step::row);
    EXPECT_EQ(take(query.text(0)),"confirmed");
    EXPECT_EQ(take(query.step()),am::db::Step::done);
  }
}

TEST(FaultSqlite, InitFullNeverPublishesConfiguration) {
  RecordProperty("req","K-117|K-122|ADR-0007");
  for (const auto* name : {"index","userdata","state"}) {
    SCOPED_TRACE(name);
    fault_test::FullVfs vfs(std::string(name)+".sqlite");
    const auto root=fresh();
    vfs.arm();
    auto created=am::library::Library::init(root);
    ASSERT_FALSE(created);
    space(created.error());
    EXPECT_GT(vfs.failures(),0);
    EXPECT_FALSE(fs::exists(root/"am-library.toml"));
    vfs.disarm();
  }
}

TEST(FaultSqlite, SpaceClassifierDoesNotRelabelOtherFailures) {
  RecordProperty("req","K-117|ADR-0033");
  for (const auto code : {SQLITE_BUSY,SQLITE_LOCKED,SQLITE_CORRUPT,SQLITE_READONLY,SQLITE_NOMEM,SQLITE_IOERR_READ})
    EXPECT_FALSE(am::db::insufficient_space_error(nullptr,code));
  ASSERT_TRUE(am::db::insufficient_space_error(nullptr,SQLITE_FULL));
  space(*am::db::insufficient_space_error(nullptr,SQLITE_FULL));
}
