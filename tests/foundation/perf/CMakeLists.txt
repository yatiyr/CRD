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
)

target_link_libraries(crd-perf-tests PRIVATE
    crd-core
    crd-units
    crd-time
    crd-jobs
    crd-memory
    crd-perf
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
    crd-diag-leak-specimen)             # DIAG.3f
target_compile_definitions(crd-perf-tests PRIVATE
    CRD_DIAG_CLEAN_SPECIMEN="$<TARGET_FILE:crd-diag-clean-specimen>"
    CRD_DIAG_CRASH_SPECIMEN="$<TARGET_FILE:crd-diag-crash-specimen>"
    CRD_DIAG_CRASH_CAPTURE_SPECIMEN="$<TARGET_FILE:crd-diag-crash-capture-specimen>"
    CRD_DIAG_HEAP_OVERFLOW_SPECIMEN="$<TARGET_FILE:crd-diag-heap-overflow-specimen>"
    CRD_DIAG_USE_AFTER_FREE_SPECIMEN="$<TARGET_FILE:crd-diag-use-after-free-specimen>"
    CRD_DIAG_USE_AFTER_RETURN_SPECIMEN="$<TARGET_FILE:crd-diag-use-after-return-specimen>"
    CRD_DIAG_LEAK_SPECIMEN="$<TARGET_FILE:crd-diag-leak-specimen>")

crd_discover_tests(crd-perf-tests)

# REPO.DEV.9 / DIAG.5d(e2): the bounded diagnostic-bundle ingestion fuzz target + its committed corpus replay.
add_subdirectory(fuzz)
