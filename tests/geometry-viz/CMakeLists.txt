add_executable(crd-geometry-viz-tests
    test_primitives.cpp
    test_queries.cpp
    test_bvh.cpp
)

target_link_libraries(crd-geometry-viz-tests PRIVATE
    crd-core
    crd-math
    crd-memory                # TlsfAllocator for the test fixtures (no default_allocator())
    crd-geometry-primitives
    crd-geometry-bvh
    crd-geometry-viz
    crd-draw
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-viz-tests)

catch_discover_tests(crd-geometry-viz-tests)
