#include "zip_builder.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <zlib.h>

namespace zip_test {
void put(Bytes& b, std::size_t offset, std::uint64_t value, unsigned width) {
  if (offset > b.size() || width > b.size() - offset) throw std::out_of_range("Fixture patch outside byte buffer");
  for (unsigned i = 0; i < width; ++i) b[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 255);
}
void append(Bytes& b, std::uint64_t value, unsigned width) {
  const auto offset = b.size(); b.resize(offset + width); put(b, offset, value, width);
}
namespace {
void text(Bytes& b, const std::string& s) {
  for (unsigned char c : s) b.push_back(static_cast<std::byte>(c));
}
}
Bytes raw_deflate(const std::string& data) {
  z_stream state{};
  if (deflateInit2(&state, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    throw std::runtime_error("Fixture deflateInit2 failed");
  struct End { z_stream* state; ~End() { deflateEnd(state); } } end{&state};
  state.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data())); state.avail_in = static_cast<uInt>(data.size());
  Bytes out;
  std::array<std::byte, 16384> chunk{};
  int status = Z_OK;
  while (status != Z_STREAM_END) {
    state.next_out = reinterpret_cast<Bytef*>(chunk.data()); state.avail_out = static_cast<uInt>(chunk.size());
    status = deflate(&state, Z_FINISH);
    if (status != Z_OK && status != Z_STREAM_END) throw std::runtime_error("Fixture deflate failed");
    out.insert(out.end(), chunk.begin(), chunk.end() - state.avail_out);
  }
  return out;
}
Built build(const std::vector<Entry>& entries, bool zip64, const std::string& comment, const std::string& prefix) {
  Built b; text(b.bytes, prefix);
  struct Values { std::uint64_t size; std::uint32_t crc; Bytes payload; };
  std::vector<Values> values;
  for (const auto& e : entries) {
    const auto crc = e.declared_crc.value_or(static_cast<std::uint32_t>(::crc32(0, reinterpret_cast<const Bytef*>(e.data.data()), static_cast<uInt>(e.data.size()))));
    Values v{e.declared_size.value_or(e.data.size()), crc, e.compressed.value_or(e.method == 8 ? raw_deflate(e.data) : Bytes{})};
    if (!e.compressed && e.method != 8) text(v.payload, e.data);
    b.local.push_back(b.bytes.size());
    const auto p = b.bytes.size(); b.bytes.resize(p + 30);
    put(b.bytes, p, 0x04034b50, 4); put(b.bytes, p + 4, e.zip64 ? 45 : 20, 2);
    put(b.bytes, p + 6, e.flags, 2); put(b.bytes, p + 8, e.method, 2);
    put(b.bytes, p + 14, e.flags & 8 ? 0 : crc, 4);
    put(b.bytes, p + 18, e.zip64 ? 0xffffffffULL : (e.flags & 8 ? 0 : v.payload.size()), 4);
    put(b.bytes, p + 22, e.zip64 ? 0xffffffffULL : (e.flags & 8 ? 0 : v.size), 4);
    put(b.bytes, p + 26, e.name.size(), 2); put(b.bytes, p + 28, e.zip64 ? 20 : 0, 2);
    text(b.bytes, e.name);
    if (e.zip64) { append(b.bytes, 1, 2); append(b.bytes, 16, 2); append(b.bytes, v.size, 8); append(b.bytes, v.payload.size(), 8); }
    b.data.push_back(b.bytes.size()); b.bytes.insert(b.bytes.end(), v.payload.begin(), v.payload.end());
    b.descriptor.push_back(b.bytes.size());
    if (e.flags & 8) {
      if (e.descriptor_signature) append(b.bytes, 0x08074b50, 4);
      append(b.bytes, crc, 4); append(b.bytes, v.payload.size(), e.zip64 ? 8 : 4); append(b.bytes, v.size, e.zip64 ? 8 : 4);
    }
    values.push_back(std::move(v));
  }
  b.cd = b.bytes.size();
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i]; const auto& v = values[i];
    b.central.push_back(b.bytes.size()); const auto p = b.bytes.size(); b.bytes.resize(p + 46);
    put(b.bytes, p, 0x02014b50, 4); put(b.bytes, p + 4, e.made_by, 2); put(b.bytes, p + 6, e.zip64 ? 45 : 20, 2);
    put(b.bytes, p + 8, e.flags, 2); put(b.bytes, p + 10, e.method, 2); put(b.bytes, p + 16, v.crc, 4);
    put(b.bytes, p + 20, e.zip64 ? 0xffffffffULL : v.payload.size(), 4); put(b.bytes, p + 24, e.zip64 ? 0xffffffffULL : v.size, 4);
    put(b.bytes, p + 28, e.name.size(), 2); put(b.bytes, p + 30, e.zip64 ? 28 : 0, 2);
    put(b.bytes, p + 38, e.attributes, 4); put(b.bytes, p + 42, e.zip64 ? 0xffffffffULL : b.local[i], 4);
    text(b.bytes, e.name);
    if (e.zip64) { append(b.bytes, 1, 2); append(b.bytes, 24, 2); append(b.bytes, v.size, 8); append(b.bytes, v.payload.size(), 8); append(b.bytes, b.local[i], 8); }
  }
  const auto cd_size = b.bytes.size() - b.cd;
  if (zip64) {
    b.end64 = b.bytes.size(); append(b.bytes, 0x06064b50, 4); append(b.bytes, 44, 8);
    append(b.bytes, 45, 2); append(b.bytes, 45, 2); append(b.bytes, 0, 4); append(b.bytes, 0, 4);
    append(b.bytes, entries.size(), 8); append(b.bytes, entries.size(), 8); append(b.bytes, cd_size, 8); append(b.bytes, b.cd, 8);
    b.locator = b.bytes.size(); append(b.bytes, 0x07064b50, 4); append(b.bytes, 0, 4); append(b.bytes, b.end64, 8); append(b.bytes, 1, 4);
  }
  b.eocd = b.bytes.size(); append(b.bytes, 0x06054b50, 4); append(b.bytes, 0, 2); append(b.bytes, 0, 2);
  append(b.bytes, zip64 ? 0xffff : entries.size(), 2); append(b.bytes, zip64 ? 0xffff : entries.size(), 2);
  append(b.bytes, zip64 ? 0xffffffffULL : cd_size, 4); append(b.bytes, zip64 ? 0xffffffffULL : b.cd, 4);
  append(b.bytes, comment.size(), 2); text(b.bytes, comment);
  return b;
}
}  // namespace zip_test
