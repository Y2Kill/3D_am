#pragma once
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/storage/db/sqlite.hpp"
#include "am/storage/db/index_store.hpp"
#include "am/storage/asset_zip/sha256.hpp"

namespace fault_test {
namespace fs = std::filesystem;
using Json = am::repository::Json;
template<class T> T take(am::Result<T> r) { if (!r) throw std::runtime_error(am::to_log_string(r.error())); return std::move(*r); }
inline void take(am::Result<void> r) { if (!r) throw std::runtime_error(am::to_log_string(r.error())); }
inline am::library::OpenOptions writable() {
  am::library::OpenOptions o;
  o.capabilities.emplace();
  o.capabilities->mode = am::repository::StorageMode::full;
  return o;
}
inline void write(const fs::path& path, std::string_view data) {
  fs::create_directories(path.parent_path());
  std::ofstream f(am::paths::native(path),std::ios::binary);
  f.write(data.data(),static_cast<std::streamsize>(data.size()));
  if (!f) throw std::runtime_error("Cannot write synthetic fixture");
}
inline std::string bytes(const fs::path& path) {
  std::ifstream f(am::paths::native(path),std::ios::binary);
  return {std::istreambuf_iterator<char>(f),{}};
}
inline am::asset_zip::Sha256Digest hash(const fs::path& path) {
  const auto data=bytes(path);
  auto sha=take(am::asset_zip::Sha256::create());
  take(sha.update(std::as_bytes(std::span(data))));
  return take(sha.finish());
}
inline fs::path asset_path(const fs::path& root, std::string_view id) {
  return root/"assets"/std::string(id.substr(34))/(std::string(id)+".zip");
}
inline am::repository::CreateRequest request(const fs::path& source, std::string title="Fault fixture") {
  am::repository::CreateRequest r;
  r.metadata=Json{{"title",title}};
  am::repository::AddFiles add;
  add.input={{"id",am::Uuid::v4().to_string()},{"imported_at","2026-11-27T00:00:00Z"},
             {"mode","strict"},{"completeness","complete"},{"objects",Json::array()},{"entries",Json::array()}};
  am::repository::FileInput file;
  file.path="part.stl";
  file.source=source;
  add.files.push_back(file);
  r.files.push_back(std::move(add));
  return r;
}
struct World {
  fs::path work=fs::path(AM_FAULT_TEST_ROOT)/am::Uuid::v4().to_string();
  fs::path root=work/"library", source=work/"source.bin";
  am::repository::ChangeResult asset;
  World() {
    fs::create_directories(work);
    write(source,std::string(8192,'x'));
    take(am::library::Library::init(root));
    auto catalog=take(am::catalog::Catalog::open(root,writable()));
    asset=take(catalog.create(request(source))).change;
    auto user=am::db::Connection::borrow(catalog.library().database(am::library::DatabaseKind::userdata));
    take(user.exec("INSERT INTO collections(coll_id,name,kind,created_at) VALUES('confirmed','Favorite','favorites','2020-01-01T00:00:00Z')"));
  }
  fs::path zip() const { return asset_path(root,asset.ref.asset_id); }
  fs::path orphan() const {
    const auto p=root/".am/.am-tmp"/(am::Uuid::v4().to_string()+".zip");
    write(p,"Unconfirmed partial ZIP");
    fs::last_write_time(p,fs::file_time_type::clock::now()-std::chrono::hours(72)); // hypothesis: older than retention
    return p;
  }
  void unclose() {
    auto l=take(am::library::Library::open(root,writable()));
    am::db::IndexStore store(l.database(am::library::DatabaseKind::index));
    take(store.set_meta("writer_session","open"));
  }
};
}  // namespace fault_test
