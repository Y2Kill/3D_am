add_executable(am_gen_library main.cpp)
target_link_libraries(am_gen_library PRIVATE am::services)
am_apply_defaults(am_gen_library)

target_sources(am_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/test_scale_open.cpp)
target_include_directories(am_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(am_unit_tests PRIVATE AM_PERF_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/perf/tests")

if(Python3_Interpreter_FOUND)
  add_test(NAME perf_gen_smoke
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/performance/test_gen_smoke.py
      --am $<TARGET_FILE:am_cli> --generator $<TARGET_FILE:am_gen_library>
      --root ${PROJECT_SOURCE_DIR}/.cache/perf/smoke --build-type ${CMAKE_BUILD_TYPE})
  set_tests_properties(perf_gen_smoke PROPERTIES LABELS "req:K-112")
  add_test(NAME perf_scale
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/performance/run_scale.py
      --am $<TARGET_FILE:am_cli> --generator $<TARGET_FILE:am_gen_library>
      --root ${PROJECT_SOURCE_DIR}/.cache/perf/scale --build-type ${CMAKE_BUILD_TYPE}
      --storage unspecified --output ${PROJECT_SOURCE_DIR}/.cache/perf/scale-result.json)
  set_tests_properties(perf_scale PROPERTIES LABELS "periodic;req:K-112")
endif()
