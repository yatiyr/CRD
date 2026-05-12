add_executable(crd-geometry-primitives-tests
    test_primitives.cpp
)

target_link_libraries(crd-geometry-primitives-tests PRIVATE
    crd-core
    crd-math                  # Vec3f/Mat3f/Mat4f + approx_equal_abs etc.
    crd-geometry-primitives
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-primitives-tests)

catch_discover_tests(crd-geometry-primitives-tests)
