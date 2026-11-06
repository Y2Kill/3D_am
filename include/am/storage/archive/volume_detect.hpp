#pragma once
// Name-based multi-volume candidate grouping (ADR-0011).
//
// Names are only HINTS: download clients (Telegram, browsers) rename files
// that clash, e.g. "X.part2.rar" -> "X (2).rar" (seen in the reference
// collection), so the archive backend must confirm membership and order from
// volume headers. Candidates of kind copy_suffix always need header confirmation.
//
// No source-specific (e.g. Telegram) rule is hard-wired in the core
// (ТЗ §13.2.2): which name schemes are tried is configured by the import
// profile through VolumeNamingRules.

#include <cstdint>
#include <string>
#include <vector>

namespace am::archive {

enum class VolumeScheme {
  rar_part,         // X.part1.rar, X.part2.rar ...  (RAR 3+)
  rar_old,          // X.rar, X.r00, X.r01 ...       (RAR 2/3 old naming)
  split_numeric,    // X.7z.001, X.zip.001 ...        (byte-split, concatenate)
  zip_split,        // X.z01, X.z02 ... X.zip        (PKWARE split)
  copy_suffix,      // X.rar, X (2).rar, X (3).rar   (renamed by a download client)
};

std::string to_string(VolumeScheme s);

struct VolumeCandidate {
  VolumeScheme scheme;
  std::string base;                    // common stem
  std::vector<std::string> members;    // file names in volume order
  std::vector<int> missing_indices;    // gaps in numbering (1-based)
  bool needs_header_confirmation = false;
};

struct GroupingResult {
  std::vector<VolumeCandidate> volume_sets;
  std::vector<std::string> singles;    // files not part of any volume set
};

// Profile-controlled naming rules (profiles/*.toml, section [volumes]).
struct VolumeNamingRules {
  bool rar_part = true;
  bool rar_old = true;
  bool split_numeric = true;
  bool zip_split = true;
  bool copy_suffix = true;
  // Extensions (lower case, no dot) for which "X (N).ext" is a volume hint.
  std::vector<std::string> copy_suffix_extensions = {"rar", "zip", "7z"};
};

// `names` = file names (not paths) located in ONE directory.
GroupingResult group_volumes(const std::vector<std::string>& names, const VolumeNamingRules& rules = {});

}  // namespace am::archive
