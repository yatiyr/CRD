add_executable(crd-bench
    test_bench.cpp
    test_bench_simd.cpp
    test_bench_bvh.cpp
    test_bench_gjk.cpp
    test_bench_quickhull.cpp
    test_bench_diag.cpp        # DIAG.11b (9): generation checks, recording and probes per call
)

target_link_libraries(crd-bench PRIVATE
    crd-core
    crd-log
    crd-math
    crd-geometry-primitives
    crd-geometry-bvh           # v1f BVH build/query benchmarks
    crd-geometry-convex        # v2b gjk_distance / gjk_overlap benchmarks
    crd-jobs                   # bvh_build_parallel needs the job system up in the bench body
    crd-memory
    crd-containers
    crd-ceir-cook              # DIAG.11b (9): replay.record's traced run
    crd-scene                  # DIAG.11b (9): the entity generation check
    Catch2::Catch2WithMain
)
target_compile_definitions(crd-bench PRIVATE
    CRD_BENCH_DIAG_PROGRAM="${CMAKE_SOURCE_DIR}/assets/diag/overhead_offline.ceir")

crd_enable_default_pch(crd-bench)

# Benchmarks are intentionally built with the regular test tree but kept out of
# ctest so CI/unit-test presets stay fast and deterministic.

# C4714 suppression now lives globally on `crd-warnings` (root CMakeLists.txt)
# — the prior per-target shim here was the seed of LNK1257 drift onto other
# release-class targets.
