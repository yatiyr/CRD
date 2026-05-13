add_executable(crd-geometry-bvh-tests
    test_bvh_build.cpp
    test_bvh_raycast.cpp
    test_bvh_refit.cpp
    test_dynamic_bvh.cpp
    test_bvh4.cpp
    test_bvh_closest.cpp
    test_bvh_parallel.cpp
)

target_link_libraries(crd-geometry-bvh-tests PRIVATE
    crd-core
    crd-math
    crd-memory                # TlsfAllocator for the test fixtures (no default_allocator())
    crd-geometry-primitives   # AABB3 / Ray3 / robust ray-AABB reference
    crd-geometry-bvh
    crd-jobs                  # bvh_build_parallel (v1f) — the binary's BvhJobsListener inits/shuts it
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-bvh-tests)

catch_discover_tests(crd-geometry-bvh-tests)
