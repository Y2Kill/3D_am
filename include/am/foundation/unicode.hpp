#pragma once
// Pinned Unicode normalization and default simple folding (ADR-0035 p.8).

#include <string>
#include <string_view>

#include "am/foundation/result.hpp"

namespace am::unicode {

inline constexpr std::string_view kUnicodeVersion = "17.0.0";

bool is_valid_utf8(std::string_view s);
Result<std::string> nfc(std::string_view utf8);
Result<std::string> simple_casefold(std::string_view utf8);
// NFC(simple_casefold(NFC(s))); invalid UTF-8 keeps its bytes with ASCII folding.
std::string path_collision_key(std::string_view s);

}  // namespace am::unicode
