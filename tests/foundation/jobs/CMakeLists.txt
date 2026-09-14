crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-jobs-tests ${TEST_SOURCES})

target_link_libraries(crd-jobs-tests PRIVATE
    crd-core
    crd-containers   # white-box tests include engine/foundation/jobs/src/scheduler.hpp, which uses crd::containers::ConcurrentQueue
    crd-jobs
    crd-diag-harness # DIAG.1b: runs the data-race specimen as a bounded child (instrument-present control)
    Catch2::Catch2
)

# DIAG.1b negative control: the racy specimen is built and its path handed to the test, which
# asserts SanitizerCaught under ThreadSanitizer and InstrumentAbsent otherwise -- never a silent pass.
add_dependencies(crd-jobs-tests crd-diag-data-race-specimen)
target_compile_definitions(crd-jobs-tests PRIVATE
    CRD_DIAG_DATA_RACE_SPECIMEN="$<TARGET_FILE:crd-diag-data-race-specimen>")

crd_enable_default_pch(crd-jobs-tests)

crd_discover_tests(crd-jobs-tests)
