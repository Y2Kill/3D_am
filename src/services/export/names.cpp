#include "am/services/export/names.hpp"

#include <algorithm>
#include <new>

#include "am/foundation/unicode.hpp"

namespace am::exporting {
namespace {
Error invalid_name() {
  return Error::make(Errc::invalid_argument, "Invalid export filename")
      .in("services.export").id("am.export.name_invalid");
}
bool forbidden(unsigned char c) noexcept {
  return c < 0x20 || c == 0x7f || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos;
}
bool c1(std::string_view text, std::size_t position) noexcept {
  return static_cast<unsigned char>(text[position]) == 0xc2 && position + 1 < text.size() &&
         static_cast<unsigned char>(text[position + 1]) >= 0x80 &&
         static_cast<unsigned char>(text[position + 1]) <= 0x9f;
}
bool reserved(std::string_view name) {
  const auto dot = name.find('.');
  std::string stem(name.substr(0, dot));
  for (auto& c : stem) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
  if (!stem.starts_with("com") && !stem.starts_with("lpt")) return false;
  const auto number = std::string_view(stem).substr(3);
  return (number.size() == 1 && number[0] >= '1' && number[0] <= '9') ||
         number == "\xc2\xb9" || number == "\xc2\xb2" || number == "\xc2\xb3";
}
void trim(std::string& value) {
  while (!value.empty() && (value.back() == '.' || value.back() == ' ')) value.pop_back();
}
// Called only after UTF-8 validation. Count UTF-16 units without normalizing.
std::size_t units(std::string_view text) noexcept {
  std::size_t total = 0;
  for (unsigned char c : text) if ((c & 0xc0) != 0x80) total += c >= 0xf0 ? 2 : 1;
  return total;
}
bool zip_extension(std::string_view name) noexcept {
  if (name.size() < 4) return false;
  const auto end = name.substr(name.size() - 4);
  return end[0] == '.' && (end[1] == 'z' || end[1] == 'Z') &&
         (end[2] == 'i' || end[2] == 'I') && (end[3] == 'p' || end[3] == 'P');
}
void shorten(std::string& base, std::string_view suffix) {
  std::size_t end = 0, count = 0;
  while (end < base.size()) {
    const auto c = static_cast<unsigned char>(base[end]);
    const std::size_t width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    const std::size_t next_units = count + (width == 4 ? 2 : 1);
    if (end + width + suffix.size() > kMaxNameUtf8Bytes ||
        next_units + units(suffix) > kMaxNameUtf16Units) break;
    end += width;
    count = next_units;
  }
  base.resize(end);
  trim(base);
}
}  // namespace

Result<ExportName> export_name(std::string_view title, std::string_view asset_id,
                               std::span<const Preset> presets, bool explicit_files,
                               std::optional<std::string_view> explicit_name, unsigned ordinal) {
  try {
    if (ordinal == 0 || ordinal > kMaxCollisionOrdinal) return invalid_name();
    std::string base, extension = ".zip", category;
    if (explicit_name) {
      if (!unicode::is_valid_utf8(*explicit_name) || explicit_name->empty() ||
          explicit_name->size() > kMaxNameUtf8Bytes || units(*explicit_name) > kMaxNameUtf16Units ||
          std::any_of(explicit_name->begin(), explicit_name->end(), [](unsigned char c) { return forbidden(c); }))
        return invalid_name();
      base = *explicit_name;
      for (std::size_t i = 0; i < base.size(); ++i) if (c1(base, i)) return invalid_name();
      if (zip_extension(base)) { extension = base.substr(base.size() - 4); base.resize(base.size() - 4); }
      if (base.empty() || base.back() == '.' || base.back() == ' ' || reserved(base) ||
          base.size() + extension.size() > kMaxNameUtf8Bytes || units(base) + units(extension) > kMaxNameUtf16Units)
        return invalid_name();
    } else {
      if (!unicode::is_valid_utf8(title) || !unicode::is_valid_utf8(asset_id)) return invalid_name();
      for (std::size_t i = 0; i < title.size(); ++i) {
        if (c1(title, i)) { base += '_'; ++i; }
        else base += forbidden(static_cast<unsigned char>(title[i])) ? '_' : title[i];
      }
      trim(base);
      if (base.empty()) base = asset_id;
      if (base.empty()) return invalid_name();
      if (reserved(base)) base.insert(0, "_");
      const auto label = preset_suffix(presets);
      if (!label.empty() || explicit_files) category = " (" + (label.empty() ? "files" : label) + ")";
    }
    const auto original_base = base;
    const std::string collision = ordinal == 1 ? "" : " (" + std::to_string(ordinal) + ")";
    const auto suffix = category + collision + extension;
    shorten(base, suffix);
    if (base.empty()) return invalid_name();
    if (reserved(base)) { base.insert(0, "_"); shorten(base, suffix); }
    const bool sanitized = explicit_name ? base != original_base : base != title;
    return ExportName{base + suffix, sanitized};
  } catch (const std::bad_alloc&) {
    return Error::make(Errc::resource_exhausted, "Export filename allocation failed")
        .in("services.export").id("am.export.out_of_memory");
  }
}
}  // namespace am::exporting
