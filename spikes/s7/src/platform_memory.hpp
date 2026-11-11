#pragma once
#include <cstdint>
#include <filesystem>
namespace s7 {
struct Memory { std::uint64_t peak_working_set{}, peak_pagefile{}; };
void initialize_process();
Memory memory();
void atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to);
}  // namespace s7
