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
  std::string serialize(bool structural) override {
    value["asset"]["metadata"]["title"] = "S7 edited title";
    if (structural) {
      auto& values = value.at("asset").at("metadata").at("custom_values");
      if (values.size() < 2 || values[0].at("field_id").as<std::string>().rfind("user:", 0) != 0 ||
          values[1].at("field_id").as<std::string>().rfind("plugin:", 0) != 0)
        throw std::runtime_error("structural fixture lacks core value before plugin value");
      values.erase(values.array_range().begin());
      const auto revision = value["asset"]["revision"].as<std::uint64_t>() + 1;
      value["asset"]["revision"] = revision;
      value["asset"]["modified_at"] = "2026-11-11T00:00:00Z";
      value["history"]["entries"].push_back(jsoncons::json::parse(history_entry(revision).dump()));
    }
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
