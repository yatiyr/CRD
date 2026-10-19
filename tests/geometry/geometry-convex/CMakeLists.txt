add_executable(crd-geometry-convex-tests
    test_support.cpp
    test_sub_distance.cpp
    test_gjk_distance.cpp
    test_gjk_determinism.cpp
    test_gjk_overlap.cpp
    test_epa.cpp
    test_sat.cpp
    test_hull_queries.cpp
    test_shapecast_convex.cpp
    test_hill_climb.cpp
    test_simd_support.cpp
    test_f64_orbital.cpp
    test_feature_clip.cpp
    test_tiebreak_conformance.cpp
    test_degenerate_corpus.cpp
    test_convex_hull_2d.cpp
    test_quickhull.cpp
    test_hull_simplify.cpp
    test_v3_close.cpp
    # The two v2c design probes, kept on disk unbuilt, were removed on 2026-10-19 (crd-check-source-routes): the EPA
    # one is the "EPA: rotated hull-vs-hull self-consistency" case of test_epa.cpp (77/77 at the time); the GJK
    # simplex-size histogram answered its v2c question (docs/archive/systems/geometry-convex.md).
)

target_link_libraries(crd-geometry-convex-tests PRIVATE
    crd-core
    crd-math
    crd-memory                # TlsfAllocator for hull-vertex storage (no default_allocator() in tests)
    crd-containers
    crd-geometry-primitives   # ConvexHullView / Sphere / OBB3 / Capsule3 + closest_point for brute-force ref
    crd-geometry-convex
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-convex-tests)

crd_discover_tests(crd-geometry-convex-tests)
