#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <new>

#include "am/domain/manifest/codec.hpp"
#include "am/foundation/json_text.hpp"
#include "published_manifest_schema.hpp"

namespace am::manifest {
std::span<const std::string_view> published_schema_parts() noexcept { return published_schema_data; }
namespace {
Error schema_error(std::string detail) {
  return Error::make(Errc::corrupt_data, "Schema validation failed", std::move(detail))
      .in("domain.manifest")
      .op("manifest.v2")
      .id("am.manifest.schema_violation");
}
Error oom() {
  return Error::make(Errc::resource_exhausted, "Allocation failed")
      .in("domain.manifest")
      .op("manifest.v2")
      .id("am.manifest.out_of_memory");
}
const jsoncons::json& normative_schema() {
  static const auto value = [] {
    std::string text;
    for (const auto part : published_schema_parts()) text += part;
    return jsoncons::json::parse(text);
  }();
  return value;
}
bool base64_pattern(std::string_view text) noexcept {
  // Exactly the published regex language, including empty strings and nonzero pad bits.
  if (text.size() % 4) return false;
  auto end = text.size();
  while (end && text[end - 1] == '=') --end;
  if (text.size() - end > 2) return false;
  for (std::size_t i = 0; i < end; ++i) {
    const auto c = text[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/'))
      return false;
  }
  return true;
}
bool file_type_pattern(std::string_view text) noexcept {
  if (text.empty()) return false;
  bool first = true, empty = true;
  for (const auto c : text) {
    if (c == '.') {
      if (empty) return false;
      first = false;
      empty = true;
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (!first && (c == '_' || c == '-')))) return false;
    empty = false;
  }
  return !empty;
}
Result<void> compact_raw(jsoncons::json& object, const char* field, const std::string& pointer) {
  if (!object.is_object() || !object.contains(field)) return {};
  auto& raw = object.at(field);
  if (!raw.is_object() || !raw.contains("base64") || !raw.at("base64").is_string()) return {};
  if (!base64_pattern(raw.at("base64").as_string_view()))
    return schema_error(pointer + "/" + field + "/base64: String does not match the published rawName pattern.");
  raw.at("base64") = "";
  return {};
}
Result<void> compact_normative(jsoncons::json& instance) {
  // Only a validation copy is compacted. The source document, opaque values and
  // published schema are untouched. These linear checks are regex-equivalent;
  // jsoncons still validates all other constraints. Custom schemas never enter here.
  // Published anchors follow ECMA-262 without multiline: whole strings, independent
  // of the platform's std::regex anchor behavior.
  if (!instance.is_object()) return {};
  if (instance.contains("inputs") && instance.at("inputs").is_array()) {
    auto& inputs = instance.at("inputs");
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      auto& input = inputs.at(i);
      if (!input.is_object()) continue;
      for (const auto* list : {"objects", "entries"})
        if (input.contains(list) && input.at(list).is_array()) {
          auto& items = input.at(list);
          for (std::size_t n = 0; n < items.size(); ++n) {
            auto result = compact_raw(items.at(n), std::string_view(list) == "objects" ? "name_raw" : "path_raw",
                                      "/inputs/" + std::to_string(i) + "/" + list + "/" + std::to_string(n));
            if (!result) return result;
          }
        }
    }
  }
  if (instance.contains("files") && instance.at("files").is_array()) {
    auto& files = instance.at("files");
    for (std::size_t i = 0; i < files.size(); ++i) {
      auto& file = files.at(i);
      if (!file.is_object() || !file.contains("type") || !file.at("type").is_object()) continue;
      auto& type = file.at("type");
      if (type.contains("id") && type.at("id").is_string()) {
        if (!file_type_pattern(type.at("id").as_string_view()))
          return schema_error("/files/" + std::to_string(i) +
                              "/type/id: String does not match the published file type pattern.");
        type.at("id") = "a";
      }
    }
  }
  return {};
}
}  // namespace
struct SchemaValidator::Impl {
  jsoncons::jsonschema::json_schema<jsoncons::json> schema;
  bool normative;
  explicit Impl(jsoncons::jsonschema::json_schema<jsoncons::json> value, bool published)
      : schema(std::move(value)), normative(published) {}
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
    auto parsed = jsoncons::json::parse(text);
    const bool normative = parsed == normative_schema();
    return SchemaValidator(std::make_unique<Impl>(jsoncons::jsonschema::make_json_schema(parsed), normative));
  } catch (const std::bad_alloc&) {
    return oom();
  } catch (const std::exception& e) {
    return schema_error(e.what());
  }
}
Result<void> SchemaValidator::validate(const ManifestDocument& document) const {
  try {
    auto shadow = document.shadow();
    if (!shadow) return shadow.error();
    auto instance = jsoncons::json::parse(*shadow);
    if (impl_->normative) {
      auto checked = compact_normative(instance);
      if (!checked) return checked.error();
    }
    impl_->schema.validate(instance);
    return {};
  } catch (const std::bad_alloc&) {
    return oom();
  } catch (const std::exception& e) {
    return schema_error(e.what());
  }
}
}  // namespace am::manifest
