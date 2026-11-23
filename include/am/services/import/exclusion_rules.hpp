#pragma once
// Built-in rules for technical/redundant files (ТЗ 6.5, ADR-0011, open question 7).
//
// Confidence model:
//   certain  — name/structure AND content signature agree -> excluded by default
//   probable — only the name/structure matches            -> proposed to the user
// Rules are applied BEFORE nested containers are expanded: the collection has
// "__MACOSX/._model.3mf" AppleDouble stubs that would otherwise be opened as ZIP.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "am/foundation/signature.hpp"

namespace am::import {

inline constexpr int kExclusionRulesVersion = 1;

enum class Confidence { none, probable, certain };

struct RuleMatch {
  std::string rule_id;          // e.g. "macos.appledouble"
  Confidence confidence = Confidence::none;
  std::string explanation;      // short, shown to the user
};

struct EntryFacts {
  std::string_view path;                        // normalized, '/' separators
  std::optional<std::uint64_t> size;
  std::optional<FileKind> kind;                 // from detect_kind(header) when available
};

class ExclusionEngine {
 public:
  ExclusionEngine();  // loads built-in rules v1

  // Returns the strongest match, or confidence==none.
  RuleMatch evaluate(const EntryFacts& entry) const;
  RuleMatch evaluate(const EntryFacts& entry, std::span<const std::string> enabled_rules) const;

  std::vector<std::string> rule_ids() const;

 private:
  struct Rule {
    std::string id;
    RuleMatch (*fn)(const EntryFacts&);
  };
  std::vector<Rule> rules_;
};

}  // namespace am::import
