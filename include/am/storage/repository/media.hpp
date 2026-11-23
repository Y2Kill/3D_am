#pragma once
#include <filesystem>
#include <string>
namespace am::repository {
enum class MediaKind { hdd, ssd, network, removable, unknown };
struct MediaInfo {
  std::string device_id;
  MediaKind kind = MediaKind::unknown;
};
MediaInfo detect_media(const std::filesystem::path&) noexcept;
namespace detail {
// The entry may be a partition; its canonical ancestors are searched within sysfs_root.
MediaKind media_from_sysfs(const std::filesystem::path& sysfs_root, const std::filesystem::path& entry) noexcept;
}  // namespace detail
}  // namespace am::repository
