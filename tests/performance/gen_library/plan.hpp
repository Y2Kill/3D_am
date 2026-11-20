#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include "am/foundation/paths.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/storage/asset_zip/sha256.hpp"

namespace perf {
namespace fs = std::filesystem;
using Json = am::repository::Json;
template<class T> T take(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(r).value();
}
inline void take(am::Result<void> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
}
struct Random {
  std::uint64_t state;
  std::uint64_t next() {
    auto z = (state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
  }
  // Rejection mapping, explicitly specified rather than library distributions.
  std::uint64_t range(std::uint64_t n) {
    const auto threshold = (std::uint64_t{0} - n) % n;
    for (;;) { const auto x = next(); if (x >= threshold) return x % n; }
  }
  std::string uuid() {
    std::array<unsigned,16> b{};
    for (auto& v : b) v = static_cast<unsigned>(next() & 255);
    b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
    constexpr char hex[] = "0123456789abcdef"; std::string s;
    for (std::size_t i = 0; i < b.size(); ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
      s += hex[b[i] >> 4]; s += hex[b[i] & 15];
    }
    return s;
  }
};
inline std::string digest(am::asset_zip::Sha256& h) {
  const auto bytes = take(h.finish()); std::string s; constexpr char hex[] = "0123456789abcdef";
  for (auto b : bytes) { const auto v = std::to_integer<unsigned>(b); s += hex[v >> 4]; s += hex[v & 15]; }
  return s;
}
inline void update(am::asset_zip::Sha256& h, std::string_view s) { take(h.update(std::as_bytes(std::span(s)))); }
inline std::string sha(std::string_view s) { auto h = take(am::asset_zip::Sha256::create()); update(h,s); return digest(h); }
inline void write(const fs::path& p, std::string_view s) {
  std::ofstream out(am::paths::native(p),std::ios::binary | std::ios::trunc);
  out.write(s.data(),static_cast<std::streamsize>(s.size()));
  if (!out) throw std::runtime_error("Synthetic source write failed");
}
struct Parameters {
  std::uint64_t count = 100, seed = 1, scale = 1, trash_ppm = 10000;
  Json json() const { return {{"plan_version",1},{"count",count},{"seed",seed},{"content_scale",scale},{"trash_ppm",trash_ppm}}; }
};
struct File { std::string path, id, bytes; };
struct Asset {
  std::string id, op, input;
  Json metadata;
  std::vector<File> files;
  bool trash = false;
  Json canonical() const {
    Json out{{"asset_id",id},{"op_id",op},{"input_id",input},{"metadata",metadata},{"trash",trash},{"files",Json::array()}};
    for (const auto& f : files) out["files"].push_back({{"path",f.path},{"file_id",f.id},{"size",f.bytes.size()},{"sha256",sha(f.bytes)}});
    return out;
  }
};
inline std::string vocabulary(std::uint64_t n) {
  static const std::array<std::string,5> words{"Model", "Деталь", "Surface, matte", "模型", "Print & paint"};
  return words[static_cast<std::size_t>(n % words.size())] + " " + std::to_string(n);
}
inline std::uint64_t zipf(Random& r) {
  static const auto cdf = [] {
    std::array<std::uint64_t,500> v{}; std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < v.size(); ++i) { total += 1000000 / (i+1); v[static_cast<std::size_t>(i)] = total; }
    return v;
  }();
  const auto sample = r.range(cdf.back());
  return static_cast<std::uint64_t>(std::upper_bound(cdf.begin(),cdf.end(),sample) - cdf.begin());
}
inline Asset asset(const Parameters& p, std::uint64_t index) {
  Random r{p.seed ^ ((index+1) * UINT64_C(0xd1342543de82ef95))};
  Asset a; a.id = r.uuid(); a.op = r.uuid(); a.input = r.uuid();
  // Exact floor(N*fraction) for every prefix; no dependence on total count.
  a.trash = ((index+1)*p.trash_ppm/1000000 != index*p.trash_ppm/1000000);
  a.metadata = {{"title",vocabulary(index)}};
  if (r.range(2) == 0) a.metadata["description"] = "Synthetic library: " + vocabulary(r.range(500));
  std::set<std::uint64_t> tags; const auto tag_count = r.range(13);
  while (tags.size() < tag_count) tags.insert(zipf(r));
  a.metadata["tags"] = Json::array(); for (auto t : tags) a.metadata["tags"].push_back(vocabulary(t));
  std::set<std::uint64_t> categories; const auto cat_count = r.range(4);
  while (categories.size() < cat_count) categories.insert(r.range(100));
  a.metadata["categories"] = Json::array();
  for (auto c : categories) {
    Json path = Json::array(); for (std::uint64_t d = 0; d <= c%3; ++d) path.push_back(vocabulary((c/(d+1))%100));
    a.metadata["categories"].push_back({{"path",path}});
  }
  a.metadata["authors"] = Json::array(); const auto authors = r.range(3);
  for (std::uint64_t j = 0; j < authors; ++j) a.metadata["authors"].push_back({{"name",vocabulary(r.range(100))},{"role","designer"}});
  if (r.range(3) == 0) a.metadata["license"] = {{"name","Synthetic demonstration"},{"spdx","LicenseRef-Synthetic"}};
  a.metadata["links"] = Json::array(); const auto links = r.range(3);
  for (std::uint64_t j = 0; j < links; ++j) a.metadata["links"].push_back({{"url","https://example.invalid/model/"+std::to_string(index)+"/"+std::to_string(j)},{"rel",j ? "synthetic_reference" : "source"}});
  if (r.range(3) == 0) a.metadata["dates"] = {{"published","2026-01-"+std::string(index%28 < 9 ? "0" : "")+std::to_string(1+index%28)}};
  static constexpr std::array<unsigned,20> counts{1,1,1,2,2,2,3,3,4,4,4,4,5,5,6,8,12,20,32,50};
  static constexpr std::array<std::string_view,9> extensions{"stl","obj","3mf","step","png","jpg","txt","pdf","zip"};
  const auto count = counts[static_cast<std::size_t>(r.range(counts.size()))];
  for (unsigned j = 0; j < count; ++j) {
    File f; f.id = r.uuid();
    const auto depth = r.range(3); for (std::uint64_t d = 0; d < depth; ++d) f.path += "folder_"+std::to_string(d)+"/";
    f.path += "file_"+std::to_string(j)+"."+std::string(extensions[static_cast<std::size_t>(r.range(extensions.size()))]);
    const auto size = static_cast<std::size_t>((64+r.range(16384-64+1))*p.scale);
    f.bytes.resize(size);
    // The fixed little-endian prefix proves uniqueness across seed/index/file.
    const std::array<std::uint64_t,3> identity{p.seed,index,j};
    for (std::size_t k = 0; k < 24; ++k) f.bytes[k] = static_cast<char>((identity[k/8] >> ((k%8)*8)) & 255);
    for (std::size_t k = 24; k < size; ++k) f.bytes[k] = static_cast<char>(r.next() & 255);
    a.files.push_back(std::move(f));
  }
  return a;
}
inline am::library::OpenOptions options(bool assume, am::library::Mode mode = am::library::Mode::read_write) {
  am::library::OpenOptions o; o.mode = mode; o.application_version = "am_gen_library";
  if (assume) { o.capabilities.emplace(); o.capabilities->mode = am::repository::StorageMode::full; }
  return o;
}
inline std::string now() {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm,&t);
#else
  gmtime_r(&t,&tm);
#endif
  std::ostringstream s; s << std::put_time(&tm,"%Y-%m-%dT%H:%M:%S.000Z"); return s.str();
}
inline void create(am::catalog::Catalog& c, const Asset& a, const fs::path& scratch) {
  am::repository::CreateRequest request; request.asset_id = a.id; request.op_id = a.op; request.metadata = a.metadata;
  am::repository::AddFiles add; add.input = {{"id",a.input},{"mode","strict"},{"imported_at",now()},
    {"completeness","complete"},{"objects",Json::array()},{"entries",Json::array()}};
  fs::create_directories(scratch);
  for (std::size_t i = 0; i < a.files.size(); ++i) {
    const auto& f = a.files[i]; const auto source = scratch/std::to_string(i); write(source,f.bytes);
    am::repository::FileInput input; input.path = f.path; input.file_id = f.id; input.source = source; add.files.push_back(std::move(input));
  }
  request.files.push_back(std::move(add));
  const auto result = take(c.create(request));
  if (!result.warnings.empty()) throw std::runtime_error("Catalog create returned index warnings");
}
} // namespace perf
