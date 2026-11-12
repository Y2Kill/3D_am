#pragma once
#include "am/domain/manifest/codec.hpp"

namespace am::manifest {
enum class FindingSeverity { error, warning, unverifiable };
struct Finding {
  std::string rule;
  FindingSeverity severity = FindingSeverity::error;
  std::string pointer, message_id, detail;
};
struct DeferredRule {
  std::string rule, reason;
};
struct SemanticOptions {
  std::size_t max_findings = 1000;  // hypothesis, O-17
  bool stop_after_limit = true;
};
struct SemanticReport {
  bool complete = false;
  std::vector<std::string> checked_rules;
  std::vector<DeferredRule> deferred_rules;
  std::vector<Finding> findings;
  bool has_plugin_data = false;
};
// Read-only V3 over the known DOM after V1/V2; opaque handles are never opened.
// Invalid data is reported as findings. Only internal/allocation failures use Error.
Result<SemanticReport> validate_semantics(const ManifestDocument&, const SemanticOptions& = {});
}  // namespace am::manifest
