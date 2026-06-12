add_executable(crd-hesap-ode-tests
    test_ode_steppers.cpp
    test_ode_controller.cpp
    test_ode_integrate.cpp
    test_ode_erk.cpp
    test_ode_events.cpp
    test_ode_symplectic.cpp
    test_ode_bdf.cpp
    test_ode_radau.cpp
    test_ode_rosenbrock.cpp
    test_ode_mass.cpp
    test_ode_sparse.cpp
)

target_link_libraries(crd-hesap-ode-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap-ode
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-ode-tests)

catch_discover_tests(crd-hesap-ode-tests)

