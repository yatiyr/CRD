add_executable(crd-units-tests
    test_dim.cpp
    test_quantity.cpp
    test_linear_units.cpp
    test_compound_units.cpp
    test_value_in.cpp
    test_affine_units.cpp
    test_nonlinear_units.cpp
    test_literals.cpp
    test_vec_quantity.cpp  # v0b-1
    test_vec_quantity_reductions.cpp  # v0d-1
    test_unit_preferences.cpp  # v0d-5
)

target_link_libraries(crd-units-tests PRIVATE
    crd-core
    crd-units
    crd-math  # v0b-1: Vec3<Length<>> etc.
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-units-tests)

catch_discover_tests(crd-units-tests)
