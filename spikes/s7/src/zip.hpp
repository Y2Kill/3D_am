#pragma once
#include <filesystem>
#include <map>
#include <string>
namespace s7 {
using Entries = std::map<std::string, std::string>;
Entries read_zip(const std::filesystem::path& path);
void write_zip(const std::filesystem::path& path, const Entries& entries);
std::string read_file(const std::filesystem::path& path);
}  // namespace s7
