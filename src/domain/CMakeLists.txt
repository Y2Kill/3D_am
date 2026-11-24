# Domain model (module 3, 4, 7 types): asset, files, groups, sources, IDs, revisions,
# manifest <-> JSON mapping and schema validation. Must NOT depend on ZIP or DB
# libraries (ТЗ §12.1.3, §12.2.1).
if(AM_WITH_CONTAINER)
  find_package(nlohmann_json CONFIG REQUIRED)
  find_package(jsoncons CONFIG REQUIRED)
  file(READ "${PROJECT_SOURCE_DIR}/schemas/manifest-1.0.schema.json" AM_PUBLISHED_SCHEMA)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/schemas/manifest-1.0.schema.json")
  string(REPLACE "\n" "\n)AMSCHEMA\",\nR\"AMSCHEMA(" AM_PUBLISHED_SCHEMA_PARTS "${AM_PUBLISHED_SCHEMA}")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/published_manifest_schema.hpp"
    CONTENT "#pragma once\n#include <string_view>\ninline constexpr std::string_view published_schema_data[] = {R\"AMSCHEMA(${AM_PUBLISHED_SCHEMA_PARTS})AMSCHEMA\"};\n")
  am_add_module(domain
    SOURCES manifest/codec.cpp manifest/schema_validator.cpp
      manifest/semantics.cpp manifest/semantics_syntax.cpp
    DEPENDS am::foundation am::contracts nlohmann_json::nlohmann_json
    FORBID storage services gui)
  target_link_libraries(am_domain PRIVATE jsoncons::jsoncons)
  target_include_directories(am_domain PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
else()
  am_add_module(domain
    DEPENDS am::foundation am::contracts
    FORBID storage services gui)
endif()
