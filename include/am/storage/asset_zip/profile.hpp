#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "am/storage/asset_zip/reader.hpp"
namespace am::asset_zip {
namespace profile_code {
inline constexpr std::string_view encrypted = "am.zip.profile.encrypted";
inline constexpr std::string_view method = "am.zip.profile.method";
inline constexpr std::string_view descriptor = "am.zip.profile.data_descriptor";
inline constexpr std::string_view utf8_flag = "am.zip.profile.utf8_flag";
inline constexpr std::string_view invalid_utf8 = "am.zip.profile.invalid_utf8";
inline constexpr std::string_view kind = "am.zip.profile.entry_kind";
inline constexpr std::string_view path = "am.zip.profile.path";
inline constexpr std::string_view collision = "am.zip.profile.path_collision";
inline constexpr std::string_view manifest = "am.zip.profile.missing_manifest";
inline constexpr std::string_view space = "am.zip.profile.namespace";
inline constexpr std::string_view gaps = "am.zip.profile.unaccounted_bytes";
// Manager's proposal, not an approved format rule (T-007).
inline constexpr std::string_view comment = "am.zip.profile.archive_comment";
}
struct ProfileViolation {
  std::string code;
  std::optional<std::size_t> entry;
  std::string detail;
};
Result<std::vector<ProfileViolation>> check_normative_profile(const ZipArchive& archive);
}  // namespace am::asset_zip
