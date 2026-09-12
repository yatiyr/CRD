# crd-test-helpers — header-only library of GPU sanity-check utilities.
# Phase 3.1.7 v9-prereq-test-harness (2026-05-18). Reused across the 16
# Phase 3.1.7 v9 GPU geometry slices for ULP-conformance compares +
# GPU determinism checks. Pairs with crd::rhi::ValidationCapture (in
# crd-rhi-vulkan) + crd::perf::measure_ms / CRD_PERF_BUDGET_LE.

add_library(crd-test-helpers INTERFACE)
target_include_directories(crd-test-helpers INTERFACE include)
target_link_libraries(crd-test-helpers INTERFACE
    crd-core
    crd-containers
)

# Self-test smoke — exercises the CPU-only helpers (ulp/bit compare,
# determinism check, measure_ms / CRD_PERF_BUDGET_LE). ValidationCapture
# requires a live VkInstance and is exercised by v9 GPU smoke tests.
add_executable(crd-test-helpers-tests
    test_helpers_smoke.cpp
)

target_link_libraries(crd-test-helpers-tests PRIVATE
    crd-test-helpers
    crd-core
    crd-containers
    crd-units
    crd-time
    crd-perf
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-test-helpers-tests)

catch_discover_tests(crd-test-helpers-tests)
