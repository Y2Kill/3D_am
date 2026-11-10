#include "candidates.hpp"
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

namespace s7 {
namespace {
struct JsonconsCandidate final : Candidate {
  jsoncons::json value;
  void parse(std::string_view text) override {
    value = jsoncons::json::parse(text, jsoncons::json_options{}.lossless_number(true));
  }
  std::string serialize() override {
    value["asset"]["metadata"]["title"] = "S7 edited title";
    return value.to_string();
  }
  void validate_native(std::string_view schema) override {
    auto validator = jsoncons::jsonschema::make_json_schema(jsoncons::json::parse(schema));
    validator.validate(value);
  }
};
}  // namespace
std::unique_ptr<Candidate> jsoncons_candidate() { return std::make_unique<JsonconsCandidate>(); }
void validate_shadow(std::string_view text, std::string_view schema) {
  auto validator = jsoncons::jsonschema::make_json_schema(jsoncons::json::parse(schema));
  validator.validate(jsoncons::json::parse(text));
}
}  // namespace s7
