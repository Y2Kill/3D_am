#pragma once
// Byte-level manifest codec (ADR-0038). V3/V4 and ZIP are caller responsibilities.
#include <compare>
#include <cstddef>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "am/foundation/result.hpp"

namespace am::manifest {
struct CodecLimits {
  std::size_t max_depth = 1024;                // hypothesis, O-17
  std::size_t max_bytes = 128 * 1024 * 1024;   // hypothesis, O-17
  std::size_t max_known_depth = 64;            // hypothesis, O-17
};
// Escaped, scope-qualified owner identity; obtain keys from opaque_keys().
struct OpaqueKey {
  std::string value;
  auto operator<=>(const OpaqueKey&) const = default;
};
struct IntendedChanges {
  // Explicit authorization to add, remove or replace these owners only.
  std::set<OpaqueKey> owners;
};
class SchemaValidator;
// The build embeds the published schema once in domain. Storage consumes these
// immutable parts mechanically; the generated header remains private.
std::span<const std::string_view> published_schema_parts() noexcept;
// Base history fields named by SPEC 11. Callers also check for a JSON string.
bool is_named_change_field(std::string_view kind, std::string_view key) noexcept;
class ManifestDocument {
 public:
  ManifestDocument(ManifestDocument&&) noexcept;
  ManifestDocument& operator=(ManifestDocument&&) noexcept;
  ~ManifestDocument();
  ManifestDocument(const ManifestDocument&) = delete;
  ManifestDocument& operator=(const ManifestDocument&) = delete;
  nlohmann::ordered_json& known() noexcept;
  const nlohmann::ordered_json& known() const noexcept;
  // Validates one complete value and returns a document-local handle. This does
  // not authorize its replacement/addition: declare the owner in IntendedChanges.
  // Opaque roots must remain handles; a handle in a known field blocks writing.
  Result<nlohmann::ordered_json> make_opaque(std::string_view json_text);
  // Current DOM owners. An invalid/unwritable DOM returns no keys; write_manifest
  // provides the structured error. Empty keys never authorize a lost value.
  std::vector<OpaqueKey> opaque_keys() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit ManifestDocument(std::unique_ptr<Impl>);
  Result<std::string> shadow() const;
  friend Result<ManifestDocument> parse_manifest(std::string_view, const CodecLimits&);
  friend Result<std::string> write_manifest(const ManifestDocument&);
  friend class SchemaValidator;
};
// V1 only: owns its source/fragments, canonicalizes known integers before DOM
// parsing and hides opaque roots behind binary handles. Does not perform V2.
Result<ManifestDocument> parse_manifest(std::string_view text, const CodecLimits& limits = {});
// Validates each handle/fragment, size, depth and owner uniqueness. Call
// verify_preserved and V2 before accepting a write; this function performs no I/O.
Result<std::string> write_manifest(const ManifestDocument& document);
// Reparse both texts and compare unannounced owners by typed internal paths,
// decoded strings and exact numeric lexemes. Object member order is irrelevant;
// array order inside a fragment is significant. No file access or V3/V4 checks.
Result<void> verify_preserved(std::string_view before, std::string_view after,
                            const IntendedChanges& changes = {}, const CodecLimits& limits = {});

class SchemaValidator {
 public:
  // Reusable draft-2020-12 validator, compiled from caller-supplied text. Schema
  // DOM depth is bounded by the default known-depth hypothesis before jsoncons.
  static Result<SchemaValidator> create(std::string_view schema_json);
  // Validates the current DOM with same-type empty values for opaque roots.
  Result<void> validate(const ManifestDocument& document) const;
  SchemaValidator(SchemaValidator&&) noexcept;
  SchemaValidator& operator=(SchemaValidator&&) noexcept;
  ~SchemaValidator();
  SchemaValidator(const SchemaValidator&) = delete;
  SchemaValidator& operator=(const SchemaValidator&) = delete;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit SchemaValidator(std::unique_ptr<Impl>);
};
}  // namespace am::manifest
