#include <gtest/gtest.h>
#include "am/storage/db/commit_journal.hpp"
#include "am/foundation/uuid.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <set>

using namespace am::db;
namespace {
std::filesystem::path fresh() { auto p = std::filesystem::path(AM_COMMIT_TEST_ROOT) / ("journal-" + am::Uuid::v4().to_string()); std::filesystem::create_directories(p); return p; }
PendingOperation row(const std::filesystem::path& root) {
  PendingOperation p; p.op_id = am::Uuid::v4().to_string(); p.asset_id = am::Uuid::v4().to_string(); p.kind = "update";
  p.target_path = root / "asset.zip"; p.temp_path = root / ".am/.am-tmp" / (p.asset_id + "." + p.op_id + ".zip");
  p.base = BaseSnapshot{123, 987654321, std::string(64, 'a'), 7}; p.expected_rev = 7; p.job_id = "job-42"; return p;
}
struct RawDb {
  sqlite3* db = nullptr;
  explicit RawDb(const std::filesystem::path& p) { if (sqlite3_open((p / ".am/state.sqlite").string().c_str(), &db) != SQLITE_OK) throw std::runtime_error("Open test SQLite failed"); }
  ~RawDb() { sqlite3_close(db); }
  int integer(const char* sql) { sqlite3_stmt* s = nullptr; if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db)); const auto code = sqlite3_step(s); const auto result = code == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1; sqlite3_finalize(s); return result; }
};
}
TEST(StateJournal, SchemaVersionTablesAndDurableTransitionsAcrossConnections) {
  RecordProperty("req", "ADR-0006");
  const auto root = fresh(); auto journal = StateJournal::open(root); ASSERT_TRUE(journal) << journal.error().ctx.detail;
  RawDb raw(root); EXPECT_EQ(raw.integer("PRAGMA user_version"), 1);
  for (const auto table : {"pending_ops", "snapshots", "jobs", "op_log", "edit_sessions", "edit_session_files", "pending_ops_new"}) {
    const auto sql = "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='" + std::string(table) + "'"; EXPECT_EQ(raw.integer(sql.c_str()), 1) << table;
  }
  auto p = row(root); ASSERT_TRUE((*journal)->insert(p)); auto another = StateJournal::open(root); ASSERT_TRUE(another);
  auto found = (*another)->find(p.op_id); ASSERT_TRUE(found); ASSERT_TRUE(found->has_value()); EXPECT_EQ((**found).base->size, 123u); EXPECT_EQ((**found).base->mtime_ns, 987654321);
  EXPECT_EQ((**found).job_id, p.job_id); EXPECT_EQ((**found).expected_rev, p.expected_rev); EXPECT_TRUE((**found).started_at.ends_with('Z'));
  NewFileEvidence evidence{321, std::string(64, 'b'), std::string(64, 'c')};
  ASSERT_TRUE((*journal)->advance(p.op_id, CommitState::prepared, CommitState::verified, evidence));
  found = (*another)->find(p.op_id); ASSERT_TRUE(found); ASSERT_TRUE(found->has_value()); EXPECT_EQ((**found).state, CommitState::verified); ASSERT_TRUE((**found).verified);
  EXPECT_EQ((**found).verified->file_sha256, evidence.file_sha256); EXPECT_EQ((**found).verified->manifest_sha256, evidence.manifest_sha256);
  EXPECT_EQ(raw.integer("SELECT count(*) FROM pending_ops_new"), 1); EXPECT_EQ(raw.integer("SELECT count(*) FROM pending_ops WHERE backup_path IS NOT NULL"), 0);
  ASSERT_TRUE((*journal)->advance(p.op_id, CommitState::verified, CommitState::committed)); ASSERT_TRUE((*journal)->advance(p.op_id, CommitState::committed, CommitState::indexed));
  found = (*another)->find(p.op_id); ASSERT_TRUE(found); EXPECT_EQ((**found).state, CommitState::indexed);
  ASSERT_TRUE((*journal)->remove(p.op_id)); auto all = (*another)->list(); ASSERT_TRUE(all); EXPECT_TRUE(all->empty()); EXPECT_EQ(raw.integer("SELECT count(*) FROM pending_ops_new"), 0);
}
TEST(StateJournal, InvalidTransitionAndVersionDoNotModifyRows) {
  RecordProperty("req", "ADR-0006");
  const auto root = fresh(); auto journal = StateJournal::open(root); ASSERT_TRUE(journal); auto p = row(root); ASSERT_TRUE((*journal)->insert(p));
  for (const auto& states : {std::pair{CommitState::prepared, CommitState::committed}, std::pair{CommitState::verified, CommitState::committed}, std::pair{CommitState::prepared, CommitState::verified}}) {
    auto bad = (*journal)->advance(p.op_id, states.first, states.second); ASSERT_FALSE(bad); EXPECT_EQ(bad.error().code, am::Errc::conflict); EXPECT_EQ(bad.error().message_id, "am.commit.journal_error");
  }
  auto found = (*journal)->find(p.op_id); ASSERT_TRUE(found); EXPECT_EQ((**found).state, CommitState::prepared);
  RawDb raw(root); ASSERT_EQ(sqlite3_exec(raw.db, "PRAGMA user_version=2", nullptr, nullptr, nullptr), SQLITE_OK);
  auto unsupported = StateJournal::open(root); ASSERT_FALSE(unsupported); EXPECT_EQ(unsupported.error().message_id, "am.commit.journal_version"); EXPECT_EQ(unsupported.error().code, am::Errc::unsupported);
  EXPECT_EQ(raw.integer("PRAGMA user_version"), 2);
}
TEST(StateJournal, EvidenceFailureRollsBackTheStateTransition) {
  RecordProperty("req", "ADR-0006");
  const auto root = fresh(); auto journal = StateJournal::open(root); ASSERT_TRUE(journal); auto p = row(root); ASSERT_TRUE((*journal)->insert(p));
  RawDb raw(root);
  ASSERT_EQ(sqlite3_exec(raw.db, "CREATE TRIGGER reject_evidence BEFORE INSERT ON pending_ops_new BEGIN SELECT RAISE(ABORT,'injected evidence failure'); END", nullptr, nullptr, nullptr), SQLITE_OK);
  NewFileEvidence evidence{321, std::string(64, 'b'), std::string(64, 'c')};
  auto failed = (*journal)->advance(p.op_id, CommitState::prepared, CommitState::verified, evidence);
  ASSERT_FALSE(failed); EXPECT_EQ(failed.error().code, am::Errc::io_error); EXPECT_EQ(failed.error().message_id, "am.commit.journal_error");
  EXPECT_NE(failed.error().ctx.detail.find("injected evidence failure"), std::string::npos);
  auto another = StateJournal::open(root); ASSERT_TRUE(another); auto found = (*another)->find(p.op_id); ASSERT_TRUE(found); ASSERT_TRUE(found->has_value());
  EXPECT_EQ((**found).state, CommitState::prepared); EXPECT_FALSE((**found).verified); EXPECT_EQ(raw.integer("SELECT count(*) FROM pending_ops_new"), 0);
}
