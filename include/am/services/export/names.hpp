#pragma once

#include "am/services/export/categories.hpp"

namespace am::exporting {
// File-system component limits apply to the final name, including all suffixes.
inline constexpr std::size_t kMaxNameUtf8Bytes = 255, kMaxNameUtf16Units = 255;
inline constexpr unsigned kMaxCollisionOrdinal = 99;  // hypothesis, T-033
struct ExportName { std::string filename; bool sanitized = false; };
Result<ExportName> export_name(std::string_view title, std::string_view asset_id,
                               std::span<const Preset> presets = {}, bool explicit_files = false,
                               std::optional<std::string_view> explicit_name = {}, unsigned ordinal = 1);
}  // namespace am::exporting
