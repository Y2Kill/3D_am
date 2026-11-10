// Structured error model (TZ §14.4.2, ADR-0032).
#include <gtest/gtest.h>

#include <set>
#include <string>

#include "am/foundation/result.hpp"

using am::Errc;
using am::Error;
using am::ErrorAction;

// Codes are a stable external contract: values must never change.
static_assert(static_cast<int>(Errc::ok) == 0);
static_assert(static_cast<int>(Errc::internal) == 10);
static_assert(static_cast<int>(Errc::resource_exhausted) == 11);
static_assert(static_cast<int>(Errc::insufficient_space) == 12);
static_assert(static_cast<int>(Errc::read_only) == 13);
static_assert(static_cast<int>(Errc::locked) == 14);
static_assert(static_cast<int>(Errc::incomplete) == 15);

TEST(Error, CodeNamesAreUniqueAndStable) {
  ::testing::Test::RecordProperty("req", "NFR-14.4.2|ADR-0035");
  std::set<std::string> names;
  for (int i = 0; i <= static_cast<int>(Errc::incomplete); ++i) {
    auto n = std::string(am::errc_name(static_cast<Errc>(i)));
    EXPECT_EQ(n.rfind("am.", 0), 0u) << n;
    EXPECT_NE(n, "am.unknown") << "code " << i << " has no name";
    names.insert(n);
  }
  EXPECT_EQ(names.size(), static_cast<size_t>(Errc::incomplete) + 1);
  EXPECT_EQ(am::errc_name(Errc::insufficient_space), "am.insufficient_space");
  EXPECT_EQ(am::errc_name(Errc::incomplete), "am.incomplete");
}

TEST(Error, LegacyMakeKeepsContext) {
  auto e = Error::make(Errc::io_error, "cannot open", "x.zip");
  EXPECT_EQ(e.context, "x.zip");
  EXPECT_EQ(e.ctx.detail, "x.zip");
  EXPECT_EQ(e.severity, am::Severity::error);
  EXPECT_TRUE(e.actions.empty());
}

TEST(Error, StructuredFieldsAndCauseChain) {
  ::testing::Test::RecordProperty("req", "NFR-14.4.2");
  auto inner = Error::make(Errc::io_error, "write failed", "tmp/x.zip.am-tmp").in("foundation.fs");
  auto e = Error::make(Errc::insufficient_space, "not enough space for commit")
               .in("storage")
               .op("asset.commit")
               .id("storage.commit.no_space")
               .for_asset("0190f0a0-0000-7000-8000-000000000001")
               .for_library("lib-1")
               .allow(ErrorAction::free_space)
               .allow(ErrorAction::abort)
               .caused_by(inner);

  EXPECT_EQ(e.component, "storage");
  EXPECT_EQ(e.ctx.operation, "asset.commit");
  EXPECT_EQ(e.message_id, "storage.commit.no_space");
  EXPECT_TRUE(e.allows(ErrorAction::free_space));
  EXPECT_FALSE(e.allows(ErrorAction::retry));
  EXPECT_EQ(e.root_cause().code, Errc::io_error);

  const auto line = am::to_log_string(e);
  EXPECT_NE(line.find("am.insufficient_space [storage] asset.commit"), std::string::npos) << line;
  EXPECT_NE(line.find("<- am.io_error [foundation.fs]"), std::string::npos) << line;

  // Error stays copyable and cheap: the cause is shared, not deep-copied.
  Error copy = e;
  EXPECT_EQ(copy.cause.get(), e.cause.get());
}

TEST(Error, ResultCarriesStructuredError) {
  am::Result<int> r = Error::make(Errc::locked, "library has another write owner").allow(ErrorAction::open_read_only);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, Errc::locked);
  EXPECT_TRUE(r.error().allows(ErrorAction::open_read_only));
}
