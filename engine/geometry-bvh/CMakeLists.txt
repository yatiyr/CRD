# crd-geometry-bvh — Phase 3.1.7 v1a–v1f.
#
# Second sub-module of crd-geometry (ADR-0076 §1, after -primitives). The
# bounding-volume hierarchy: binned-SAH builder (serial + jobs-parallel) + refit
# + DynamicBvh + Bvh4Tree + raycast / overlap / closest-point queries over
# AABB3<f32> primitive boxes. Out-of-line — real TUs (no force-link stub).
# Deterministic by construction: ADR-0076 §5.2 pins the SAH split tiebreak; no
# std::sort; the parallel build's per-chunk reductions are min/max + integer
# adds (exact, commutative) → bit-identical to the serial build.
crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-bvh STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-bvh
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-bvh
    PUBLIC
        crd-core                # types, asserts
        crd-math                # Vec3f + scalar helpers
        crd-containers          # Array / ConstSpan (the canonical owning/view containers per ADR-0076 §5)
        crd-geometry-primitives # AABB3, Ray3, the v0f precomputed Williams/Ize robust ray-AABB slab
        crd-simd-flags
    PRIVATE
        crd-jobs                # bvh_build_parallel — jobs::parallel_reduce (v1f); not in any public header
)

target_compile_features(crd-geometry-bvh PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-bvh)
