#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "am/foundation/result.hpp"
namespace am::application {
struct DevelopmentOutput {
  int exit_code = 0;
  std::string text;
};
Result<DevelopmentOutput> development_command(std::string_view command, const std::vector<std::string>& arguments);
}  // namespace am::application
