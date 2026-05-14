# crd-geometry-convex — Phase 3.1.7 v2.
#
# Third sub-module of crd-geometry (ADR-0076 §1, after -primitives and -bvh).
# The convex-shape narrow-phase substrate: GJK distance / overlap, EPA
# penetration, SAT box-pair fast path, ConvexHullView queries, GJK-cast TOI,
# and the hill-climb / SIMD support-function performance lanes (v2g, v2h).
# This v2a slice lands the GJK distance kernel + the three locked substrate
# decisions: ConvexShape concept, local-frame shapes + two transforms, and
# SupportPoint{point, vertex_idx} for cross-platform bit-exact reproducibility
# (ADR-0076 §4 pin #14).
#
# Deterministic by construction: Ericson §9.5 sub-distance order (NOT van den
# Bergen), lowest-vertex-idx tiebreak on coincident hull extrema, primary
# index-match termination (Box2D pattern — epsilon-free); inherits ADR-0063
# determinism contract via crd-no-std-math-check.
crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-convex STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-convex
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-convex
    PUBLIC
        crd-core                # types, asserts
        crd-math                # Vec3 / Mat3 / Quat / Transform + scalar helpers
        crd-containers          # ConstSpan / StaticArray (the canonical view containers per ADR-0076 §5)
        crd-memory              # IAllocator (Quickhull result-arrays carry one)
        crd-geometry-primitives # Sphere / OBB3 / Capsule3 / ConvexHullView / k_distance_epsilon
        crd-simd-flags
)

target_compile_features(crd-geometry-convex PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-convex)
