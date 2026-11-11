#pragma once
#include "zip_internal.hpp"
namespace am::asset_zip::detail {
inline Error write_error(Errc code, std::string_view id, std::string detail) {
  return Error::make(code, "ZIP writer rejected input or operation", std::move(detail))
      .in("storage.asset_zip").op("zip.write").id(std::string(id));
}
inline Error invalid_state() { return write_error(Errc::invalid_argument, "am.zip.write.invalid_state", "Writer state does not allow this operation"); }
inline Error write_oom() { return write_error(Errc::resource_exhausted, "am.zip.out_of_memory", "Allocation failed"); }
[[noreturn]] inline void write_fail(Errc code, std::string_view id, std::string detail) { throw write_error(code, id, std::move(detail)); }
inline void put(std::span<std::byte> bytes, std::size_t p, U64 value, unsigned width) {
  if (p > bytes.size() || width > bytes.size() - p) write_fail(Errc::internal, "am.zip.write.invalid_state", "Internal ZIP header field overflow");
  for (unsigned i = 0; i < width; ++i) bytes[p + i] = static_cast<std::byte>((value >> (8 * i)) & 255);
}
inline void append(std::vector<std::byte>& bytes, U64 value, unsigned width) {
  const auto p = bytes.size(); bytes.resize(p + width); put(bytes, p, value, width);
}
inline void append_bytes(std::vector<std::byte>& bytes, Bytes data) { bytes.insert(bytes.end(), data.begin(), data.end()); }
inline void append_name(std::vector<std::byte>& bytes, std::string_view name) { append_bytes(bytes, std::as_bytes(std::span(name))); }
}  // namespace am::asset_zip::detail
