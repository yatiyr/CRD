add_executable(crd-perf-tests
    test_sample_pod.cpp
    test_intern_names.cpp
    test_scope_push_pop.cpp
    test_thread_registration.cpp
    test_ring_overflow.cpp
    test_frame_mark.cpp
    test_fiber_migration.cpp
    test_zero_overhead_gate.cpp
    test_determinism_contract.cpp
    test_counters_register.cpp
    test_counters_set_add.cpp
    test_counters_frame_snapshot.cpp
    test_counters_macros.cpp
    test_counters_threaded.cpp
    test_jobs_adapter.cpp
    test_gpu_scope.cpp
    test_memory_tracking.cpp
    test_capture_roundtrip.cpp
    test_diag_cprof_version.cpp # DIAG.6c(b): pinned-layout contract + version/magic/size reject + unknown-flag tolerance
    test_diag_perfetto_export.cpp # DIAG.6c(c): CPROF -> Perfetto trace-events JSON export
    test_diag_frame_pacing.cpp # DIAG.6c(i): frame pacing / input-to-present, separate from throughput + shader time
    test_perf_budget_soft.cpp
    test_diag_harness.cpp
    test_doctor.cpp
    test_diagnostics.cpp
    test_diagnostic_recorder.cpp
    test_diag_bundle.cpp
    test_diag_symbol_index.cpp
    test_diag_bundle_import.cpp
    test_diag_symbolize_e2e.cpp
    test_diag_registry_race.cpp
    test_diag_ring_tearing.cpp
    test_diag_sample_ring.cpp
    test_diag_sample_ring_readers.cpp
    test_diag_dynamic_names.cpp
    test_diag_history_labels.cpp
    test_diag_quiescence.cpp
    test_diag_correlation.cpp
    test_diag_gpu_tracks.cpp
    test_diag_gpu_calibration.cpp
    test_diag_gpu_bridge.cpp
    test_diag_gpu_resolve.cpp
    test_diag_commands.cpp # typed bounded diagnostic commands through a native caller (no transport linked)
    test_diag_flight_recorder.cpp # DIAG.9b: triggerable bounded flight recording on the existing failure seams
    test_diag_fault_injection.cpp # DIAG.9b: seeded, authorized fault injection and its shipping refusal
    test_diag_flight_commands.cpp # DIAG.9b: authored flight/fault assets, flight.* commands and the doctor's live modes
    test_diag_flight_identity.cpp # DIAG.9b: a flight bundle's build identity and live module snapshot
    test_diag_fault_minimize.cpp # DIAG.9c: minimizing a fault plan that reproduces a failure
    test_diag_observer_effect.cpp # DIAG.9c: flight-recorder detail levels and observer-effect experiments
    test_diag_host.cpp # DIAG.9b/11a: the reusable diagnostic host, its authored profile and its change rules
)

target_link_libraries(crd-perf-tests PRIVATE
    crd-core
    crd-units
    crd-time
    crd-jobs
    crd-memory
    crd-perf
    crd-platform    # DIAG.11a: the diagnostic host cases read and write files through the I/O fault seam
    crd-diag-harness
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-perf-tests)

add_dependencies(crd-perf-tests
    crd-diag-clean-specimen
    crd-diag-crash-specimen
    crd-diag-crash-capture-specimen     # DIAG.5d(f): real-dump symbolization e2e
    crd-diag-heap-overflow-specimen
    crd-diag-use-after-free-specimen    # DIAG.3f
    crd-diag-use-after-return-specimen  # DIAG.3f
    crd-diag-leak-specimen              # DIAG.3f
    crd-diag-flight-fatal-specimen      # DIAG.9b: the fatal path writes the flight window before the process dies
    crd-diag-host-specimen              # DIAG.11a: an application that fails on purpose under its authored profile
    crd-diag-cpu-hotspot-specimen)      # DIAG.10b: the harness's own Timeout verdict
target_compile_definitions(crd-perf-tests PRIVATE
    CRD_DIAG_CLEAN_SPECIMEN="$<TARGET_FILE:crd-diag-clean-specimen>"
    CRD_DIAG_CRASH_SPECIMEN="$<TARGET_FILE:crd-diag-crash-specimen>"
    CRD_DIAG_CRASH_CAPTURE_SPECIMEN="$<TARGET_FILE:crd-diag-crash-capture-specimen>"
    CRD_DIAG_HEAP_OVERFLOW_SPECIMEN="$<TARGET_FILE:crd-diag-heap-overflow-specimen>"
    CRD_DIAG_USE_AFTER_FREE_SPECIMEN="$<TARGET_FILE:crd-diag-use-after-free-specimen>"
    CRD_DIAG_USE_AFTER_RETURN_SPECIMEN="$<TARGET_FILE:crd-diag-use-after-return-specimen>"
    CRD_DIAG_LEAK_SPECIMEN="$<TARGET_FILE:crd-diag-leak-specimen>"
    CRD_DIAG_FLIGHT_FATAL_SPECIMEN="$<TARGET_FILE:crd-diag-flight-fatal-specimen>"
    CRD_DIAG_HOST_SPECIMEN="$<TARGET_FILE:crd-diag-host-specimen>"
    CRD_DIAG_CPU_HOTSPOT_SPECIMEN="$<TARGET_FILE:crd-diag-cpu-hotspot-specimen>"
    CRD_DIAG_COMMANDS_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}"
    CRD_DIAG_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/diag")

crd_discover_tests(crd-perf-tests)

# REPO.DEV.9 / DIAG.5d(e2): the bounded diagnostic-bundle ingestion fuzz target + its committed corpus replay.
add_subdirectory(fuzz)
