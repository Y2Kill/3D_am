#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "am/processing/formats/stl_probe.hpp"

using namespace am::formats;

namespace {

// Unit cube, 12 outward-facing triangles.
const float kCube[12][9] = {
    {0, 0, 0, 0, 1, 0, 1, 1, 0}, {0, 0, 0, 1, 1, 0, 1, 0, 0},  // z=0 (normal -z)
    {0, 0, 1, 1, 0, 1, 1, 1, 1}, {0, 0, 1, 1, 1, 1, 0, 1, 1},  // z=1
    {0, 0, 0, 1, 0, 0, 1, 0, 1}, {0, 0, 0, 1, 0, 1, 0, 0, 1},  // y=0
    {0, 1, 0, 0, 1, 1, 1, 1, 1}, {0, 1, 0, 1, 1, 1, 1, 1, 0},  // y=1
    {0, 0, 0, 0, 0, 1, 0, 1, 1}, {0, 0, 0, 0, 1, 1, 0, 1, 0},  // x=0
    {1, 0, 0, 1, 1, 0, 1, 1, 1}, {1, 0, 0, 1, 1, 1, 1, 0, 1},  // x=1
};

std::vector<std::uint8_t> binary_cube(std::string_view header) {
  std::vector<std::uint8_t> d(84 + 50 * 12, 0);
  std::memcpy(d.data(), header.data(), std::min<std::size_t>(header.size(), 80));
  std::uint32_t n = 12;
  std::memcpy(d.data() + 80, &n, 4);
  for (int t = 0; t < 12; ++t) std::memcpy(d.data() + 84 + t * 50 + 12, kCube[t], 36);
  return d;
}

std::string ascii_cube() {
  std::string s = "solid cube_test\n";
  for (auto& t : kCube) {
    s += "  facet normal 0 0 0\n    outer loop\n";
    for (int k = 0; k < 3; ++k)
      s += "      vertex " + std::to_string(t[k * 3]) + " " + std::to_string(t[k * 3 + 1]) + " " +
           std::to_string(t[k * 3 + 2]) + "\n";
    s += "    endloop\n  endfacet\n";
  }
  s += "endsolid cube_test\n";
  return s;
}

// Delivers data in small, odd-sized chunks to exercise buffer boundaries.
ReadFn chunked(const std::vector<std::uint8_t>& data, std::size_t chunk = 7) {
  auto pos = std::make_shared<std::size_t>(0);
  return [&data, pos, chunk](std::span<std::uint8_t> b) -> std::size_t {
    std::size_t n = std::min({b.size(), chunk, data.size() - *pos});
    std::memcpy(b.data(), data.data() + *pos, n);
    *pos += n;
    return n;
  };
}

}  // namespace

TEST(StlProbe, BinaryCube) {
  auto d = binary_cube("solid but actually binary");
  auto r = probe_stl(chunked(d), d.size());
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_TRUE(r->binary);
  EXPECT_EQ(r->triangles, 12u);
  EXPECT_NEAR(r->area, 6.0, 1e-6);
  EXPECT_NEAR(std::abs(r->volume), 1.0, 1e-6);
  EXPECT_FLOAT_EQ(r->max[0] - r->min[0], 1.0f);
  EXPECT_EQ(r->header, "solid but actually binary");
}

TEST(StlProbe, AsciiCube) {
  auto s = ascii_cube();
  std::vector<std::uint8_t> d(s.begin(), s.end());
  auto r = probe_stl(chunked(d, 13), d.size());
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_FALSE(r->binary);
  EXPECT_EQ(r->triangles, 12u);
  EXPECT_NEAR(r->area, 6.0, 1e-5);
  EXPECT_NEAR(std::abs(r->volume), 1.0, 1e-5);
  EXPECT_EQ(r->header, "cube_test");
}

TEST(StlProbe, TruncatedBinaryIsCorrupt) {
  auto d = binary_cube("x");
  d.resize(d.size() - 50);  // one triangle missing; declared count stays 12
  auto r = probe_stl(chunked(d), std::nullopt);
  EXPECT_FALSE(r.ok());
}
