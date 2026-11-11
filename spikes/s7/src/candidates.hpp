#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
namespace s7 {
struct Candidate {
  virtual ~Candidate() = default;
  virtual void parse(std::string_view text) = 0;
  virtual std::string serialize(bool structural = true) = 0;
  virtual void validate_native(std::string_view schema) = 0;
};
std::unique_ptr<Candidate> candidate(std::string_view id);
void edit_dom(nlohmann::ordered_json& value, bool structural);
nlohmann::ordered_json history_entry(std::uint64_t revision);
std::string write_hybrid(const nlohmann::ordered_json& value, const std::vector<std::string>& fragments);
std::unique_ptr<Candidate> jsoncons_candidate();
void validate_shadow(std::string_view text, std::string_view schema);
nlohmann::ordered_json validator_probe(std::string_view text, std::string_view schema);
}  // namespace s7
