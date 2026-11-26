#pragma once

#include <vector>

#include "am/domain/manifest/codec.hpp"
#include "am/services/export/categories.hpp"

namespace am::exporting {
// Indices into current files[], in manifest order. No historical or opaque data.
Result<std::vector<std::size_t>> select_files(const manifest::ManifestDocument&,
                                             std::span<const Preset> presets,
                                             std::span<const std::string> file_ids);
}  // namespace am::exporting
