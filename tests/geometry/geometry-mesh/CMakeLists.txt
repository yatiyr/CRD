add_executable(crd-geometry-mesh-tests
    test_mesh_closest_point.cpp
    test_mesh_raycast.cpp
    test_mesh_raycast_simd.cpp
    test_mesh_validate.cpp
    test_mesh_winding_number.cpp
)

target_link_libraries(crd-geometry-mesh-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-units
    crd-geometry-primitives
    crd-geometry-bvh
    crd-geometry-mesh
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-mesh-tests)

catch_discover_tests(crd-geometry-mesh-tests)
