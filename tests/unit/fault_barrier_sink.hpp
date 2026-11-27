#pragma once
#include "am/storage/asset_zip/sink.hpp"
#include <functional>
#include <string_view>

namespace fault_test {
class BarrierSink final : public am::asset_zip::RandomAccessSink {
 public:
  std::shared_ptr<am::asset_zip::RandomAccessSink> inner;
  std::function<void(std::string_view)> point;
  bool wrote = false;
  am::Result<void> write_at(std::uint64_t offset,std::span<const std::byte> bytes) override {
    if (!wrote && bytes.size()>1) {
      wrote=true;
      auto partial=inner->write_at(offset,bytes.first(bytes.size()/2));
      if (!partial) return partial.error();
      point("mid_write");
    }
    return inner->write_at(offset,bytes);
  }
  am::Result<void> flush() override {
    auto result=inner->flush();
    if (result) point("after_write");
    return result;
  }
  am::Result<void> close() override { return inner->close(); }
  am::Result<void> discard() override { return inner->discard(); }
  std::uint64_t size() const noexcept override { return inner->size(); }
};
}
