#include <gtest/gtest.h>

#include "am/foundation/uuid.hpp"
#include "am/storage/db/sqlite.hpp"

namespace {
template <class T>
T value(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(r).value();
}
void value(am::Result<void> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
}
am::db::Connection database() {
  auto root = std::filesystem::path(AM_LIBRARY_TEST_ROOT);
  std::filesystem::create_directories(root);
  return value(am::db::Connection::open(root / (am::Uuid::v7().to_string() + ".sqlite")));
}
}  // namespace
TEST(Sqlite, BoundValuesPreserveNulUnicodeIntegerRealAndNull) {
  RecordProperty("req", "ADR-0007");
  auto c = database();
  value(c.exec("CREATE TABLE probe(t TEXT,i INTEGER,r REAL,n TEXT)"));
  auto insert = value(c.prepare("INSERT INTO probe VALUES(?,?,?,?)"));
  const std::string text = std::string("a\0", 2) + "\xc3\xa9\xe6\xa8\xa1";
  value(insert.bind(1, std::string_view(text)));
  value(insert.bind(2, INT64_MIN));
  value(insert.bind(3, 1.25));
  value(insert.bind_null(4));
  EXPECT_EQ(value(insert.step()), am::db::Step::done);
  auto row = value(c.prepare("SELECT * FROM probe"));
  ASSERT_EQ(value(row.step()), am::db::Step::row);
  EXPECT_EQ(value(row.text(0)), text);
  EXPECT_EQ(row.integer(1), INT64_MIN);
  EXPECT_EQ(row.real(2), 1.25);
  EXPECT_TRUE(row.is_null(3));
  EXPECT_EQ(value(row.step()), am::db::Step::done);
}
TEST(Sqlite, TransactionRollbackCommitAndPrepareError) {
  RecordProperty("req", "ADR-0007");
  auto c = database();
  value(c.exec("CREATE TABLE probe(n)"));
  {
    auto tx = value(am::db::Transaction::begin(c));
    value(c.exec("INSERT INTO probe VALUES(7)"));
    auto moved = std::move(tx);
  }
  auto count = [&]() {
    auto s = value(c.prepare("SELECT count(*) FROM probe"));
    value(s.step());
    return s.integer(0);
  };
  EXPECT_EQ(count(), 0);
  {
    auto tx = value(am::db::Transaction::begin(c));
    value(c.exec("INSERT INTO probe VALUES(8)"));
    value(tx.commit());
  }
  EXPECT_EQ(count(), 1);
  auto bad = c.prepare("SELECT FROM");
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().message_id, "am.db.prepare");
  EXPECT_FALSE(c.prepare(""));
}
