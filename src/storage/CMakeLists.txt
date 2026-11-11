# Storage level (modules 5, 6, library data, trash; ТЗ §12.2.2, §12.7):
#  archive/   - reading input archives, volume detection (ADR-0011)
#  asset_zip/ - own ZIP container writer/reader, commit protocol (M1, ADR-0005/0006)
#  repository/- AssetRepository: the ONLY writer of live ZIP assets (ADR-0025)
#  db/        - index/userdata/state SQLite access, migrations (M2, ADR-0007)
set(AM_STORAGE_SOURCES archive/volume_detect.cpp)
if(AM_WITH_CONTAINER)
  find_package(ZLIB REQUIRED)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  list(APPEND AM_STORAGE_SOURCES asset_zip/source.cpp asset_zip/sha256.cpp
    asset_zip/reader.cpp asset_zip/profile.cpp asset_zip/sink.cpp
    asset_zip/writer.cpp asset_zip/compression_policy.cpp)
endif()
am_add_module(storage
  SOURCES ${AM_STORAGE_SOURCES}
  DEPENDS am::foundation am::contracts am::domain
  FORBID services processing automation gui)
if(AM_WITH_CONTAINER)
  target_link_libraries(am_storage PRIVATE ZLIB::ZLIB OpenSSL::Crypto)
endif()
