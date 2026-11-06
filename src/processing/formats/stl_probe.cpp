#include "am/processing/formats/stl_probe.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

#include "am/foundation/paths.hpp"

namespace am::formats {
namespace {

struct Accumulator {
  StlStats& s;
  bool first = true;

  void triangle(const float* v) {  // v: 9 floats (3 vertices)
    for (int k = 0; k < 3; ++k) {
      for (int a = 0; a < 3; ++a) {
        float x = v[k * 3 + a];
        if (first) { s.min[a] = s.max[a] = x; }
        else { s.min[a] = std::min(s.min[a], x); s.max[a] = std::max(s.max[a], x); }
      }
      first = false;
    }
    const double ax = v[3] - v[0], ay = v[4] - v[1], az = v[5] - v[2];
    const double bx = v[6] - v[0], by = v[7] - v[1], bz = v[8] - v[2];
    const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
    const double twice_area = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (twice_area < 1e-12) ++s.degenerate;
    s.area += 0.5 * twice_area;
    // Signed tetra volume against origin (divergence theorem).
    s.volume += (double(v[0]) * (double(v[4]) * v[8] - double(v[5]) * v[7]) -
                 double(v[1]) * (double(v[3]) * v[8] - double(v[5]) * v[6]) +
                 double(v[2]) * (double(v[3]) * v[7] - double(v[4]) * v[6])) / 6.0;
    ++s.triangles;
  }
};

std::string sanitize_header(const char* p, std::size_t n) {
  std::string h;
  for (std::size_t i = 0; i < n && p[i] != '\0'; ++i) {
    auto c = static_cast<unsigned char>(p[i]);
    h.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
  }
  while (!h.empty() && h.back() == ' ') h.pop_back();
  return h;
}

Result<StlStats> probe_binary(const ReadFn& read, std::vector<std::uint8_t>& pending, StlStats s) {
  // pending holds already-read bytes starting at file offset 0.
  std::uint32_t declared = 0;
  std::memcpy(&declared, pending.data() + 80, 4);
  s.header = sanitize_header(reinterpret_cast<const char*>(pending.data()), 80);
  Accumulator acc{s};
  std::vector<std::uint8_t> buf(1 << 20);
  std::size_t have = pending.size() - 84;
  std::memmove(buf.data(), pending.data() + 84, have);
  float v[9];
  for (;;) {
    std::size_t off = 0;
    while (have - off >= 50) {
      std::memcpy(v, buf.data() + off + 12, sizeof v);  // skip normal (12 bytes)
      acc.triangle(v);
      off += 50;
    }
    std::memmove(buf.data(), buf.data() + off, have - off);
    have -= off;
    std::size_t n = read({buf.data() + have, buf.size() - have});
    if (n == 0) break;
    have += n;
  }
  if (s.triangles != declared)
    return Error::make(Errc::corrupt_data, "binary STL: declared " + std::to_string(declared) +
                                                " triangles, found " + std::to_string(s.triangles));
  return s;
}

Result<StlStats> probe_ascii(const ReadFn& read, std::vector<std::uint8_t>& pending, StlStats s) {
  s.binary = false;
  Accumulator acc{s};
  std::string carry(pending.begin(), pending.end());
  std::vector<std::uint8_t> buf(1 << 20);
  float v[9];
  int nv = 0;
  bool name_done = false;
  auto process_line = [&](std::string_view line) {
    auto b = line.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) return;
    line.remove_prefix(b);
    if (!name_done && line.rfind("solid", 0) == 0) {
      s.header = sanitize_header(line.data() + 5, std::min<std::size_t>(line.size() - 5, 80));
      if (!s.header.empty() && s.header.front() == ' ') s.header.erase(0, s.header.find_first_not_of(' '));
      name_done = true;
      return;
    }
    if (line.rfind("vertex", 0) != 0) return;
    const char* p = line.data() + 6;
    const char* end = line.data() + line.size();
    for (int a = 0; a < 3; ++a) {
      while (p < end && (*p == ' ' || *p == '\t')) ++p;
      float x = 0;
      auto r = std::from_chars(p, end, x);
      if (r.ec != std::errc{}) return;
      p = r.ptr;
      v[nv * 3 + a] = x;
    }
    if (++nv == 3) { acc.triangle(v); nv = 0; }
  };
  for (;;) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < carry.size(); ++i) {
      if (carry[i] == '\n') {
        process_line(std::string_view(carry).substr(start, i - start));
        start = i + 1;
      }
    }
    carry.erase(0, start);
    std::size_t n = read({buf.data(), buf.size()});
    if (n == 0) break;
    carry.append(reinterpret_cast<const char*>(buf.data()), n);
  }
  if (!carry.empty()) process_line(carry);
  if (s.triangles == 0) return Error::make(Errc::corrupt_data, "ASCII STL without facets");
  return s;
}

}  // namespace

Result<StlStats> probe_stl(const ReadFn& read, std::optional<std::uint64_t> total_size) {
  std::vector<std::uint8_t> pending(1024);
  std::size_t got = 0;
  while (got < pending.size()) {
    std::size_t n = read({pending.data() + got, pending.size() - got});
    if (n == 0) break;
    got += n;
  }
  pending.resize(got);
  if (got < 15) return Error::make(Errc::corrupt_data, "file too small for STL");

  StlStats s;
  bool binary;
  if (got >= 84 && total_size) {
    std::uint32_t n = 0;
    std::memcpy(&n, pending.data() + 80, 4);
    binary = *total_size == 84ull + 50ull * n;
  } else {
    const std::string_view head(reinterpret_cast<const char*>(pending.data()), got);
    binary = !(head.rfind("solid", 0) == 0 && head.find("facet") != std::string_view::npos);
  }
  if (binary) {
    if (got < 84) return Error::make(Errc::corrupt_data, "binary STL shorter than header");
    return probe_binary(read, pending, s);
  }
  return probe_ascii(read, pending, s);
}

Result<StlStats> probe_stl_file(const std::string& utf8_path) {
  namespace fs = std::filesystem;
  const fs::path p = paths::native(fs::absolute(fs::path(std::u8string(utf8_path.begin(), utf8_path.end()))));
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  if (ec) return Error::make(Errc::io_error, ec.message(), utf8_path);
  std::ifstream in(p, std::ios::binary);
  if (!in) return Error::make(Errc::io_error, "cannot open", utf8_path);
  ReadFn read = [&in](std::span<std::uint8_t> b) -> std::size_t {
    in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<std::size_t>(in.gcount());
  };
  return probe_stl(read, size);
}

}  // namespace am::formats
