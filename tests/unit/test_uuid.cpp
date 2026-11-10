#include <gtest/gtest.h>

#include <set>
#include <regex>
#include <vector>

#include "am/foundation/uuid.hpp"

using am::Uuid;

TEST(Uuid, V4UniqueWithCanonicalVersionAndVariant) {
  RecordProperty("req", "ADR-0035");
  const std::regex pattern("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  std::set<Uuid> ids;
  for (int i = 0; i < 10'000; ++i) {
    const auto u = Uuid::v4();
    EXPECT_TRUE(ids.insert(u).second) << i;
    EXPECT_EQ(u.version(), 4);
    EXPECT_EQ(u.bytes()[8] & 0xC0, 0x80);
    EXPECT_TRUE(std::regex_match(u.to_string(), pattern));
    EXPECT_EQ(Uuid::parse_canonical(u.to_string()), u);
  }
  EXPECT_EQ(ids.size(), 10'000u);
}

TEST(Uuid, ParseCanonicalAcceptsLowercaseV4AndV7) {
  RecordProperty("req", "ADR-0035");
  for (const auto text : {"debdf916-b47c-41db-b4fd-7aa9511396f3", "019a5d2e-7c41-7b3e-9a10-4f2c8e1d0a77"}) {
    const auto u = Uuid::parse_canonical(text);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->to_string(), text);
    EXPECT_EQ(u, Uuid::parse(text));
  }
  const auto u = Uuid::v7();
  EXPECT_EQ(Uuid::parse_canonical(u.to_string()), u);
}

TEST(Uuid, ParseCanonicalRejectsNonCanonicalText) {
  RecordProperty("req", "ADR-0035");
  for (const auto text : {"DEBDF916-B47C-41DB-B4FD-7AA9511396F3", "debdf916b47c41dbb4fd7aa9511396f3",
                          "debdf916-b47c-41db-b4fd-7aa9511396f3x", "", " debdf916-b47c-41db-b4fd-7aa9511396f3",
                          "debdf916-b47c-41db-b4fd-7aa9511396fg", "debdf916-b47c-41db-b4fd-7aa9511396f",
                          "debdf916-b47c-41db-b4fd--aa9511396f3", "debdf916_b47c-41db-b4fd-7aa9511396f3"}) {
    EXPECT_FALSE(Uuid::parse_canonical(text).has_value()) << text;
  }
}

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
