#include "am/services/export/categories.hpp"

#include <algorithm>
#include <array>

namespace am::exporting {
namespace {
bool ascii_equal(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const auto c = left[i];
    if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c) != right[i]) return false;
  }
  return true;
}
template <std::size_t N>
bool extension_in(std::string_view extension, const std::array<std::string_view, N>& values) noexcept {
  return std::any_of(values.begin(), values.end(), [&](auto value) { return ascii_equal(extension, value); });
}
}  // namespace

std::string_view preset_name(Preset preset) noexcept {
  switch (preset) {
    case Preset::model: return "3d";
    case Preset::model_preview: return "3d_preview";
    case Preset::slicer: return "slicer";
    case Preset::all: return "all";
  }
  return {};
}
Result<Preset> parse_preset(std::string_view value) {
  for (auto preset : {Preset::model, Preset::model_preview, Preset::slicer, Preset::all})
    if (value == preset_name(preset)) return preset;
  return Error::make(Errc::invalid_argument, "Unknown export preset", std::string(value))
      .in("services.export").id("am.export.preset_invalid");
}
std::string preset_suffix(std::span<const Preset> presets) {
  std::string suffix;
  for (auto preset : {Preset::model, Preset::model_preview, Preset::slicer, Preset::all}) {
    if (std::find(presets.begin(), presets.end(), preset) == presets.end()) continue;
    if (!suffix.empty()) suffix += '+';
    suffix += preset_name(preset);
  }
  return suffix;
}
Categories categories(std::optional<std::string_view> type_id, std::string_view path) noexcept {
  if (type_id) {
    const auto id = *type_id;
    return {id.starts_with("mesh."), id.starts_with("image."),
            id.starts_with("mesh.stl.") || id == "mesh.obj" || id.starts_with("slice.") || id == "gcode"};
  }
  const auto slash = path.rfind('/');
  const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const auto dot = name.rfind('.');
  const auto extension = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
  static constexpr std::array<std::string_view, 17> models{
      "stl", "obj", "mtl", "3mf", "ply", "amf", "fbx", "gltf", "glb", "dae", "off", "3ds", "step", "stp",
      "iges", "igs", "blend"};
  static constexpr std::array<std::string_view, 8> images{
      "png", "jpg", "jpeg", "webp", "gif", "bmp", "tif", "tiff"};
  static constexpr std::array<std::string_view, 7> slicers{"stl", "3mf", "obj", "lys", "chitubox", "ctb", "gcode"};
  if (extension.empty()) return {};
  return {extension_in(extension, models), extension_in(extension, images), extension_in(extension, slicers)};
}
bool selected(Categories value, bool preview, std::span<const Preset> presets) noexcept {
  return std::any_of(presets.begin(), presets.end(), [&](auto preset) {
    switch (preset) {
      case Preset::model: return value.model;
      case Preset::model_preview: return value.model || value.image || preview;
      case Preset::slicer: return value.slicer;
      case Preset::all: return true;
    }
    return false;
  });
}
}  // namespace am::exporting
