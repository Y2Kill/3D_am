#include <iostream>

#include "am/application/logging.hpp"
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  am::application::LoggingOptions options;
  const auto value = std::string_view(argv[1]);
  options.directory = std::filesystem::path(std::u8string(value.begin(), value.end()));
  auto sink = am::application::open_file_log(options);
  if (!sink) return 4;
  AM_LOG(error, "test", "crash.test", "Error before abrupt termination");
  std::cout << "READY" << std::endl;
  std::cin.get();
  return 0;
}
