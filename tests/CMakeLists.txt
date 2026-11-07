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
  unit/test_exclusion.cpp
  unit/test_volumes.cpp
  unit/test_stl_probe.cpp
  unit/test_command_registry.cpp
  unit/test_plugin_abi.cpp)
target_link_libraries(am_unit_tests PRIVATE am::application GTest::gtest_main)
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
    add_test(NAME arch_check
      COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/arch_check/arch_check.py
              --root ${PROJECT_SOURCE_DIR} --build-dir ${PROJECT_BINARY_DIR})

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
    set_tests_properties(schema_check PROPERTIES SKIP_RETURN_CODE 77 LABELS "schema;req:ADR-0036")
  endif()
endif()
