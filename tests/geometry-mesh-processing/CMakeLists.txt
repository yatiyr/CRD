add_executable(crd-geometry-mesh-processing-tests
    test_half_edge_mesh.cpp
    test_attribute_quadric.cpp # REN-40-C1: Hoppe attribute quadrics (the LOD chain's error metric)
    test_quadric.cpp
    test_qem_decimate.cpp
    test_qem_decimate_attr.cpp # REN-40-C1: decimation that keeps its UVs
    test_loop_subdivide.cpp
    test_isotropic_remesh.cpp
    test_fill_holes.cpp
    test_repair_manifoldness.cpp
    test_remove_self_intersections.cpp
    test_taubin_smooth.cpp
)

target_link_libraries(crd-geometry-mesh-processing-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-units
    crd-geometry-primitives
    crd-geometry-mesh-processing
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-mesh-processing-tests)

catch_discover_tests(crd-geometry-mesh-processing-tests)
