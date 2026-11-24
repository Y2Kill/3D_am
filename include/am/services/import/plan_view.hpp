#pragma once
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "am/foundation/result.hpp"

namespace am::import {
// Checked against every occurrence of the raw hex pattern in the normative schema.
inline constexpr std::array<std::string_view, 3> plan_raw_schema_fields{"/properties/roots/items/properties/path_raw",
                                                                        "/properties/nodes/items/properties/name_raw",
                                                                        "/properties/nodes/items/properties/path_raw"};
enum class NodeKind { file, directory, symlink, other };
enum class InputKind { file, directory, archive, multivolume_archive };
enum class CandidateKind { new_asset, ready_asset };
enum class CandidateAction { import, skip, ask };
enum class EntryDecision { include, exclude, ask, skip };
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
};
class PlanView {
 public:
  const std::vector<PlanRoot>& roots() const noexcept { return roots_; }
  const std::vector<PlanNode>& nodes() const noexcept { return nodes_; }
  const std::vector<PlanCandidate>& candidates() const noexcept { return candidates_; }
  ImportMode mode() const noexcept { return mode_; }
  bool scan_complete() const noexcept { return scan_complete_; }

 private:
  PlanView() = default;
  std::vector<PlanRoot> roots_;
  std::vector<PlanNode> nodes_;
  std::vector<PlanCandidate> candidates_;
  ImportMode mode_ = ImportMode::strict;
  bool scan_complete_ = true;
  friend Result<PlanView> parse_plan_view(std::string_view);
};
// The only builder validates exact input bytes through parse_plan first.
Result<PlanView> parse_plan_view(std::string_view);
// Native raw names are usable only on the platform that supplied their encoding.
Result<std::filesystem::path> source_path(const PlanView&, const PlanNode&);
}  // namespace am::import
