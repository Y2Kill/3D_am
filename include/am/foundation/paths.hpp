#pragma once
// Path policy (ADR-0012).
//  * native(): long-path-safe OS paths (\\?\ on Windows). 2 980 paths in the
//    reference collection exceed MAX_PATH once archive members are extracted.
//  * ZipPathMapper: turns original (archive) paths into safe, unique paths
//    inside the asset ZIP, recording why a name had to change.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace am::paths {

// Windows: absolute paths get the \\?\ (or \\?\UNC\) prefix and backslashes.
// Other platforms: returned unchanged.
std::filesystem::path native(const std::filesystem::path& p);

// Normalises an original path from an archive or filesystem to forward
// slashes, drops leading "./" and "/" and collapses duplicate separators.
// Does NOT resolve ".." — that is a policy decision of the mapper.
std::string normalize_source_path(std::string_view original);

enum class RenameReason { none, path_too_long, invalid_chars, reserved_name, collision, unsafe_traversal };
std::string_view to_string(RenameReason r);

struct MappedPath {
  std::string zip_path;                     // e.g. "content/i001/28mm/body.stl"
  RenameReason reason = RenameReason::none; // none => zip path mirrors source exactly
};

class ZipPathMapper {
 public:
  static constexpr std::size_t kMaxZipPathBytes = 200;

  // root: "content/i001/" (must end with '/').
  explicit ZipPathMapper(std::string root);

  // Maps one source path. Collisions are detected case-insensitively because
  // extracted assets must survive Windows filesystems.
  MappedPath map(std::string_view source_path);

  const std::string& root() const { return root_; }

 private:
  std::string root_;
  std::unordered_set<std::string> used_folded_;
};

// Exposed for tests.
bool is_reserved_windows_name(std::string_view component);
std::string truncate_utf8(std::string_view s, std::size_t max_bytes);
std::uint64_t fnv1a64(std::string_view s);

}  // namespace am::paths
