#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>
#include "am/foundation/result.hpp"

namespace am::asset_zip {
class RandomAccessSource {
 public:
  virtual ~RandomAccessSource() = default;
  virtual std::uint64_t size() const noexcept = 0;
  // May return a short read; 0 is EOF. Never writes outside the supplied span.
  virtual Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) const = 0;
};
// Owns its bytes. Concurrent reads are safe; the buffer is not mutable via API.
Result<std::shared_ptr<const RandomAccessSource>> memory_source(std::vector<std::byte> bytes);
// Read-only, shared-read file; cached initial size. Uses native long paths.
Result<std::shared_ptr<const RandomAccessSource>> file_source(const std::filesystem::path& path);
}  // namespace am::asset_zip
