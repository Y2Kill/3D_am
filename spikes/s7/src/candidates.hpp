#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
namespace s7 {
struct Candidate {
  virtual ~Candidate() = default;
  virtual void parse(std::string_view text) = 0;
  virtual std::string serialize() = 0;
  virtual void validate_native(std::string_view schema) = 0;
};
std::unique_ptr<Candidate> candidate(char id);
std::unique_ptr<Candidate> jsoncons_candidate();
void validate_shadow(std::string_view text, std::string_view schema);
nlohmann::ordered_json validator_probe(std::string_view text, std::string_view schema);
}  // namespace s7
