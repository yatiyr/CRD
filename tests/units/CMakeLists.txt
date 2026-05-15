add_executable(crd-units-tests
    test_dim.cpp
    test_quantity.cpp
    test_linear_units.cpp
    test_compound_units.cpp
    test_value_in.cpp
    test_affine_units.cpp
    test_nonlinear_units.cpp
    test_literals.cpp
)

target_link_libraries(crd-units-tests PRIVATE
    crd-core
    crd-units
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-units-tests)

catch_discover_tests(crd-units-tests)
