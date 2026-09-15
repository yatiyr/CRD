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

# DIAG.4a negative control: a mid-flight set_observer() must trip the quiescence fatal. The specimen
# is built and its path handed to the consuming test, which asserts the specimen exited with the
# handler's expected code -- never a silent pass.
add_dependencies(crd-jobs-tests crd-diag-observer-swap-specimen)
target_compile_definitions(crd-jobs-tests PRIVATE
    CRD_DIAG_OBSERVER_SWAP_SPECIMEN="$<TARGET_FILE:crd-diag-observer-swap-specimen>")

# DIAG.4a no-blocking-context negative control: an unenrolled wait on a single-thread pool must trip
# the deadlock fatal. The specimen exits with the handler's expected code; a hang -> timeout -> fail.
add_dependencies(crd-jobs-tests crd-diag-unenrolled-wait-specimen)
target_compile_definitions(crd-jobs-tests PRIVATE
    CRD_DIAG_UNENROLLED_WAIT_SPECIMEN="$<TARGET_FILE:crd-diag-unenrolled-wait-specimen>")

# DIAG.4c seeded-deadlock positive control: two sibling jobs each parked on the other's completion counter.
# The hang watchdog classifies the wait cycle and the specimen exits 42; a hang -> timeout -> fail. Proves
# the WaitCycle HangKind end-to-end (it cannot be run in-process -- a live deadlock leaves counters acquired).
add_dependencies(crd-jobs-tests crd-diag-deadlock-specimen)
target_compile_definitions(crd-jobs-tests PRIVATE
    CRD_DIAG_DEADLOCK_SPECIMEN="$<TARGET_FILE:crd-diag-deadlock-specimen>")

# DIAG.4c pool-exhaustion positive controls: acquiring past max_counters / small_fiber_count must fire an
# always-on, counted fatal (not a silent nullptr deref / dropped job). Each specimen exits 42 on the counted
# exhaustion; 60/97 or a hang -> timeout -> fail.
add_dependencies(crd-jobs-tests crd-diag-counter-exhaustion-specimen crd-diag-fiber-exhaustion-specimen)
target_compile_definitions(crd-jobs-tests PRIVATE
    CRD_DIAG_COUNTER_EXHAUSTION_SPECIMEN="$<TARGET_FILE:crd-diag-counter-exhaustion-specimen>"
    CRD_DIAG_FIBER_EXHAUSTION_SPECIMEN="$<TARGET_FILE:crd-diag-fiber-exhaustion-specimen>")

crd_enable_default_pch(crd-jobs-tests)

crd_discover_tests(crd-jobs-tests)

# DIAG.4b controlled-interleaving tests: a separate binary linking the gate-ON twin (crd-jobs-schedcheck)
# so the yield points are live. EXCLUDE_FROM_ALL and NOT ctest-registered -- it is a diagnostic tool
# built and run explicitly (like the tsan/fuzz diagnostic presets), never in the default build or the CI
# ctest set. The gated test files compile to nothing in crd-jobs-tests (gate off); here the
# CRD_JOBS_SCHED_CHECK=1 definition arrives via crd-jobs-schedcheck's PUBLIC compile definition.
add_executable(crd-jobs-schedcheck-tests EXCLUDE_FROM_ALL test_sched_check.cpp)
target_link_libraries(crd-jobs-schedcheck-tests PRIVATE
    crd-core
    crd-containers
    crd-jobs-schedcheck
    Catch2::Catch2WithMain)
