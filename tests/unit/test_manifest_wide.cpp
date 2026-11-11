#include <gtest/gtest.h>
#include "am/domain/manifest/codec.hpp"
#include <chrono>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using namespace am::manifest;
using Json = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;
constexpr std::size_t members = 200000;  // Regression input size, not a product limit.
double elapsed(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}
std::string wide_input(bool opaque) {
  std::string text = opaque ? "{\"extensions\":{" : "{\"known\":{";
  for (std::size_t i = 0; i < members; ++i) {
    if (i) text += ',';
    text += opaque ? "\"org.example.e" : "\"k";
    text += std::to_string(i);
    text += opaque ? "\":{\"data\":null}" : "\":null";
  }
  text += "}}";
  return text;
}
void measurements(std::string_view mode, std::size_t bytes, double parse_ms,
                  double write_ms, double verify_ms, double reparse_ms,
                  double equality_ms, double total_ms) {
  ::testing::Test::RecordProperty("parse_ms", std::to_string(parse_ms));
  ::testing::Test::RecordProperty("total_ms", std::to_string(total_ms));
  std::cout << "Wide object: mode=" << mode << " members=" << members << " bytes=" << bytes
            << " parse_ms=" << parse_ms << " write_ms=" << write_ms << " verify_ms=" << verify_ms
            << " reparse_ms=" << reparse_ms << " equality_ms=" << equality_ms << " total_ms=" << total_ms;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  ASSERT_TRUE(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)));
  ::testing::Test::RecordProperty("peak_working_set", std::to_string(counters.PeakWorkingSetSize));
  ::testing::Test::RecordProperty("peak_pagefile", std::to_string(counters.PeakPagefileUsage));
  std::cout << " peak_working_set=" << counters.PeakWorkingSetSize << " peak_pagefile=" << counters.PeakPagefileUsage;
#endif
  std::cout << '\n';
}
TEST(ManifestCodec, WideExtensionsRoundTripWithoutObjectKeySearch) {
  RecordProperty("req", "ADR-0038|R-09.8");
  const auto text = wide_input(true);
  const auto start = Clock::now();
  auto doc = parse_manifest(text);
  ASSERT_TRUE(doc) << am::to_log_string(doc.error());
  const auto parsed = Clock::now();
  const auto& object = doc->known().at("extensions").get_ref<const Json::object_t&>();
  ASSERT_EQ(object.size(), members);
  EXPECT_EQ(object.front().first, "org.example.e0");
  EXPECT_EQ(object.back().first, "org.example.e199999");
  auto output = write_manifest(*doc);
  ASSERT_TRUE(output) << am::to_log_string(output.error());
  const auto written = Clock::now();
  auto preserved = verify_preserved(text, *output);
  ASSERT_TRUE(preserved) << am::to_log_string(preserved.error());
  const auto verified = Clock::now();
  auto again = parse_manifest(*output);
  ASSERT_TRUE(again) << am::to_log_string(again.error());
  const auto reparsed = Clock::now();
  EXPECT_EQ(doc->known(), again->known());
  const auto compared = Clock::now();
  measurements("opaque", text.size(), elapsed(start, parsed), elapsed(parsed, written),
               elapsed(written, verified), elapsed(verified, reparsed), elapsed(reparsed, compared), elapsed(start, compared));
}
TEST(ManifestCodec, WideKnownObjectV1WithoutObjectKeySearch) {
  RecordProperty("req", "ADR-0038");
  const auto text = wide_input(false);
  const auto start = Clock::now();
  auto doc = parse_manifest(text);
  ASSERT_TRUE(doc) << am::to_log_string(doc.error());
  const auto parsed = Clock::now();
  const auto& object = doc->known().at("known").get_ref<const Json::object_t&>();
  ASSERT_EQ(object.size(), members);
  EXPECT_EQ(object.front().first, "k0");
  EXPECT_EQ(object.back().first, "k199999");
  EXPECT_TRUE(object.front().second.is_null());
  EXPECT_TRUE(object.back().second.is_null());
  measurements("known", text.size(), elapsed(start, parsed), 0, 0, 0, 0, elapsed(start, parsed));
}
}  // namespace
