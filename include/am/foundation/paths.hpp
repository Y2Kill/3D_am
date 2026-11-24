#pragma once
// Path policy (ADR-0012).
//  * native(): long-path-safe OS paths (\\?\ on Windows). 2 980 paths in the
//    reference collection exceed MAX_PATH once archive members are extracted.
//  * ZipPathMapper: turns original (archive) paths into safe, unique paths
//    inside the asset ZIP, recording why a name had to change.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "am/foundation/result.hpp"

namespace am::paths {

// Windows: absolute paths get the \\?\ (or \\?\UNC\) prefix and backslashes.
// Other platforms: returned unchanged.
std::filesystem::path native(const std::filesystem::path& p);

enum class NodeKind { file, directory, link, other };
struct WindowsNodeData {
  std::uint32_t attributes = 0, reparse_tag = 0;
  std::optional<std::uint64_t> size;
  std::optional<std::int64_t> mtime_ns;
};
using NodeDataHook = std::function<Result<WindowsNodeData>(const std::filesystem::path&)>;
struct NodeInfo {
  NodeKind kind = NodeKind::other;
  std::optional<std::uint32_t> reparse_tag;
  bool cloud_placeholder = false;
  std::optional<std::uint64_t> size;
  std::optional<std::int64_t> mtime_ns;
};
// Pure Windows metadata mapping, also testable on POSIX. Never reads file data.
NodeInfo classify_windows_node(const WindowsNodeData&) noexcept;
Result<WindowsNodeData> windows_node_data(const std::filesystem::path&);
// Does not follow links or read file data. POSIX uses symlink_status.
Result<NodeInfo> node_info(const std::filesystem::path&);

// Normalises an original path from an archive or filesystem to forward
// slashes, drops leading "./" and "/" and collapses duplicate separators.
// Does NOT resolve ".." — that is a policy decision of the mapper.
std::string normalize_source_path(std::string_view original);

// Strict, non-transforming normative container path check. The first failure
// is returned in ctx.detail; unlike ZipPathMapper this never repairs a name.
Result<void> check_container_path(std::string_view utf8_path);

enum class RenameReason { none, path_too_long, invalid_chars, encoding, collision, reserved_name };
std::string_view to_string(RenameReason r);

struct MappedPath {
  std::string zip_path;  // e.g. "content/debdf916-b47c-41db-b4fd-7aa9511396f3/28mm/body.stl"
  RenameReason reason = RenameReason::none; // none => zip path mirrors source exactly
};

class ZipPathMapper {
 public:
  static constexpr std::size_t kMaxZipPathBytes = 200;
  static constexpr std::size_t kCollisionTagBytes = 9;
  static constexpr std::size_t kMaxRootBytes = kMaxZipPathBytes - kCollisionTagBytes - 1;

  // Nonempty root, at most kMaxRootBytes including an appended trailing '/'.
  static Result<ZipPathMapper> create(std::string root);

  // Maps one source path. Collisions are detected case-insensitively because
  // extracted assets must survive Windows filesystems.
  MappedPath map(std::string_view source_path);

  const std::string& root() const { return root_; }

 private:
  explicit ZipPathMapper(std::string root);
  std::string root_;
  std::unordered_set<std::string> used_folded_;
};

// Exposed for tests.
bool is_reserved_windows_name(std::string_view component);
std::string truncate_utf8(std::string_view s, std::size_t max_bytes);
std::uint64_t fnv1a64(std::string_view s);

}  // namespace am::paths
