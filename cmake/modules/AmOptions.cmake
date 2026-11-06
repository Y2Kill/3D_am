# Build options (ТЗ §12.6.4, §12.6.6). Turning an option OFF really removes the
# corresponding code and third-party dependencies from the build.
# Everything beyond the dependency-free levels is opt-in, so a bare checkout
# with only a compiler and CMake always builds.
option(AM_BUILD_TESTS     "Build unit tests (GoogleTest)"                          ON)
option(AM_BUILD_CLI       "Build the console application"                          ON)
option(AM_BUILD_GUI       "Build Qt 6 Widgets GUI (needs Qt 6.8+)"                 OFF)
option(AM_WITH_CONTAINER  "Build asset container storage (minizip-ng, json, ssl)" OFF)
option(AM_WITH_RENDER3D   "Build heavy 3D rendering/preview generation"           OFF)
option(AM_WITH_AI         "Build optional AI adapter layer (ТЗ §11)"              OFF)
option(AM_FETCH_GTEST     "Download GoogleTest with FetchContent if not found"    ON)
option(AM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"                  OFF)
option(AM_ENABLE_ASAN     "Enable AddressSanitizer"                               OFF)

set(AM_QT_ROOT "" CACHE PATH "Qt kit root, e.g. C:/Qt/6.10.2/msvc2022_64")
if(AM_QT_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${AM_QT_ROOT}")
endif()

if(AM_WITH_RENDER3D AND NOT AM_BUILD_GUI)
  message(STATUS "AM_WITH_RENDER3D without GUI: offscreen preview generation only")
endif()
