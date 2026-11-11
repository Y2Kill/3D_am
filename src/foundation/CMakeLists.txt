# Level (а) infrastructure basics: Result/Error, paths (long path), text encodings,
# UUIDv7, signatures. NO third-party dependencies on purpose (CLAUDE.md, ADR-0024).
# Later: hashing, logging, configuration (module 2), platform abstraction (module 1).
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.hpp.in ${PROJECT_BINARY_DIR}/generated/am/foundation/version.hpp @ONLY)

am_add_module(foundation
  SOURCES error.cpp uuid.cpp text_encoding.cpp signature.cpp paths.cpp unicode.cpp json_text.cpp)
target_include_directories(am_foundation PUBLIC $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>)
