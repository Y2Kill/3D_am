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

# Architecture checks (ADR-0024, ТЗ §12.9.1): #include rules + CMake graph cycles.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME arch_check
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/arch_check/arch_check.py
            --root ${PROJECT_SOURCE_DIR} --build-dir ${PROJECT_BINARY_DIR})
else()
  message(WARNING "Python 3 not found: arch_check test is not registered")
endif()
