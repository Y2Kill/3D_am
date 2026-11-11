#include "am/storage/asset_zip/compression_policy.hpp"
#include "writer_internal.hpp"
namespace am::asset_zip {
Result<CompressionMethod> choose_method(std::string_view name, std::span<const std::byte> sample) {
  using namespace detail;
  try {
    // ADR-0005; refine this list using measurements.
    constexpr std::array<std::string_view, 16> compressed{"jpg", "jpeg", "png", "webp", "gif", "mp4", "mkv", "webm", "zip", "7z", "rar", "gz", "bz2", "xz", "zst", "3mf"};
    const auto dot = name.find_last_of('.');
    if (dot != std::string_view::npos) {
      std::string ext(name.substr(dot + 1));
      for (auto& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
      if (!ext.empty() && std::find(compressed.begin(), compressed.end(), ext) != compressed.end()) return CompressionMethod::stored;
    }
    if (sample.empty()) return CompressionMethod::deflate;
    if (sample.size() > 65536) return write_error(Errc::invalid_argument, "am.zip.write.invalid_state", "Compression sample exceeds 64 KiB");
    z_stream stream{};
    const auto status = deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (status == Z_MEM_ERROR) return write_oom();
    if (status != Z_OK) return invalid_state();
    struct End { z_stream* stream; ~End() { deflateEnd(stream); } } end{&stream};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(sample.data())); stream.avail_in = static_cast<uInt>(sample.size());
    std::array<std::byte, 16384> output{};
    U64 count = 0; int next = Z_OK;
    do {
      stream.next_out = reinterpret_cast<Bytef*>(output.data()); stream.avail_out = static_cast<uInt>(output.size());
      next = deflate(&stream, Z_FINISH);
      if (next == Z_MEM_ERROR) return write_oom();
      if (next != Z_OK && next != Z_STREAM_END) return invalid_state();
      count += output.size() - stream.avail_out;
    } while (next != Z_STREAM_END);
    return count * 100 > sample.size() * 95 ? CompressionMethod::stored : CompressionMethod::deflate;
  } catch (const std::bad_alloc&) { return write_oom(); }
  catch (const std::exception& e) { return write_error(Errc::io_error, "am.zip.io_error", e.what()); }
}
}  // namespace am::asset_zip
