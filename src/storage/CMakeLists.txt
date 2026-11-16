# Storage level (modules 5, 6, library data, trash; ТЗ §12.2.2, §12.7):
#  archive/   - reading input archives, volume detection (ADR-0011)
#  asset_zip/ - own ZIP container writer/reader, commit protocol (M1, ADR-0005/0006)
#  repository/- AssetRepository: the ONLY writer of live ZIP assets (ADR-0025)
#  db/        - index/userdata/state SQLite access, migrations (M2, ADR-0007)
set(AM_STORAGE_SOURCES archive/volume_detect.cpp)
if(AM_WITH_CONTAINER)
  find_package(ZLIB REQUIRED)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  find_package(unofficial-sqlite3 CONFIG REQUIRED)
  find_package(tomlplusplus CONFIG REQUIRED)
  foreach(_am_schema index userdata)
    file(READ "${PROJECT_SOURCE_DIR}/schemas/sql/${_am_schema}.sql" AM_LIBRARY_SCHEMA)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/schemas/sql/${_am_schema}.sql")
    string(REPLACE "\n" "\n)AMSQL\"\nR\"AMSQL(" AM_LIBRARY_SCHEMA_PARTS "${AM_LIBRARY_SCHEMA}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/${_am_schema}_schema.hpp"
      CONTENT "#pragma once\ninline constexpr const char ${_am_schema}_schema_sql[] = R\"AMSQL(${AM_LIBRARY_SCHEMA_PARTS})AMSQL\";\n")
  endforeach()
  file(READ "${PROJECT_SOURCE_DIR}/schemas/sql/state.sql" AM_STATE_SCHEMA_SQL)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/schemas/sql/state.sql")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/state_schema.hpp"
    CONTENT "#pragma once\ninline constexpr const char state_schema_sql[] = R\"AMSQL(${AM_STATE_SCHEMA_SQL})AMSQL\";\n")
  file(READ "${PROJECT_SOURCE_DIR}/schemas/manifest-1.0.schema.json" AM_REPOSITORY_SCHEMA)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/schemas/manifest-1.0.schema.json")
  string(REPLACE "\n" "\n)AMSCHEMA\",\nR\"AMSCHEMA(" AM_REPOSITORY_SCHEMA_PARTS "${AM_REPOSITORY_SCHEMA}")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/manifest_schema.hpp"
    CONTENT "#pragma once\ninline constexpr const char* manifest_schema_parts[] = {R\"AMSCHEMA(${AM_REPOSITORY_SCHEMA_PARTS})AMSCHEMA\"};\n")
  list(APPEND AM_STORAGE_SOURCES asset_zip/source.cpp asset_zip/sha256.cpp
    asset_zip/reader.cpp asset_zip/profile.cpp asset_zip/sink.cpp
    asset_zip/writer.cpp asset_zip/compression_policy.cpp db/state_journal.cpp db/sqlite.cpp db/index_store.cpp commit/protocol.cpp repository/repository.cpp library/library.cpp)
endif()
am_add_module(storage
  SOURCES ${AM_STORAGE_SOURCES}
  DEPENDS am::foundation am::contracts am::domain
  FORBID services processing automation gui)
if(AM_WITH_CONTAINER)
  target_link_libraries(am_storage PRIVATE ZLIB::ZLIB OpenSSL::Crypto)
  target_link_libraries(am_storage PRIVATE unofficial::sqlite3::sqlite3)
  target_link_libraries(am_storage PRIVATE tomlplusplus::tomlplusplus)
  target_include_directories(am_storage PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  if(WIN32)
    target_compile_definitions(am_storage PRIVATE _WIN32_WINNT=0x0A00)
  endif()
endif()
