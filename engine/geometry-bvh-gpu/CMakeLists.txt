# crd-geometry-bvh-gpu — Phase 3.1.7 v9a.
#
# GPU implementation of the LBVH (Karras 2012) build pipeline.
# Sibling module to `crd-geometry-bvh` (CPU-only). Mirrors the
# rhi / rhi-vulkan split pattern: the existing CPU BVH module stays
# CPU-only (no Vulkan dependency); GPU kernels + dispatch live here.
#
# Slice ledger:
#   v9a-a  compute_morton_codes_{cpu,gpu}    30-bit Morton bit-interleave
#                                            of AABB centroids (this slice).
#   v9a-b1 radix_sort_cpu                    Deterministic CPU sort of
#                                            (morton, original_index) pairs.
#   v9a-b2 radix_sort_gpu                    Blelloch parallel-scan; 8 passes
#                                            on 4-bit digits over 30-bit keys.
#   v9a-c  build_lbvh_{cpu,gpu}              Karras 2012 prefix-length-
#                                            comparison binary tree. CPU
#                                            sequential is the algorithm
#                                            definition; GPU is a mechanical
#                                            translation.
#   v9a-d  upsweep_aabbs_{cpu,gpu}           Bottom-up parent-AABB
#                                            propagation. Atomic-on-parent
#                                            with sign-flipped-uint encoding
#                                            (Vulkan has no atomicMin/Max on
#                                            float in core).
#
# Determinism contract (locked at v9a-close):
#   - CPU sort path (v9a-b1 + v9a-c + v9a-d w/ commutative-min/max atomics
#     on non-NaN input) is bit-deterministic across runs.
#   - GPU sort path (v9a-b2) is throughput-tier; byte-conformance tested
#     vs the CPU sort path.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-bvh-gpu STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-bvh-gpu
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-bvh-gpu
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-units           # for `Length<T>` / `Quantity<D, T>` typed wrappers (v9a-a-typed)
        crd-geometry-primitives
        crd-geometry-bvh    # for `BvhTree` / `BvhNode` output types at v9a-c
        crd-rhi             # for the backend-agnostic Device / Buffer / dispatch surface
    PRIVATE
        crd-rhi-vulkan      # impl-only — module is intended to be Vulkan-only initially
)

target_compile_features(crd-geometry-bvh-gpu PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-bvh-gpu)
