#pragma once
// Content-based type detection (ADR-0011). Extensions are only hints: the
// reference collection contains RAR/7z files named *.zip and AppleDouble
// stubs named *.3mf / *.stl.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace am {

enum class FileKind : std::uint16_t {
  unknown = 0,
  // containers
  zip, rar4, rar5, sevenzip, gzip, xz, bzip2, zstd,
  // OS junk (exclusion candidates)
  apple_double, ds_store, ole_compound,  // OLE = Thumbs.db, old Office, …
  // images
  png, jpeg, gif, webp, bmp, tiff, psd,
  // 3D / slicer
  stl_binary, stl_ascii, obj_text, blend, ctb, gcode_text,
  // documents
  pdf, rtf,
};

std::string_view to_string(FileKind k);
bool is_archive(FileKind k);

// Minimum header bytes callers should provide for reliable detection.
inline constexpr std::size_t kSignatureProbeBytes = 96;

// `header` = first bytes of the file (up to kSignatureProbeBytes or more),
// `total_size` = full file size when known (needed for binary STL check).
FileKind detect_kind(std::span<const std::uint8_t> header,
                     std::optional<std::uint64_t> total_size = std::nullopt);

}  // namespace am
