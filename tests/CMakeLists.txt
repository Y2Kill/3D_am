find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  if(NOT AM_FETCH_GTEST)
    message(WARNING "GoogleTest not found and AM_FETCH_GTEST=OFF — tests disabled")
    return()
  endif()
  include(FetchContent)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(am_unit_tests
  unit/test_error.cpp
  unit/test_uuid.cpp
  unit/test_text_encoding.cpp
  unit/test_signature.cpp
  unit/test_paths.cpp
  unit/test_unicode.cpp
  unit/test_json_text.cpp
  unit/test_exclusion.cpp
  unit/test_volumes.cpp
  unit/test_stl_probe.cpp
  unit/test_command_registry.cpp
  unit/test_plugin_abi.cpp)
target_link_libraries(am_unit_tests PRIVATE am::application GTest::gtest_main)
target_compile_definitions(am_unit_tests PRIVATE
  AM_UNICODE_DATA_DIR="${PROJECT_SOURCE_DIR}/third_party/unicode/17.0.0")
if(AM_WITH_CONTAINER)
  find_package(ZLIB REQUIRED)
  find_package(unofficial-sqlite3 CONFIG REQUIRED)
  target_sources(am_unit_tests PRIVATE unit/test_manifest_codec.cpp unit/test_manifest_wide.cpp
    unit/test_manifest_semantics.cpp
    unit/test_asset_service.cpp
    unit/zip_builder.cpp unit/test_zip_reader.cpp unit/test_zip_stream.cpp unit/test_zip_probe.cpp
    unit/test_zip_writer.cpp unit/test_zip_writer_large.cpp unit/test_zip_writer_interop.cpp
    unit/test_state_journal.cpp unit/test_commit_protocol.cpp unit/test_asset_repository.cpp)
  add_executable(am_commit_crash_helper integration/commit_crash_helper.cpp)
  target_link_libraries(am_commit_crash_helper PRIVATE am::storage)
  am_apply_defaults(am_commit_crash_helper)
  add_dependencies(am_unit_tests am_commit_crash_helper)
  add_executable(am_repository_crash_helper integration/asset_repository_crash_helper.cpp)
  target_link_libraries(am_repository_crash_helper PRIVATE am::storage)
  am_apply_defaults(am_repository_crash_helper)
  add_dependencies(am_unit_tests am_repository_crash_helper)
  target_link_libraries(am_unit_tests PRIVATE unofficial::sqlite3::sqlite3)
  target_link_libraries(am_unit_tests PRIVATE ZLIB::ZLIB)
  target_compile_definitions(am_unit_tests PRIVATE
    AM_MANIFEST_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/manifest/s7"
    AM_MANIFEST_SCHEMA="${PROJECT_SOURCE_DIR}/schemas/manifest-1.0.draft.schema.json"
    AM_ZIP_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/zip"
    AM_ZIP_CACHE="${PROJECT_SOURCE_DIR}/.cache/zip-tests"
    AM_COMMIT_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/commit-tests"
    AM_REPO_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/repo-tests"
    AM_ASSET_SERVICE_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/cli-tests/api"
    AM_REPO_HELPER="$<TARGET_FILE:am_repository_crash_helper>"
    AM_COMMIT_HELPER="$<TARGET_FILE:am_commit_crash_helper>")
  if(WIN32)
    target_compile_definitions(am_unit_tests PRIVATE _WIN32_WINNT=0x0A00)
  endif()
  if(WIN32)
    target_link_libraries(am_unit_tests PRIVATE psapi)
  endif()
endif()
am_apply_defaults(am_unit_tests)
gtest_discover_tests(am_unit_tests DISCOVERY_MODE PRE_TEST)

# Python-based checks: architecture (ADR-0024) and the draft manifest schema (ADR-0036 item 8).
# - Default: a missing jsonschema module makes schema_check FAIL (not a silent SKIP).
# - Local opt-out: -DAM_ALLOW_PYTHON_SKIP=ON turns that into an explicit SKIP.
# - CI: -DAM_REQUIRE_PYTHON_TESTS=ON makes a missing interpreter or module a configure error.
option(AM_PYTHON_TESTS "Register Python-based checks (arch_check, schema_check)" ON)
option(AM_REQUIRE_PYTHON_TESTS "Fail configuration if Python checks cannot run (CI)" OFF)
option(AM_ALLOW_PYTHON_SKIP "Locally report schema_check as SKIP when jsonschema is missing" OFF)
set(AM_PYTHON_EXTRA_PATH "${PROJECT_SOURCE_DIR}/.cache/pylib" CACHE PATH
    "Extra Python path for test-only modules such as jsonschema")

if(AM_REQUIRE_PYTHON_TESTS AND (NOT AM_PYTHON_TESTS OR AM_ALLOW_PYTHON_SKIP))
  message(FATAL_ERROR "AM_REQUIRE_PYTHON_TESTS=ON contradicts AM_PYTHON_TESTS=OFF / AM_ALLOW_PYTHON_SKIP=ON")
endif()

if(AM_PYTHON_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(NOT Python3_Interpreter_FOUND)
    if(AM_REQUIRE_PYTHON_TESTS)
      message(FATAL_ERROR "Python 3 is required for arch_check and schema_check")
    endif()
    message(WARNING "Python 3 not found: arch_check and schema_check are not registered")
  else()
    add_test(NAME golden_regen_check
      COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/make_golden_zips.py
        --root ${PROJECT_SOURCE_DIR} --check)
    set_tests_properties(golden_regen_check PROPERTIES LABELS "req:K-3;req:K-6;req:ADR-0035")
    if(AM_WITH_CONTAINER)
      add_test(NAME message_catalog_check
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/tools/message_catalog_check.py --root ${PROJECT_SOURCE_DIR})
      set_tests_properties(message_catalog_check PROPERTIES LABELS "req:ADR-0032;req:ADR-0034")
      if(TARGET am_cli)
      add_test(NAME golden_zip
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/golden/run_golden.py
          --am $<TARGET_FILE:am_cli> --corpus ${PROJECT_SOURCE_DIR}/tests/fixtures/golden)
      set_tests_properties(golden_zip PROPERTIES LABELS "req:K-3;req:K-6;req:ADR-0035")
      foreach(_am_cli_case create_roundtrip create_refusals validate_verify inspect_minor extract json_examples development)
        add_test(NAME cli_${_am_cli_case}
          COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/cli/test_asset_cli.py
            --am $<TARGET_FILE:am_cli> --root ${PROJECT_SOURCE_DIR}/.cache/cli-tests --case ${_am_cli_case})
        set_tests_properties(cli_${_am_cli_case} PROPERTIES LABELS "req:K-15;req:K-85;req:K-3;req:K-6")
      endforeach()
      endif()
      add_test(NAME zip_writer_interop
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/check_written_zips.py
          ${PROJECT_SOURCE_DIR}/.cache/zip-tests/written --generator $<TARGET_FILE:am_unit_tests>)
      set_tests_properties(zip_writer_interop PROPERTIES LABELS "req:ADR-0006;req:ADR-0035")
    endif()
    add_test(NAME arch_check
      COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/arch_check/arch_check.py
              --root ${PROJECT_SOURCE_DIR} --build-dir ${PROJECT_BINARY_DIR})

    add_test(NAME unicode_tables_check
      COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/unicode/gen_unicode_tables.py --check
              --data-dir ${PROJECT_SOURCE_DIR}/third_party/unicode/17.0.0
              --output ${PROJECT_SOURCE_DIR}/src/foundation/unicode_tables.inc)
    set_tests_properties(unicode_tables_check PROPERTIES LABELS "req:ADR-0035")

    set(_am_schema_args --root ${PROJECT_SOURCE_DIR} --pylib ${AM_PYTHON_EXTRA_PATH})
    if(AM_REQUIRE_PYTHON_TESTS)
      execute_process(
        COMMAND ${Python3_EXECUTABLE} -I -c
                "import sys; sys.path.insert(0, sys.argv[1]); import jsonschema" ${AM_PYTHON_EXTRA_PATH}
        RESULT_VARIABLE _am_jsonschema_rc OUTPUT_QUIET ERROR_QUIET)
      if(NOT _am_jsonschema_rc EQUAL 0)
        message(FATAL_ERROR "Python module 'jsonschema' is required for schema_check (pip install jsonschema)")
      endif()
    elseif(AM_ALLOW_PYTHON_SKIP)
      list(APPEND _am_schema_args --allow-skip)
    endif()
    add_test(NAME schema_check
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/schema/schema_check.py ${_am_schema_args})
    set_tests_properties(schema_check PROPERTIES SKIP_RETURN_CODE 77 LABELS "schema;req:ADR-0036;req:ADR-0037")

    # SQLite contracts (standard library only): exact uint64 TEXT in index.sql (ADR-0037 item 10).
    add_test(NAME sql_check
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/schema/sql_check.py --root ${PROJECT_SOURCE_DIR})
    set_tests_properties(sql_check PROPERTIES LABELS "schema;req:ADR-0037")
  endif()
endif()
