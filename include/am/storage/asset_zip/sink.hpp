#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>
#include "am/foundation/result.hpp"

namespace am::asset_zip {
class RandomAccessSink {
 public:
  virtual ~RandomAccessSink() = default;
  virtual Result<void> write_at(std::uint64_t offset, std::span<const std::byte> bytes) = 0;
  virtual Result<void> flush() = 0;
  // Called after successful flush, before reopening through RandomAccessSource.
  virtual Result<void> close() { return {}; }
  virtual Result<void> discard() = 0;
  virtual std::uint64_t size() const noexcept = 0;
};
class MemorySink final : public RandomAccessSink {
 public:
  MemorySink();
  ~MemorySink() override;
  Result<void> write_at(std::uint64_t offset, std::span<const std::byte> bytes) override;
  Result<void> flush() override;
  Result<void> discard() override;
  std::uint64_t size() const noexcept override;
  std::span<const std::byte> bytes() const noexcept;
  Result<std::vector<std::byte>> take_bytes();
 private:
  std::vector<std::byte> bytes_;
  bool discarded_ = false;
};
Result<std::shared_ptr<MemorySink>> memory_sink();
// CREATE_NEW / O_EXCL: never truncates an existing file. Destruction closes it;
// ZipWriter is responsible for discard of an unfinished archive.
Result<std::shared_ptr<RandomAccessSink>> file_sink(const std::filesystem::path& path);
}  // namespace am::asset_zip
