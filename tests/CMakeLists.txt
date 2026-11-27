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
target_sources(am_unit_tests PRIVATE unit/test_jobs.cpp unit/test_log.cpp unit/test_export_policy.cpp)
target_compile_definitions(am_unit_tests PRIVATE AM_LOG_RECORD_ROOT="${PROJECT_SOURCE_DIR}/.cache/log-record-tests")
target_link_libraries(am_unit_tests PRIVATE am::application GTest::gtest_main)
target_sources(am_unit_tests PRIVATE unit/test_node_info.cpp)
target_compile_definitions(am_unit_tests PRIVATE AM_NODE_INFO_ROOT="${PROJECT_SOURCE_DIR}/.cache/node-info-tests")
target_compile_definitions(am_unit_tests PRIVATE
  AM_UNICODE_DATA_DIR="${PROJECT_SOURCE_DIR}/third_party/unicode/17.0.0")
if(AM_WITH_CONTAINER)
  target_sources(am_unit_tests PRIVATE unit/test_fault_sqlite.cpp unit/test_fault_state.cpp unit/test_fault_space.cpp unit/test_fault_pipeline.cpp)
  target_compile_definitions(am_unit_tests PRIVATE AM_FAULT_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/fault-tests")
  add_executable(am_fault_process_helper unit/fault_process_helper.cpp unit/zip_builder.cpp)
  target_link_libraries(am_fault_process_helper PRIVATE am::application ZLIB::ZLIB)
  target_compile_definitions(am_fault_process_helper PRIVATE
    AM_FAULT_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/fault-tests"
    AM_GOLDEN_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/golden")
  am_apply_defaults(am_fault_process_helper)
  add_executable(am_small_volume_tests unit/test_small_volume.cpp)
  target_link_libraries(am_small_volume_tests PRIVATE am::application GTest::gtest_main)
  target_compile_definitions(am_small_volume_tests PRIVATE AM_FAULT_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/fault-tests")
  am_apply_defaults(am_small_volume_tests)
  if(WIN32)
    target_sources(am_fault_process_helper PRIVATE ${PROJECT_SOURCE_DIR}/src/application/common/am.manifest)
    target_sources(am_small_volume_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/application/common/am.manifest)
  endif()
  target_sources(am_unit_tests PRIVATE unit/test_export_selection.cpp)
  target_sources(am_unit_tests PRIVATE unit/test_export.cpp)
  target_compile_definitions(am_unit_tests PRIVATE AM_EXPORT_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/export-tests")
  if(TARGET am_cli)
    find_package(CLI11 CONFIG REQUIRED)
    add_executable(am_diag_export_test_driver ${PROJECT_SOURCE_DIR}/src/application/cli/main.cpp cli/diagnostic_node_data_hook.cpp)
    target_link_libraries(am_diag_export_test_driver PRIVATE am::application am::foundation CLI11::CLI11)
    target_compile_definitions(am_diag_export_test_driver PRIVATE AM_DIAGNOSTIC_CLI_TEST_HOOK=1)
    am_apply_defaults(am_diag_export_test_driver)
    if(WIN32)
      target_sources(am_diag_export_test_driver PRIVATE ${PROJECT_SOURCE_DIR}/src/application/common/am.manifest)
    endif()
  endif()
  target_sources(am_unit_tests PRIVATE unit/test_import_scan.cpp unit/test_import_plan.cpp unit/test_import_plan_view.cpp unit/test_import_run.cpp unit/test_import_zip.cpp)
  target_compile_definitions(am_unit_tests PRIVATE AM_IMPORT_SCAN_ROOT="${PROJECT_SOURCE_DIR}/.cache/import-scan-tests" AM_IMPORT_PLAN_CASES="${PROJECT_SOURCE_DIR}/.cache/import-plan-cases")
  target_sources(am_unit_tests PRIVATE unit/test_logging_services.cpp)
  add_executable(am_log_crash_helper integration/log_crash_helper.cpp)
  target_link_libraries(am_log_crash_helper PRIVATE am::application)
  am_apply_defaults(am_log_crash_helper)
  if(WIN32)
    target_sources(am_log_crash_helper PRIVATE ${PROJECT_SOURCE_DIR}/src/application/common/am.manifest)
  endif()
  add_dependencies(am_unit_tests am_log_crash_helper)
  target_compile_definitions(am_unit_tests PRIVATE AM_LOG_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/log-service-tests" AM_DIAG_CASE_ROOT="${PROJECT_SOURCE_DIR}/.cache/diag-export-cases")
  if(WIN32)
    target_sources(am_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/application/common/am.manifest)
  endif()
  target_compile_definitions(am_unit_tests PRIVATE AM_JOBS_STORAGE_TESTS=1)
  target_sources(am_unit_tests PRIVATE unit/test_jobs_services.cpp)
  find_package(ZLIB REQUIRED)
  find_package(unofficial-sqlite3 CONFIG REQUIRED)
  target_sources(am_unit_tests PRIVATE unit/test_manifest_codec.cpp unit/test_manifest_wide.cpp
    unit/test_manifest_semantics.cpp
    unit/test_asset_service.cpp
    unit/test_library.cpp unit/test_job_store.cpp
    unit/test_sqlite.cpp unit/test_indexer.cpp unit/test_index_scale.cpp unit/test_database_checks.cpp unit/test_sync.cpp unit/test_trash.cpp unit/test_import_ready.cpp unit/test_independent_copy.cpp unit/test_open_recovery.cpp
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
  add_executable(am_import_crash_helper unit/test_import_ready.cpp)
  target_compile_definitions(am_import_crash_helper PRIVATE AM_IMPORT_HELPER=1)
  target_link_libraries(am_import_crash_helper PRIVATE am::application)
  am_apply_defaults(am_import_crash_helper)
  add_dependencies(am_unit_tests am_import_crash_helper)
  target_compile_definitions(am_unit_tests PRIVATE AM_IMPORT_HELPER_PATH="$<TARGET_FILE:am_import_crash_helper>")
  add_executable(am_open_recovery_helper unit/test_open_recovery.cpp)
  target_compile_definitions(am_open_recovery_helper PRIVATE AM_OPEN_RECOVERY_HELPER=1)
  target_link_libraries(am_open_recovery_helper PRIVATE am::application)
  am_apply_defaults(am_open_recovery_helper)
  add_dependencies(am_unit_tests am_open_recovery_helper)
  target_compile_definitions(am_unit_tests PRIVATE AM_OPEN_HELPER_PATH="$<TARGET_FILE:am_open_recovery_helper>")
  target_link_libraries(am_unit_tests PRIVATE unofficial::sqlite3::sqlite3)
  target_link_libraries(am_unit_tests PRIVATE ZLIB::ZLIB)
  target_compile_definitions(am_unit_tests PRIVATE
    AM_INDEX_SCHEMA="${PROJECT_SOURCE_DIR}/schemas/sql/index.sql"
    AM_STATE_JOB_SCHEMA="${PROJECT_SOURCE_DIR}/schemas/sql/state.sql"
    AM_MANIFEST_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/manifest/s7"
    AM_MANIFEST_SCHEMA="${PROJECT_SOURCE_DIR}/schemas/manifest-1.0.schema.json"
    AM_ZIP_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/zip"
    AM_ZIP_CACHE="${PROJECT_SOURCE_DIR}/.cache/zip-tests"
    AM_COMMIT_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/commit-tests"
    AM_REPO_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/repo-tests"
    AM_ASSET_SERVICE_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/cli-tests/api"
    AM_LIBRARY_TEST_ROOT="${PROJECT_SOURCE_DIR}/.cache/library-tests"
    AM_GOLDEN_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/golden"
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
gtest_discover_tests(am_unit_tests DISCOVERY_MODE PRE_TEST
  TEST_FILTER "*-ExportStreaming.*:FaultSpace.*:FaultSqlitePipeline.*"
  XML_OUTPUT_DIR "${PROJECT_BINARY_DIR}/gtest-xml")

if(AM_WITH_CONTAINER)
  add_test(NAME export_streaming_measurement
    COMMAND am_unit_tests --gtest_filter=ExportStreaming.IncompressibleSizesRecordPeakAndResidualMemory)
  set_tests_properties(export_streaming_measurement PROPERTIES LABELS "periodic;req:K-116;req:NFR-14.2.3" TIMEOUT 900)
endif()

# Python-based checks: architecture (ADR-0024) and the manifest schema 1.0 (ADR-0036 item 8, ADR-0039).
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
    if(WIN32)
      add_test(NAME small_volume_script
        COMMAND powershell.exe -NoProfile -NonInteractive -Command
          "& ([scriptblock]::Create([IO.File]::ReadAllText('tests/unit/test_small_volume_script.ps1')))")
      set_tests_properties(small_volume_script PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "fault;req:K-117;req:K-122" TIMEOUT 60)
    endif()
    add_test(NAME req_report_selftest
      COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/coverage/req_report.py --selftest)
    set_tests_properties(req_report_selftest PROPERTIES LABELS "req:ADR-0029")
    add_test(NAME golden_regen_check
      COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/make_golden_zips.py
        --root ${PROJECT_SOURCE_DIR} --check)
    set_tests_properties(golden_regen_check PROPERTIES LABELS "req:K-3;req:K-6;req:ADR-0035")
    if(AM_WITH_CONTAINER)
      if(TARGET am_diag_export_test_driver)
        foreach(_am_diag_case json text unchanged)
          add_test(NAME cli_diagnostic_placeholders_${_am_diag_case}
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/cli/test_diagnostic_placeholders_cli.py
              --am $<TARGET_FILE:am_cli> --driver $<TARGET_FILE:am_diag_export_test_driver>
              --root ${PROJECT_SOURCE_DIR}/.cache/diagnostic-placeholders-cli-tests
              --metadata-root ${PROJECT_SOURCE_DIR}/.cache/diag-export-cases --case ${_am_diag_case})
          set_tests_properties(cli_diagnostic_placeholders_${_am_diag_case} PROPERTIES LABELS "req:NFR-14.4.9;req:K-130" TIMEOUT 120)
        endforeach()
      endif()
      add_test(NAME log_word_ranges_check COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/tools/gen_log_word_ranges.py --check)
      set_tests_properties(log_word_ranges_check PROPERTIES LABELS "req:NFR-14.4.9;req:K-130")
      add_test(NAME message_catalog_check
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/tools/message_catalog_check.py --root ${PROJECT_SOURCE_DIR})
      set_tests_properties(message_catalog_check PROPERTIES LABELS "req:ADR-0032;req:ADR-0034")
      if(TARGET am_cli)
      file(GLOB _am_import_cases LIST_DIRECTORIES true CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tests/fixtures/import/*")
      foreach(_am_case_dir IN LISTS _am_import_cases)
        if(EXISTS "${_am_case_dir}/case.json")
          set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_am_case_dir}/case.json")
          get_filename_component(_am_case_name "${_am_case_dir}" NAME)
          file(READ "${_am_case_dir}/case.json" _am_case_json)
          string(JSON _am_req_count LENGTH "${_am_case_json}" req)
          set(_am_case_labels import_corpus)
          math(EXPR _am_req_last "${_am_req_count} - 1")
          foreach(_am_req_i RANGE 0 ${_am_req_last})
            string(JSON _am_req GET "${_am_case_json}" req ${_am_req_i})
            list(APPEND _am_case_labels "req:${_am_req}")
          endforeach()
          add_test(NAME import_corpus.${_am_case_name}
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/import_corpus/run_import_corpus.py
              --am $<TARGET_FILE:am_cli> --cases ${PROJECT_SOURCE_DIR}/tests/fixtures/import
              --work ${CMAKE_BINARY_DIR}/import_corpus --case ${_am_case_name})
          set_tests_properties(import_corpus.${_am_case_name} PROPERTIES LABELS "${_am_case_labels}" SKIP_RETURN_CODE 77 TIMEOUT 180)
        endif()
      endforeach()
      foreach(_am_fault_unit JournalFullBeforeAndAfterReplacement IndexFullAfterConfirmedCommitWarnsAndRemainsRecoverable IndexTransactionFull OperationLogFull ImportJournalFull ExportCheckpointFull RecoveryFull)
        add_test(NAME FaultSqlitePipeline.${_am_fault_unit}
          COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/unit/run_fault_unit.py
            --tests $<TARGET_FILE:am_unit_tests> --helper $<TARGET_FILE:am_fault_process_helper>
            --name FaultSqlitePipeline.${_am_fault_unit} --work ${PROJECT_SOURCE_DIR}/.cache/fault-unit)
        set_tests_properties(FaultSqlitePipeline.${_am_fault_unit} PROPERTIES LABELS "fault;req:K-117;req:K-122;req:ADR-0033" TIMEOUT 300)
      endforeach()
      add_test(NAME import_corpus_regen_check
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/make_import_cases.py
          --cases ${PROJECT_SOURCE_DIR}/tests/fixtures/import --work ${CMAKE_BINARY_DIR}/import_corpus_regen --check)
      set_tests_properties(import_corpus_regen_check PROPERTIES LABELS "import_corpus;req:ADR-0029;req:SC-1")
      add_test(NAME import_corpus_selftest
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/import_corpus/test_runner_self.py)
      set_tests_properties(import_corpus_selftest PROPERTIES LABELS "import_corpus;req:ADR-0029;req:SC-13")
      add_test(NAME cli_export
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/cli/test_export_cli.py
          --am $<TARGET_FILE:am_cli> --root ${PROJECT_SOURCE_DIR}/.cache/export-cli-tests --project ${PROJECT_SOURCE_DIR})
      set_tests_properties(cli_export PROPERTIES LABELS "req:K-10;req:K-46;req:K-127;req:NFR-14.5.6" TIMEOUT 180)
      add_test(NAME cli_state_recovery
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/cli/test_state_recovery_cli.py
          --am $<TARGET_FILE:am_cli> --root ${PROJECT_SOURCE_DIR}/.cache/fault-cli)
      set_tests_properties(cli_state_recovery PROPERTIES LABELS "fault;req:K-117;req:K-122;req:ADR-0007" TIMEOUT 180)
      add_test(NAME small_volume
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/unit/run_small_volume.py
          --tests $<TARGET_FILE:am_small_volume_tests> --helper $<TARGET_FILE:am_fault_process_helper>
          --work ${PROJECT_SOURCE_DIR}/.cache/small-volume-runs)
      set_tests_properties(small_volume PROPERTIES LABELS "periodic;small_volume;req:K-117;req:K-122" SKIP_RETURN_CODE 77 TIMEOUT 900)
      foreach(_am_fault_unit ReadyAndCopyPreflight ReadyAndCopyDuringWrite ExportDuringWrite SelectiveExportPreflight DiagnosticExportDuringWrite ImportInputs)
        add_test(NAME FaultSpace.${_am_fault_unit}
          COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/unit/run_fault_unit.py
            --tests $<TARGET_FILE:am_unit_tests> --helper $<TARGET_FILE:am_fault_process_helper>
            --name FaultSpace.${_am_fault_unit} --work ${PROJECT_SOURCE_DIR}/.cache/fault-unit)
        set_tests_properties(FaultSpace.${_am_fault_unit} PROPERTIES LABELS "fault;req:K-117;req:K-122;req:ADR-0033" TIMEOUT 300)
      endforeach()
      foreach(_am_fault_case init copy ready ready_replace lifecycle rebuild sync operation_log recovery state_lock state_readonly wal_loss import_folder import_zip import_nested import_between export_full export_files diagnostic)
        add_test(NAME FaultProcess.${_am_fault_case}
          COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/unit/run_fault_process.py
            --helper $<TARGET_FILE:am_fault_process_helper> --work ${PROJECT_SOURCE_DIR}/.cache/fault-process --mode ${_am_fault_case})
        # Hypothesis: generous stalled-process guard for three repetitions per checkpoint.
        set_tests_properties(FaultProcess.${_am_fault_case} PROPERTIES LABELS "fault;req:K-117;req:K-122" TIMEOUT 900)
        if(_am_fault_case STREQUAL "state_readonly")
          set_tests_properties(FaultProcess.${_am_fault_case} PROPERTIES SKIP_RETURN_CODE 77)
        endif()
      endforeach()
      foreach(_am_e2e_case import-transfer ready-transfer copy-transfer index-rebuild trash-roundtrip selective-export)
        add_test(NAME e2e-${_am_e2e_case}
          COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/e2e/run_e2e.py
            --am $<TARGET_FILE:am_cli> --work ${CMAKE_BINARY_DIR}/e2e --case e2e-${_am_e2e_case})
        # Hypothesis: generous process guard, not a product performance target.
        set_tests_properties(e2e-${_am_e2e_case} PROPERTIES LABELS "e2e;req:K-10;req:K-22;req:K-31" TIMEOUT 900)
      endforeach()
      set_property(TEST e2e-index-rebuild APPEND PROPERTY LABELS "req:K-122")
      set_property(TEST e2e-copy-transfer APPEND PROPERTY LABELS "req:ADR-0036")
      add_test(NAME e2e_selftest COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/e2e/test_e2e_self.py)
      set_tests_properties(e2e_selftest PROPERTIES LABELS "e2e;req:K-22;req:ADR-0029")
      add_test(NAME e2e_portability_bundle
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/e2e/run_e2e.py
          --am $<TARGET_FILE:am_cli> --work ${CMAKE_BINARY_DIR}/e2e
          --bundle ${PROJECT_SOURCE_DIR}/build/e2e/portability)
      set_tests_properties(e2e_portability_bundle PROPERTIES LABELS "e2e;req:K-10;req:K-22;req:K-31" TIMEOUT 900)
      add_test(NAME cli_import_run
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/cli/test_import_run_cli.py
          --am $<TARGET_FILE:am_cli> --root ${PROJECT_SOURCE_DIR}/.cache/import-run-cli-tests)
      set_tests_properties(cli_import_run PROPERTIES LABELS "req:SC-1;req:SC-2;req:K-5;req:K-121" TIMEOUT 180)
      add_test(NAME cli_import_zip
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/cli/test_import_zip_cli.py
          --am $<TARGET_FILE:am_cli> --root ${PROJECT_SOURCE_DIR}/.cache/import-zip-cli-tests)
      set_tests_properties(cli_import_zip PROPERTIES LABELS "req:SC-1;req:SC-10;req:K-5;req:K-6" TIMEOUT 180)
      foreach(_am_import_case summary errors profiles ready overlap signal)
        if(_am_import_case STREQUAL "summary")
          set(_am_import_name cli_import_scan)
        else()
          set(_am_import_name cli_import_scan_${_am_import_case})
        endif()
        add_test(NAME ${_am_import_name}
          COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/cli/test_import_scan_cli.py
            --am $<TARGET_FILE:am_cli> --root ${PROJECT_SOURCE_DIR}/.cache/import-scan-cli-tests
            --source ${PROJECT_SOURCE_DIR} --pylib ${AM_PYTHON_EXTRA_PATH} --case ${_am_import_case})
        set_tests_properties(${_am_import_name} PROPERTIES LABELS "req:SC-1;req:SC-2;req:SC-3;req:SC-13;req:SC-14;req:SC-15" TIMEOUT 180)
      endforeach()
      foreach(_am_log_case default_off configuration failures concurrent commit export killed)
        if(_am_log_case STREQUAL "default_off")
          set(_am_log_name cli_logging)
        elseif(_am_log_case STREQUAL "killed")
          set(_am_log_name FileLog.ErrorSurvivesKilledProcess)
        else()
          set(_am_log_name cli_logging_${_am_log_case})
        endif()
        add_test(NAME ${_am_log_name}
          COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/cli/test_logging_cli.py
            --am $<TARGET_FILE:am_cli> --helper $<TARGET_FILE:am_log_crash_helper>
            --root ${PROJECT_SOURCE_DIR}/.cache/logging-cli-tests --source ${PROJECT_SOURCE_DIR}
            --pylib ${AM_PYTHON_EXTRA_PATH} --case ${_am_log_case})
        set_tests_properties(${_am_log_name} PROPERTIES LABELS "req:NFR-14.4.9;req:K-130" TIMEOUT 120)
      endforeach()
      set_tests_properties(cli_logging_commit PROPERTIES SKIP_REGULAR_EXPRESSION "Production writes require a verified fixed NTFS volume")
      foreach(_am_jobs_case verify decision failure_and_controls locked read_only_and_media signal)
        if(_am_jobs_case STREQUAL "read_only_and_media")
          set(_am_jobs_name cli_jobs)
        else()
          set(_am_jobs_name cli_jobs_${_am_jobs_case})
        endif()
        add_test(NAME ${_am_jobs_name}
          COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/cli/test_jobs_cli.py
            --am $<TARGET_FILE:am_cli> --root ${PROJECT_SOURCE_DIR}/.cache/jobs-cli-tests --case ${_am_jobs_case})
        set_tests_properties(${_am_jobs_name} PROPERTIES LABELS "req:K-115;req:K-121;req:NFR-14.4.3" TIMEOUT 120)
        if(NOT _am_jobs_case STREQUAL "read_only_and_media")
          set_tests_properties(${_am_jobs_name} PROPERTIES SKIP_REGULAR_EXPRESSION "Production writes require a verified fixed NTFS volume")
        endif()
      endforeach()
      foreach(_am_library_case init_info refusals missing_databases owner_lock rebuild_list sync trash add_ready copy verify verify_database_pages log log_writes)
        add_test(NAME cli_library_${_am_library_case}
          COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/cli/test_library_cli.py
            --am $<TARGET_FILE:am_cli> --root ${PROJECT_SOURCE_DIR}/.cache/library-cli-tests --case ${_am_library_case})
        set_tests_properties(cli_library_${_am_library_case} PROPERTIES LABELS "req:K-7;req:K-123;req:ADR-0007;req:ADR-0031")
      endforeach()
      set_property(TEST cli_library_sync APPEND PROPERTY LABELS "req:K-3")
      set_property(TEST cli_library_trash APPEND PROPERTY LABELS "req:K-3")
      set_property(TEST cli_library_add_ready APPEND PROPERTY LABELS "req:K-31" "req:SC-14")
      set_property(TEST cli_library_copy APPEND PROPERTY LABELS "req:K-3")
      set_property(TEST cli_library_verify cli_library_verify_database_pages APPEND PROPERTY LABELS "req:K-22")
      set_property(TEST cli_library_log cli_library_log_writes APPEND PROPERTY LABELS "req:NFR-14.4.9;req:K-31")
      set_tests_properties(cli_library_log_writes PROPERTIES SKIP_REGULAR_EXPRESSION "Production writes require a verified fixed NTFS volume")
      set_tests_properties(cli_library_rebuild_list cli_library_sync cli_library_trash cli_library_add_ready cli_library_copy PROPERTIES SKIP_REGULAR_EXPRESSION "Production writes require a verified fixed NTFS volume")
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
    set_tests_properties(sql_check PROPERTIES LABELS "schema;req:ADR-0006;req:ADR-0007;req:ADR-0037")
  endif()
endif()

if(TARGET am_cli)
  add_subdirectory(performance/gen_library)
endif()
