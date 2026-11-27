#pragma once
#include "am/storage/asset_zip/sink.hpp"
#include <algorithm>
#include <atomic>

namespace fault_test {
class FullSink final : public am::asset_zip::RandomAccessSink {
 public:
  std::shared_ptr<am::asset_zip::RandomAccessSink> inner;
  std::uint64_t limit = 128; // hypothesis: deterministic mid-stream failure point
  std::shared_ptr<std::atomic<unsigned>> failures;
  am::Result<void> write_at(std::uint64_t offset,std::span<const std::byte> bytes) override {
    if (offset < limit) {
      const auto count=static_cast<std::size_t>((std::min)(limit-offset,static_cast<std::uint64_t>(bytes.size())));
      auto written=inner->write_at(offset,bytes.first(count));
      if (!written) return written.error();
      if (count==bytes.size()) return {};
    }
    if (failures) ++*failures;
    return am::Error::make(am::Errc::insufficient_space,"Injected full file sink")
        .id("am.zip.write.insufficient_space").allow(am::ErrorAction::free_space).allow(am::ErrorAction::abort);
  }
  am::Result<void> flush() override { return inner->flush(); }
  am::Result<void> close() override { return inner->close(); }
  am::Result<void> discard() override { return inner->discard(); }
  std::uint64_t size() const noexcept override { return inner->size(); }
};
}
