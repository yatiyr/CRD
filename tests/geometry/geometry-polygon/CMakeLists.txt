add_executable(crd-geometry-polygon-tests
    test_polygon_types.cpp
    test_polygon_predicates.cpp
    test_point_in_polygon.cpp
    test_polygon_typed.cpp
    test_triangulate_ear_clip.cpp
    test_cdt.cpp
    test_polygon_boolean.cpp
    test_bentley_ottmann.cpp
)

target_link_libraries(crd-geometry-polygon-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-units
    crd-geometry-primitives
    crd-geometry-polygon
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-polygon-tests)

catch_discover_tests(crd-geometry-polygon-tests)
