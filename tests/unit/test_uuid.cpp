#include <gtest/gtest.h>

#include <set>

#include "am/foundation/uuid.hpp"

using am::Uuid;

TEST(Uuid, V7HasVersionAndVariant) {
  auto u = Uuid::v7();
  EXPECT_EQ(u.version(), 7);
  EXPECT_EQ(u.bytes()[8] & 0xC0, 0x80);  // RFC 9562 variant 10xx
  EXPECT_FALSE(u.is_nil());
}

TEST(Uuid, RoundTripText) {
  auto u = Uuid::v7();
  auto s = u.to_string();
  ASSERT_EQ(s.size(), 36u);
  auto p = Uuid::parse(s);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(*p, u);
  EXPECT_FALSE(Uuid::parse("not-a-uuid").has_value());
  EXPECT_TRUE(Uuid::parse("019A5D2E-7C41-7B3E-9A10-4F2C8E1D0A77").has_value());  // upper case accepted
}

TEST(Uuid, MonotonicWithinSameMillisecond) {
  const auto tp = std::chrono::system_clock::now() + std::chrono::hours(1);  // isolate from other tests
  std::vector<Uuid> ids;
  for (int i = 0; i < 2000; ++i) ids.push_back(Uuid::v7_at(tp));
  for (std::size_t i = 1; i < ids.size(); ++i) EXPECT_LT(ids[i - 1], ids[i]) << i;
  std::set<Uuid> uniq(ids.begin(), ids.end());
  EXPECT_EQ(uniq.size(), ids.size());
}

TEST(Uuid, TimestampAndShard) {
  const auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(1'800'000'000'000));
  auto u = Uuid::v7_at(tp + std::chrono::hours(48));
  EXPECT_EQ(u.unix_ms(), 1'800'000'000'000ull + 48ull * 3600 * 1000);
  EXPECT_EQ(u.shard(), u.to_string().substr(34));
}
