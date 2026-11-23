#pragma once
#include <array>

#include "am/services/import/scan.hpp"
#include "am/storage/asset_zip/sha256.hpp"

namespace am::import::detail {
inline Error error(std::string id, std::string pointer = {}, Errc code = Errc::invalid_argument) {
  return Error::make(code, "Import operation failed", std::move(pointer)).in("services.import").id(std::move(id));
}
template <class T>
T take(Result<T> value) {
  if (!value) throw value.error();
  return std::move(*value);
}
inline void take(Result<void> value) {
  if (!value) throw value.error();
}
inline std::string hex(std::span<const std::byte> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (auto byte : bytes) {
    const auto b = std::to_integer<unsigned>(byte);
    result += digits[b >> 4];
    result += digits[b & 15];
  }
  return result;
}
inline std::string hash(std::string_view bytes) {
  auto state = take(asset_zip::Sha256::create());
  take(state.update(std::as_bytes(std::span(bytes.data(), bytes.size()))));
  return hex(take(state.finish()));
}
}  // namespace am::import::detail
