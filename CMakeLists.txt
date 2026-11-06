cmake_minimum_required(VERSION 3.28)

project(am
  VERSION 0.1.0
  DESCRIPTION "Catalog and manager for 3D/2D printing, CNC and laser resources"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)   # ADR-0001: no C++20 modules
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules)
include(AmOptions)
include(AmWarnings)
include(AmModule)

# ---------------------------------------------------------------------------
# Architecture levels (ADR-0024, ТЗ §12.1, §12.6). One CMake target per level.
# Order below follows the allowed dependency direction; cycles are forbidden
# and checked by tools/arch_check (also registered as a test).
# ---------------------------------------------------------------------------
add_subdirectory(src/foundation)      # am_foundation: no third-party deps
add_subdirectory(src/contracts)       # am_contracts: narrow interfaces (header-only)
add_subdirectory(src/domain)          # am_domain: asset model, manifest (M1)
add_subdirectory(src/jobs)            # am_jobs: job infrastructure (M3)
add_subdirectory(src/storage)         # am_storage: own ZIP, input archives, repository, DB
add_subdirectory(src/processing)      # am_processing: format handlers, analyzers, converters
add_subdirectory(src/services)        # am_services: import/analyze, export, catalog, search…
add_subdirectory(src/automation)      # am_automation: command registry, scenarios
add_subdirectory(src/plugin_runtime)  # am_plugin_runtime: plugin discovery/loading
if(AM_WITH_AI)
  add_subdirectory(src/ai)            # am_ai: optional, absent from minimal builds
endif()
add_subdirectory(src/application)     # am_application facade + cli/gui executables

if(AM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "am ${PROJECT_VERSION}: tests=${AM_BUILD_TESTS} cli=${AM_BUILD_CLI} gui=${AM_BUILD_GUI} "
               "container=${AM_WITH_CONTAINER} render3d=${AM_WITH_RENDER3D} ai=${AM_WITH_AI}")
