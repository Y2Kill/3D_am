#include "am/domain/manifest/codec.hpp"
#include "am/foundation/json_text.hpp"
#include <new>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

namespace am::manifest {
namespace {
Error schema_error(std::string detail) {
  return Error::make(Errc::corrupt_data, "Schema validation failed", std::move(detail))
      .in("domain.manifest").op("manifest.v2").id("am.manifest.schema_violation");
}
Error oom() {
  return Error::make(Errc::resource_exhausted, "Allocation failed")
      .in("domain.manifest").op("manifest.v2").id("am.manifest.out_of_memory");
}
}
struct SchemaValidator::Impl {
  jsoncons::jsonschema::json_schema<jsoncons::json> schema;
  explicit Impl(jsoncons::jsonschema::json_schema<jsoncons::json> value) : schema(std::move(value)) {}
};
SchemaValidator::SchemaValidator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SchemaValidator::SchemaValidator(SchemaValidator&&) noexcept = default;
SchemaValidator& SchemaValidator::operator=(SchemaValidator&&) noexcept = default;
SchemaValidator::~SchemaValidator() = default;
Result<SchemaValidator> SchemaValidator::create(std::string_view text) {
  try {
    // Bound the schema DOM before entering a recursive third-party parser too.
    const CodecLimits hypothesis;
    auto checked = am::json::parse_strict(text, {hypothesis.max_known_depth, hypothesis.max_bytes});
    if (!checked) {
      auto e = schema_error(checked.error().ctx.detail);
      e.code = checked.error().code;
      e.message_id = checked.error().message_id;
      if (e.message_id.starts_with("am.json.")) e.message_id.replace(0, 8, "am.manifest.");
      return std::move(e).caused_by(checked.error());
    }
    return SchemaValidator(std::make_unique<Impl>(jsoncons::jsonschema::make_json_schema(jsoncons::json::parse(text))));
  } catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return schema_error(e.what()); }
}
Result<void> SchemaValidator::validate(const ManifestDocument& document) const {
  try {
    auto shadow = document.shadow();
    if (!shadow) return shadow.error();
    impl_->schema.validate(jsoncons::json::parse(*shadow));
    return {};
  } catch (const std::bad_alloc&) { return oom(); }
  catch (const std::exception& e) { return schema_error(e.what()); }
}
}  // namespace am::manifest
