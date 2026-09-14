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
    test_perf_budget_soft.cpp
    test_diag_harness.cpp
    test_doctor.cpp
    test_diagnostics.cpp
    test_diagnostic_recorder.cpp
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
    crd-diag-heap-overflow-specimen)
target_compile_definitions(crd-perf-tests PRIVATE
    CRD_DIAG_CLEAN_SPECIMEN="$<TARGET_FILE:crd-diag-clean-specimen>"
    CRD_DIAG_CRASH_SPECIMEN="$<TARGET_FILE:crd-diag-crash-specimen>"
    CRD_DIAG_HEAP_OVERFLOW_SPECIMEN="$<TARGET_FILE:crd-diag-heap-overflow-specimen>")

crd_discover_tests(crd-perf-tests)
