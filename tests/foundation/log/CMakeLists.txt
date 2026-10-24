crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-log-tests ${TEST_SOURCES})

target_link_libraries(crd-log-tests PRIVATE
    crd-core
    crd-log
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-log-tests)

crd_discover_tests(crd-log-tests)

# Out of memory while a sink formats a record, one child process per mode (a fatal cannot run in-process): the
# String's out-of-memory fatal fires inside sink delivery and ends the process promptly, never a deadlock on the sink
# lock. Source and protocol: tests/support/diag/specimens/log_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-log-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/log_fatal_specimen.cpp")
target_link_libraries(crd-log-fatal-specimen PRIVATE crd-core crd-memory crd-containers crd-log)
crd_diag_fatal_tests(crd-log-fatal-specimen PREFIX crd-log-fatal OWNER crd-log-tests
    MODES sync-sink-format-refused async-sink-format-refused)
