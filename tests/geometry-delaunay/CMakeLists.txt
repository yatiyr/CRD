add_executable(crd-geometry-delaunay-tests
    test_delaunay_2d.cpp
    test_delaunay_2d_hilbert.cpp
    test_delaunay_3d.cpp
    test_voronoi_2d.cpp
)

target_link_libraries(crd-geometry-delaunay-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-units
    crd-geometry-primitives
    crd-geometry-delaunay
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-delaunay-tests)

catch_discover_tests(crd-geometry-delaunay-tests)
