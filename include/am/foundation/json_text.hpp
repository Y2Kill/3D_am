#pragma once
// Strict UTF-8 JSON text, without numeric conversion or input-depth recursion.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include "am/foundation/result.hpp"

namespace am::json {
inline constexpr std::size_t no_node = static_cast<std::size_t>(-1);
enum class Kind { object, array, string, number, literal };
struct Node {
  Kind kind = Kind::literal;
  std::size_t begin = 0, end = 0, parent = no_node;
  // Object key (decoded); empty for array children, whose index is separate.
  std::string edge;
  std::size_t index = 0;
  std::string decoded;
  std::vector<std::size_t> children;
};
struct Tree { std::vector<Node> nodes; };
struct ParseLimits {
  std::size_t max_depth = 1024;                 // hypothesis, O-17
  std::size_t max_bytes = 128 * 1024 * 1024;    // hypothesis, O-17
};
// Spans refer to the caller's text. Tree owns all decoded strings and edges.
// Depth counts enclosing containers, including a container at the root.
Result<Tree> parse_strict(std::string_view text, const ParseLimits& limits = {});
bool is_json_number(std::string_view text) noexcept;
}  // namespace am::json
