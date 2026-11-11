#pragma once
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include "am/foundation/result.hpp"

namespace am::asset_zip {
using Sha256Digest = std::array<std::byte, 32>;
class Sha256 {
 public:
  static Result<Sha256> create();
  Sha256(Sha256&&) noexcept;
  Sha256& operator=(Sha256&&) noexcept;
  ~Sha256();
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  Result<void> update(std::span<const std::byte> bytes);
  Result<Sha256Digest> finish();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit Sha256(std::unique_ptr<Impl> impl);
};
}  // namespace am::asset_zip
