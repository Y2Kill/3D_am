#pragma once
// Streaming STL probe: type, triangle count, bounding box, surface area and
// signed volume WITHOUT loading the mesh into memory. STL is 72 % of files in
// the reference collection, so this runs for every imported STL (metadata
// extractor, ADR-0018) and is the baseline for mesh fingerprints (ADR-0016).

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "am/foundation/result.hpp"

namespace am::formats {

struct StlStats {
  bool binary = true;
  std::uint64_t triangles = 0;
  std::array<float, 3> min{0, 0, 0};
  std::array<float, 3> max{0, 0, 0};
  double area = 0.0;          // in file units (STL has no units; mm assumed and marked "assumed")
  double volume = 0.0;        // signed; |volume| meaningful for closed meshes
  std::uint64_t degenerate = 0;
  std::string header;         // 80-byte header (binary) or solid name (ASCII), sanitized
};

// Pull-style reader: fills `buf`, returns bytes read (0 = EOF). Lets the probe
// read straight from a ZIP entry stream or a file.
using ReadFn = std::function<std::size_t(std::span<std::uint8_t> buf)>;

Result<StlStats> probe_stl(const ReadFn& read, std::optional<std::uint64_t> total_size);
Result<StlStats> probe_stl_file(const std::string& utf8_path);

}  // namespace am::formats
