#include "zip.hpp"
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <minizip-ng/mz.h>
#include <minizip-ng/mz_strm.h>
#include <minizip-ng/mz_zip.h>
#include <minizip-ng/mz_zip_rw.h>
namespace s7 {
namespace {
void check(int32_t code) { if (code != MZ_OK) throw std::runtime_error("minizip error " + std::to_string(code)); }
std::string path_utf8(const std::filesystem::path& p) { const auto u = p.u8string(); return {u.begin(), u.end()}; }
struct Reader { void* h = mz_zip_reader_create(); ~Reader() { if (h) { mz_zip_reader_close(h); mz_zip_reader_delete(&h); } } };
struct Writer { void* h = mz_zip_writer_create(); ~Writer() { if (h) { mz_zip_writer_close(h); mz_zip_writer_delete(&h); } } };
}
std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open file");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
Entries read_zip(const std::filesystem::path& path) {
  Reader reader;
  check(mz_zip_reader_open_file(reader.h, path_utf8(path).c_str()));
  Entries entries;
  int32_t status = mz_zip_reader_goto_first_entry(reader.h);
  while (status == MZ_OK) {
    mz_zip_file* info = nullptr;
    check(mz_zip_reader_entry_get_info(reader.h, &info));
    const auto length = mz_zip_reader_entry_save_buffer_length(reader.h);
    if (length < 0) throw std::runtime_error("unsupported ZIP entry size");
    std::string bytes(static_cast<std::size_t>(length), '\0');
    check(mz_zip_reader_entry_save_buffer(reader.h, bytes.data(), length));
    if (!entries.emplace(info->filename, std::move(bytes)).second) throw std::runtime_error("duplicate ZIP member");
    status = mz_zip_reader_goto_next_entry(reader.h);
  }
  if (status != MZ_END_OF_LIST) check(status);
  return entries;
}
void write_zip(const std::filesystem::path& path, const Entries& entries) {
  Writer writer;
  check(mz_zip_writer_open_file(writer.h, path_utf8(path).c_str(), 0, 0));
  auto add = [&](const std::string& name, const std::string& data) {
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) throw std::runtime_error("ZIP entry too large for spike");
    mz_zip_file info{};
    info.filename = name.c_str();
    info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
    info.flag = MZ_ZIP_FLAG_UTF8;
    check(mz_zip_writer_add_buffer(writer.h, data.data(), static_cast<int32_t>(data.size()), &info));
  };
  // Keep the manifest first, as required by the asset writer convention.
  if (const auto manifest = entries.find("manifest.json"); manifest != entries.end()) add(manifest->first, manifest->second);
  for (const auto& [name, data] : entries) if (name != "manifest.json") add(name, data);
  check(mz_zip_writer_close(writer.h));
  mz_zip_writer_delete(&writer.h);
}
}  // namespace s7
