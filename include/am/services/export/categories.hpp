#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "am/foundation/result.hpp"

namespace am::exporting {
inline constexpr unsigned kCategoriesVersion = 1;
enum class Preset { model, model_preview, slicer, all };
std::string_view preset_name(Preset) noexcept;
Result<Preset> parse_preset(std::string_view);
// Stable order with duplicates removed, independent of command-line order.
std::string preset_suffix(std::span<const Preset>);
struct Categories { bool model = false, image = false, slicer = false; };
// A present type ID is authoritative, including an unrecognized or empty ID.
Categories categories(std::optional<std::string_view> type_id, std::string_view storage_path) noexcept;
bool selected(Categories, bool listed_preview, std::span<const Preset>) noexcept;
}  // namespace am::exporting
