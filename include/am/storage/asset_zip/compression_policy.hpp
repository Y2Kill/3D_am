#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include "am/foundation/result.hpp"
namespace am::asset_zip {
enum class CompressionMethod : std::uint16_t { stored = 0, deflate = 8 };
// Caller supplies at most the first 64 KiB. Performs no file access.
Result<CompressionMethod> choose_method(std::string_view name, std::span<const std::byte> sample);
}  // namespace am::asset_zip
