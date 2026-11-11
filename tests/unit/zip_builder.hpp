#pragma once
// Synthetic byte-level fixtures, deliberately mutable for corruption tests.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zip_test {
using Bytes = std::vector<std::byte>;
struct Entry {
  std::string name = "content/a";
  std::string data = "abc";
  std::uint16_t method = 0, flags = 0x800, made_by = 20;
  std::uint32_t attributes = 0;
  bool zip64 = false, descriptor_signature = true;
  std::optional<Bytes> compressed;
  std::optional<std::uint64_t> declared_size;
  std::optional<std::uint32_t> declared_crc;
};
struct Built {
  Bytes bytes;
  std::vector<std::size_t> local, data, descriptor, central;
  std::size_t cd = 0, end64 = 0, locator = 0, eocd = 0;
};
void put(Bytes& bytes, std::size_t offset, std::uint64_t value, unsigned width);
void append(Bytes& bytes, std::uint64_t value, unsigned width);
Bytes raw_deflate(const std::string& data);
Built build(const std::vector<Entry>& entries = {}, bool zip64 = false,
            const std::string& comment = {}, const std::string& prefix = {});
}  // namespace zip_test
