add_executable(crd-geometry-spatial-tests
    test_kd_tree_build.cpp
    test_kd_radius.cpp
    test_kd_nearest_n.cpp
    test_kd_range_aabb.cpp
    test_kd_typed.cpp
    test_loose_octree.cpp
    test_loose_octree_typed.cpp
    test_rtree.cpp
    test_rtree_typed.cpp
    test_spatial_hash.cpp
    test_spatial_hash_typed.cpp
    test_uniform_grid.cpp
    test_uniform_grid_typed.cpp
)

target_link_libraries(crd-geometry-spatial-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-units
    crd-geometry-primitives
    crd-geometry-spatial
    crd-jobs                # SpatialHashJobsListener — concurrent scratch tests via fiber pool
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-spatial-tests)

catch_discover_tests(crd-geometry-spatial-tests)
