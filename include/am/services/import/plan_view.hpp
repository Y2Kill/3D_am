#pragma once
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "am/foundation/result.hpp"

namespace am::import {
// Checked against every occurrence of the raw hex pattern in the normative schema.
inline constexpr std::array<std::string_view, 3> plan_raw_schema_fields{"/properties/roots/items/properties/path_raw",
                                                                        "/properties/nodes/items/properties/name_raw",
                                                                        "/properties/nodes/items/properties/path_raw"};
inline constexpr std::string_view plan_archive_raw_schema_field="/properties/archive_entries/items/properties/path_raw";
enum class NodeKind { file, directory, symlink, other };
enum class InputKind { file, directory, archive, multivolume_archive };
enum class CandidateKind { new_asset, ready_asset };
enum class CandidateAction { import, skip, ask };
enum class EntryDecision { include, exclude, ask, skip, expand };
enum class ImportMode { strict, partial };
struct RawBytes {
  std::string bytes, encoding;
};
struct PlanRoot {
  std::string path;
  std::optional<RawBytes> raw;
};
struct PlanProblem {
  std::string code, message_id;
  std::optional<std::string> limit{}, effective_value{}, source{};
};
struct PlanNode {
  std::string id, path, name, detected;
  std::size_t root = 0;
  NodeKind kind = NodeKind::other;
  bool available = true, empty = false, excluded_only = false;
  std::optional<std::uint64_t> size;
  std::optional<std::int64_t> mtime_ns;
  std::optional<std::string> sha256;
  std::optional<RawBytes> name_raw, path_raw;
  std::optional<PlanProblem> problem;
};
struct PlanEntry {
  std::size_t node = 0;
  EntryDecision decision = EntryDecision::ask;
  std::string rule_id, confidence, explanation_id;
  std::optional<std::size_t> archive_entry{};
};
struct PlanLimits {
  std::uint64_t max_entries=0, max_unpacked_bytes=0, max_seconds_per_container=0, max_temp_bytes=0;
  double max_ratio=0;
  std::uint64_t max_depth=0; // 0: absent before plan v3, the unconditional ceiling applies.
};
enum class PlanArchiveStatus { listed, unreadable, limit_exceeded };
struct PlanArchive {
  // For a nested archive (plan v3) `node` is its top-level filesystem archive.
  std::size_t node=0;
  std::optional<std::size_t> parent_entry;  // archive_entries index of the containing record
  std::uint64_t depth=1;
  std::optional<std::string> sha256;
  bool temp=false;  // access = temp: materialized in the library's managed temporary area
  PlanArchiveStatus status=PlanArchiveStatus::listed;
  std::uint64_t entry_count=0, comment_length=0, encrypted_entries=0, unsupported_entries=0;
  bool zip64=false;
  std::vector<std::string> name_encodings;
  std::vector<std::size_t> entries;
  std::optional<PlanProblem> problem;
};
struct PlanArchiveEntry {
  std::string id, path, raw_path, name_encoding, detected;
  std::size_t archive=0;
  std::uint64_t index=0, size=0, compressed_size=0;
  std::uint32_t crc32=0;
  std::uint16_t method=0;
  NodeKind kind=NodeKind::file;
  bool available=true, encrypted=false, encoding_exact=true;
  std::optional<double> encoding_confidence;
  std::optional<PlanProblem> problem;
};
struct PlanVolumeSet {
  std::string id, scheme, base;
  std::vector<std::size_t> members;
  std::vector<std::uint64_t> missing_indices;
  bool confirmed = false;
};
struct PlanObjectRef {
  std::optional<std::size_t> node, volume_set;
};
struct PlanInput {
  std::string id;
  InputKind kind;
  std::vector<PlanObjectRef> objects;
};
struct PlanSource {
  std::string kind, name;
};
struct PlanLink {
  std::string url;
  std::size_t node = 0;
  bool ad = false;
};
struct PlanMatch {
  std::string kind, asset_id, object_id, relation;
  std::optional<std::size_t> node;
};
struct PlanCandidate {
  std::string id, base_path, title_hint, confidence;
  std::size_t root = 0;
  CandidateKind kind;
  CandidateAction action;
  std::vector<PlanInput> inputs;
  std::vector<PlanEntry> entries;
  std::vector<std::size_t> directories;
  std::vector<PlanVolumeSet> volume_sets;
  std::vector<PlanSource> sources;
  std::vector<PlanLink> suggested_links;
  std::vector<PlanMatch> matches;
  std::vector<std::string> reasons, warnings;
  std::optional<std::string> asset_id;
  std::optional<std::uint64_t> revision;
  // Built once with the immutable view; archive decisions must not scan every record.
  std::map<std::size_t,std::size_t> node_decisions, archive_decisions;
  std::map<std::pair<std::size_t,std::string>,std::size_t> excluded_archive_directories;
};
class PlanView {
 public:
  const std::vector<PlanRoot>& roots() const noexcept { return roots_; }
  const std::vector<PlanNode>& nodes() const noexcept { return nodes_; }
  const std::vector<PlanCandidate>& candidates() const noexcept { return candidates_; }
  ImportMode mode() const noexcept { return mode_; }
  bool scan_complete() const noexcept { return scan_complete_; }
  unsigned version() const noexcept { return version_; }
  const std::optional<PlanLimits>& limits() const noexcept { return limits_; }
  const std::vector<PlanArchive>& archives() const noexcept { return archives_; }
  const std::vector<PlanArchiveEntry>& archive_entries() const noexcept { return archive_entries_; }
  std::optional<std::size_t> archive_for_node(std::size_t node) const noexcept {
    const auto at=archive_by_node_.find(node);
    return at==archive_by_node_.end()?std::nullopt:std::optional<std::size_t>{at->second};
  }
  // Nested archive listed from the given archive_entries index (plan v3).
  std::optional<std::size_t> archive_for_entry(std::size_t entry) const noexcept {
    const auto at=archive_by_entry_.find(entry);
    return at==archive_by_entry_.end()?std::nullopt:std::optional<std::size_t>{at->second};
  }

 private:
  PlanView() = default;
  std::vector<PlanRoot> roots_;
  std::vector<PlanNode> nodes_;
  std::vector<PlanCandidate> candidates_;
  ImportMode mode_ = ImportMode::strict;
  bool scan_complete_ = true;
  unsigned version_=1;
  std::optional<PlanLimits> limits_;
  std::vector<PlanArchive> archives_;
  std::vector<PlanArchiveEntry> archive_entries_;
  std::map<std::size_t,std::size_t> archive_by_node_, archive_by_entry_;
  friend Result<PlanView> parse_plan_view(std::string_view);
};
// The only builder validates exact input bytes through parse_plan first.
Result<PlanView> parse_plan_view(std::string_view);
// Native raw names are usable only on the platform that supplied their encoding.
Result<std::filesystem::path> source_path(const PlanView&, const PlanNode&);
}  // namespace am::import
