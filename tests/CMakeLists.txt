# Test suite.
#
# Every test is a separate executable registered with CTest. No test carries a
# timeout property: a hang is a defect to diagnose, not a condition to hide.

add_library(ecg_test_support STATIC
  support/test_support.cpp)
target_include_directories(ecg_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(ecg_test_support PUBLIC ecg_core ecg_warnings)
target_compile_definitions(ecg_test_support PUBLIC
  ECG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  ECG_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")

# Helper process used by the multiprocess lock, crash, and restart tests. It is a
# real executable driven through the same public API as any other consumer.
add_executable(ecg_test_child support/child_process.cpp)
target_link_libraries(ecg_test_child PRIVATE ecg_core ecg_warnings)

function(ecg_add_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE ecg_test_support)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "ECG_TEST_CHILD=$<TARGET_FILE:ecg_test_child>;ECG_TEST_CLI=$<TARGET_FILE:ecg_cli>")
endfunction()

ecg_add_test(test_strong_and_quantity test_strong_and_quantity.cpp)
ecg_add_test(test_hash_and_canonical test_hash_and_canonical.cpp)
ecg_add_test(test_json_dialect test_json_dialect.cpp)
ecg_add_test(test_time_and_zones test_time_and_zones.cpp)
ecg_add_test(test_tariff_coverage test_tariff_coverage.cpp)
ecg_add_test(test_evidence_freshness test_evidence_freshness.cpp)
ecg_add_test(test_engine_decisions test_engine_decisions.cpp)
ecg_add_test(test_engine_property test_engine_property.cpp)
ecg_add_test(test_json_roundtrip test_json_roundtrip.cpp)
ecg_add_test(test_ledger_persistence test_ledger_persistence.cpp)
ecg_add_test(test_ledger_recovery test_ledger_recovery.cpp)
ecg_add_test(test_concurrency test_concurrency.cpp)
ecg_add_test(test_multiprocess test_multiprocess.cpp)
ecg_add_test(test_cli_end_to_end test_cli_end_to_end.cpp)
