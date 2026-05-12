add_executable(crd-bench
    test_bench.cpp
    test_bench_simd.cpp
)

target_link_libraries(crd-bench PRIVATE
    crd-core
    crd-log
    crd-math
    crd-geometry-primitives
    crd-memory
    crd-containers
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-bench)

# Benchmarks are intentionally built with the regular test tree but kept out of
# ctest so CI/unit-test presets stay fast and deterministic.

# C4714 suppression now lives globally on `crd-warnings` (root CMakeLists.txt)
# — the prior per-target shim here was the seed of LNK1257 drift onto other
# release-class targets.
