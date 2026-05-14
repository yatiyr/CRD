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
    # test_epa_probe.cpp        # answered: EPA robust on rotated hull-hull (77/77); pathology was OBB-OBB-rotated only, covered by SAT (v2d). probe kept on disk for reference; not built.
    # test_simplex_size_probe.cpp  # design-phase probe (kept on disk; not built)
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

catch_discover_tests(crd-geometry-convex-tests)
