# Application services (modules 8-13, 16-18): import/analyze, export, catalog and
# index, search, duplicates, preview, external editing sessions.
set(AM_SERVICES_SOURCES import/exclusion_rules.cpp)
if(AM_WITH_CONTAINER)
  list(APPEND AM_SERVICES_SOURCES catalog/indexer.cpp import/scan.cpp import/plan.cpp)
  find_package(jsoncons CONFIG REQUIRED)
  set(AM_IMPORT_SCHEMA "${PROJECT_SOURCE_DIR}/schemas/import-plan.v1.schema.json")
  file(READ "${AM_IMPORT_SCHEMA}" AM_IMPORT_SCHEMA_TEXT)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AM_IMPORT_SCHEMA}")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/import_schema.hpp"
    CONTENT "#pragma once\ninline constexpr char import_schema[] = R\"AMPLAN(${AM_IMPORT_SCHEMA_TEXT})AMPLAN\";\n")
endif()
am_add_module(services
  SOURCES ${AM_SERVICES_SOURCES}
  DEPENDS am::foundation am::contracts am::domain am::jobs am::storage am::processing
  FORBID automation application gui)
if(AM_WITH_CONTAINER)
  target_link_libraries(am_services PRIVATE jsoncons::jsoncons)
  target_include_directories(am_services PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()
