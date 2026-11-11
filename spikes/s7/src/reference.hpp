#pragma once
#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace s7 {
inline std::size_t hypothesis_max_depth = 1024;
enum class Kind { object, array, string, number, literal };
struct Token {
  Kind kind{};
  std::size_t begin{}, end{}, parent = static_cast<std::size_t>(-1);
  std::string edge, decoded;
  std::vector<std::size_t> children;
};
bool number(std::string_view text);
std::string checked_number(std::string_view text);
struct Document {
  std::string_view text;
  std::vector<Token> tokens;
  bool duplicates = false;
  std::size_t max_depth = 0;
  explicit Document(std::string_view source, std::size_t hypothesis_depth = hypothesis_max_depth);
  std::size_t child(std::size_t parent, std::string_view key) const;
  std::string path(std::size_t node, bool typed = false, bool stable = false) const;
  std::array<std::map<std::string, std::string>, 6> fingerprints(bool stable = false) const;
  std::array<std::map<std::string, std::string_view>, 6> numeric_lexemes(bool stable = false) const;
  std::vector<std::size_t> opaque_roots() const;
  std::string shadow() const;
  std::string edit_title(std::string_view title) const;
  std::string edit_known(bool structural) const;
 private:
  std::vector<int> opaque_groups() const;
};
inline constexpr std::size_t missing = static_cast<std::size_t>(-1);
}  // namespace s7
