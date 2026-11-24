#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>

#include "log_test_sink.hpp"

namespace {
namespace l = am::log;
class Log : public testing::Test {
  void TearDown() override { l::set_sink(nullptr); }
};
class LogRecord : public Log {};
std::string row(l::Record record, const char* name) {
  auto encoded = l::serialize(record);
  auto tree = log_tests::parse(encoded);
  (void)tree;
  const auto root = std::filesystem::path(AM_LOG_RECORD_ROOT);
  std::filesystem::create_directories(root);
  std::ofstream(root / (std::string(name) + ".json"), std::ios::binary) << encoded;
  return encoded;
}
}  // namespace
TEST_F(Log, DisabledDoesNotBuildFieldsOrCallSink) {
  RecordProperty("req", "NFR-14.4.9");
  auto sink = std::make_shared<log_tests::MemorySink>();
  l::set_sink(sink);
  l::set_sink(nullptr);
  int built = 0;
  auto build = [&] {
    ++built;
    return std::string("expensive field");
  };
  AM_LOG(info, "test", "log.test", "Disabled", {{"value", build()}});
  EXPECT_EQ(built, 0);
  EXPECT_EQ(sink->writes, 0u);
  EXPECT_FALSE(l::enabled(l::Level::critical));
  l::set_sink(sink);
  auto throwing = []() -> std::string { throw std::runtime_error("Injected field builder failure"); };
  EXPECT_NO_THROW(AM_LOG(info, "test", "log.test", "Builder failure", {{"value", throwing()}}));
  EXPECT_EQ(sink->writes, 0u);
}
TEST_F(Log, EnabledHonoursMinimumAndWriteAlsoFilters) {
  RecordProperty("req", "NFR-14.4.9");
  auto sink = std::make_shared<log_tests::MemorySink>();
  sink->minimum = l::Level::warning;
  l::set_sink(sink);
  for (unsigned n = 0; n < 6; ++n) {
    const auto level = static_cast<l::Level>(n);
    EXPECT_EQ(l::enabled(level), n >= 3);
    l::write(level, "test", "log.test", "Level");
  }
  EXPECT_EQ(sink->writes, 3u);
}
TEST_F(Log, CachedLevelDoesNotConsultTheSinkDuringEnabledChecks) {
  RecordProperty("req", "NFR-14.4.9");
  struct CountingSink final : l::Sink {
    mutable unsigned queries = 0;
    l::Level min_level() const noexcept override {
      ++queries;
      return l::Level::warning;
    }
    void write(const l::Record&) noexcept override {}
  };
  auto sink = std::make_shared<CountingSink>();
  l::set_sink(sink);
  const auto installed = sink->queries;
  for (unsigned i = 0; i < 1000; ++i) {
    EXPECT_FALSE(l::enabled(l::Level::info));
    EXPECT_TRUE(l::enabled(l::Level::warning));
  }
  EXPECT_EQ(sink->queries, installed);
  l::set_sink(nullptr);
  for (unsigned level = 0; level <= static_cast<unsigned>(l::Level::critical); ++level)
    EXPECT_FALSE(l::enabled(static_cast<l::Level>(level)));
}
TEST_F(Log, NestedScopesAndExplicitRecordValuesHavePriority) {
  RecordProperty("req", "NFR-14.4.9");
  auto sink = std::make_shared<log_tests::MemorySink>();
  l::set_sink(sink);
  EXPECT_FALSE(l::current_context().job_id);
  {
    l::Scope outer({"outer", {}, "library", {}});
    {
      l::Scope inner({"inner", "asset", {}, "operation"});
      l::write(l::Record{l::Level::info, "test", "log.test", "Context", {}, nullptr, {"explicit", {}, {}, {}}});
      EXPECT_EQ(l::current_context().job_id, "inner");
    }
    EXPECT_EQ(l::current_context().job_id, "outer");
  }
  EXPECT_FALSE(l::current_context().library_id);
  ASSERT_EQ(sink->rows.size(), 1u);
  EXPECT_EQ(log_tests::context(sink->rows[0], "job_id"), "explicit");
  EXPECT_EQ(log_tests::context(sink->rows[0], "library_id"), "library");
  EXPECT_EQ(log_tests::context(sink->rows[0], "asset_id"), "asset");
}
TEST_F(Log, ThreadLocalContextCanBeTransferredExplicitly) {
  RecordProperty("req", "NFR-14.4.9");
  auto sink = std::make_shared<log_tests::MemorySink>();
  l::set_sink(sink);
  l::Scope scope({"job", {}, "library", {}});
  const auto inherited = l::current_context();
  std::jthread worker([&] {
    EXPECT_FALSE(l::current_context().job_id);
    l::Scope transferred(inherited);
    AM_LOG(info, "test", "worker.test", "Worker");
  });
  worker.join();
  EXPECT_EQ(log_tests::context(sink->rows.front(), "job_id"), "job");
  EXPECT_NE(sink->threads.front(), std::this_thread::get_id());
}
TEST_F(Log, SinkReplacementDuringEightWritersPreservesEveryRecord) {
  RecordProperty("req", "NFR-14.4.9");
  auto a = std::make_shared<log_tests::CounterSink>(), b = std::make_shared<log_tests::CounterSink>();
  l::set_sink(a);
  std::barrier start(9);
  std::vector<std::jthread> workers;
  for (unsigned n = 0; n < 8; ++n)
    workers.emplace_back([&] {
      start.arrive_and_wait();
      for (unsigned i = 0; i < 1000; ++i) AM_LOG(info, "test", "concurrent.test", "Concurrent record");
    });
  start.arrive_and_wait();
  for (unsigned i = 0; i < 1000; ++i) l::set_sink((i & 1) ? a : b);
  for (auto& worker : workers) worker.join();
  EXPECT_EQ(a->count + b->count, 8000u);
}
TEST_F(Log, MeasuresDisabledAndInfoCostWithoutThresholds) {
  RecordProperty("req", "NFR-14.4.9");
  constexpr unsigned count = 200000;
  const auto begin = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < count; ++i) AM_LOG(info, "test", "cost.test", "Record");
  const auto disabled = std::chrono::steady_clock::now();
  auto sink = std::make_shared<log_tests::CounterSink>();
  l::set_sink(sink);
  const auto ready = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < count; ++i) AM_LOG(info, "test", "cost.test", "Record");
  const auto end = std::chrono::steady_clock::now();
  std::cout << "LOG_COST disabled_ns=" << std::chrono::duration<double, std::nano>(disabled - begin).count() / count
            << " memory_info_ns=" << std::chrono::duration<double, std::nano>(end - ready).count() / count << '\n';
  EXPECT_EQ(sink->count, count);
}
TEST_F(LogRecord, ExactNumbersKindsAndLastDuplicateWins) {
  RecordProperty("req", "NFR-14.4.9");
  const std::array<l::Field, 6> fields{{{"maximum", (std::numeric_limits<std::uint64_t>::max)()},
                                        {"minimum", (std::numeric_limits<std::int64_t>::min)()},
                                        {"path", std::string("C:\\private\\file"), l::FieldKind::path},
                                        {"name", std::string("file.stl"), l::FieldKind::name},
                                        {"duplicate", std::string("old"), l::FieldKind::path},
                                        {"duplicate", std::string("last")}}};
  const auto encoded = row({l::Level::info, "test", "record.test", "Numbers", fields, nullptr, {}}, "exact");
  EXPECT_NE(encoded.find("18446744073709551615"), std::string::npos);
  EXPECT_NE(encoded.find("-9223372036854775808"), std::string::npos);
  const auto tree = log_tests::parse(encoded);
  const auto& values = log_tests::field(tree, tree.nodes[0], "fields");
  EXPECT_EQ(log_tests::field(tree, values, "duplicate").decoded, "last");
  const auto& kinds = log_tests::field(tree, tree.nodes[0], "kinds");
  EXPECT_EQ(kinds.children.size(), 2u);
}
TEST_F(LogRecord, NonfiniteDoublesAreStringsAndFiniteValuesRemainNumbers) {
  RecordProperty("req", "NFR-14.4.9");
  const std::array<l::Field, 4> fields{{{"nan", std::numeric_limits<double>::quiet_NaN()},
                                        {"positive", std::numeric_limits<double>::infinity()},
                                        {"negative", -std::numeric_limits<double>::infinity()},
                                        {"finite", 12.5}}};
  const auto encoded = row({l::Level::info, "test", "record.test", "Double", fields, nullptr, {}}, "double");
  const auto tree = log_tests::parse(encoded);
  const auto& values = log_tests::field(tree, tree.nodes[0], "fields");
  EXPECT_EQ(log_tests::field(tree, values, "nan").decoded, "NaN");
  EXPECT_EQ(log_tests::field(tree, values, "positive").decoded, "Infinity");
  EXPECT_EQ(log_tests::field(tree, values, "negative").decoded, "-Infinity");
  EXPECT_EQ(log_tests::field(tree, values, "finite").kind, am::json::Kind::number);
}
TEST_F(LogRecord, RepairsUtf8EverywhereAndEscapesControls) {
  RecordProperty("req", "NFR-14.4.9");
  const std::string bad = "invalid\xff\n\"\\";
  const std::array<l::Field, 1> fields{{{"value", bad}}};
  auto error = am::Error::make(am::Errc::io_error, bad, bad).in(bad).id(bad);
  const auto encoded = row({l::Level::warning, bad, bad, bad, fields, &error, {bad, bad, bad, bad}}, "utf8");
  EXPECT_NE(encoded.find("\xef\xbf\xbd"), std::string::npos);
  EXPECT_NE(encoded.find("\"utf8_repaired\":true"), std::string::npos);
  EXPECT_EQ(std::count(encoded.begin(), encoded.end(), '\n'), 1);
}
TEST_F(LogRecord, TextAndRecordLimitsRemainValidJson) {
  RecordProperty("req", "NFR-14.4.9");
  const std::string text(6000, 'x');
  const std::array<l::Field, 1> field{{{"text", text}}};
  auto encoded = row({l::Level::info, "test", "record.test", text, field, nullptr, {}}, "truncated");
  auto tree = log_tests::parse(encoded);
  const auto& message = log_tests::field(tree, tree.nodes[0], "msg");
  EXPECT_LE(message.decoded.size(), 4096u);
  EXPECT_TRUE(message.decoded.ends_with("\xe2\x80\xa6"));
  std::vector<l::Field> fields;
  for (unsigned i = 0; i < 100; ++i) fields.push_back({"value_" + std::to_string(i), text});
  encoded = row({l::Level::info, "test", "record.test", text, fields, nullptr, {}}, "budget");
  EXPECT_LE(encoded.size(), 65536u);
  EXPECT_NE(encoded.find("\"truncated\":true"), std::string::npos);
}
TEST_F(LogRecord, CauseChainStopsAtSixteenAndInvalidKeysAreDropped) {
  RecordProperty("req", "NFR-14.4.9");
  auto error = am::Error::make(am::Errc::io_error, "Root").allow(am::ErrorAction::retry);
  for (unsigned i = 0; i < 21; ++i) error = am::Error::make(am::Errc::internal, "Outer").caused_by(error);
  const std::array<l::Field, 2> invalid{{{"Invalid", std::string("value")}, {"invalid\xff", std::string("value")}}};
  const auto encoded = row({l::Level::error, "test", "record.test", "Error", invalid, &error, {}}, "cause");
  const auto tree = log_tests::parse(encoded);
  const auto& err = log_tests::field(tree, tree.nodes[0], "error");
  EXPECT_EQ(log_tests::field(tree, err, "cause").children.size(), 16u);
  EXPECT_EQ(log_tests::field(tree, err, "cause_truncated").kind, am::json::Kind::literal);
  EXPECT_TRUE(log_tests::field(tree, tree.nodes[0], "fields").children.empty());
  EXPECT_NE(encoded.find("\"utf8_repaired\":true"), std::string::npos);
}
