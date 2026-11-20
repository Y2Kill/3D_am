#include <gtest/gtest.h>
#include "am/foundation/uuid.hpp"
#include "plan.hpp"

namespace {
using namespace perf;
TEST(ScalePlan, PrefixStableAndPayloadsUnique) {
  RecordProperty("req","K-112");
  Parameters p; std::set<std::string> hashes;
  auto first = take(am::asset_zip::Sha256::create());
  for (std::uint64_t i = 0; i < 100; ++i) {
    const auto a = asset(p,i); update(first,a.canonical().dump()+"\n");
    for (const auto& f : a.files) EXPECT_TRUE(hashes.insert(sha(f.bytes)).second);
    auto extended = p; extended.count = 101;
    EXPECT_EQ(a.canonical(),asset(extended,i).canonical());
  }
  // The same constant is checked by the process smoke on Windows and Linux.
  EXPECT_EQ(digest(first),"ff6dad951f904b063f1388ad1fa39b3c0764bfb75ec26af3b4ddf6ab5f23f767");
}

TEST(ScaleOpen, TwoHundredAssetsCleanOpenAndListReadNoZip) {
  RecordProperty("req","K-112");
  const auto root = fs::path(AM_PERF_TEST_ROOT)/am::Uuid::v4().to_string();
  fs::create_directories(root.parent_path()); take(am::library::Library::init(root));
  Parameters p; p.count = 200; p.trash_ppm = 0;
  {
    auto c = take(am::catalog::Catalog::open(root,options(true))); take(c.indexer().rebuild());
    for (std::uint64_t i = 0; i < p.count; ++i) create(c,asset(p,i),root.parent_path()/(root.filename().string()+".sources"));
  }
  std::vector<fs::path> reads;
  am::catalog::IndexerHooks hooks;
  hooks.before_read = [&](const fs::path& path) -> am::Result<void> { reads.push_back(path); return {}; };
  for (const auto mode : {am::library::Mode::read_write,am::library::Mode::read_only}) {
    auto c = take(am::catalog::Catalog::open(root,options(true,mode),hooks));
    EXPECT_EQ(take(c.indexer().list()).size(),200u); EXPECT_TRUE(reads.empty());
  }
  const auto id = asset(p,42).id;
  const auto changed = root/"assets"/id.substr(34)/(id+".zip");
  fs::last_write_time(changed,fs::last_write_time(changed)+std::chrono::seconds(2));
  {
    auto c = take(am::catalog::Catalog::open(root,options(true),hooks));
    EXPECT_EQ(take(c.indexer().list()).size(),200u); EXPECT_TRUE(reads.empty());
  }
  // Owner decision V-1: stat-sync on open requires an unclosed writer session.
  {
    auto library = take(am::library::Library::open(root,options(true)));
    am::db::IndexStore store(library.database(am::library::DatabaseKind::index)); take(store.set_meta("writer_session","open"));
  }
  {
    auto c = take(am::catalog::Catalog::open(root,options(true),hooks));
    EXPECT_TRUE(c.open_report().previous_session_unclosed);
    EXPECT_EQ(take(c.indexer().list()).size(),200u);
    ASSERT_EQ(reads.size(),1u); EXPECT_TRUE(fs::equivalent(reads.front(),changed));
  }
}
} // namespace
