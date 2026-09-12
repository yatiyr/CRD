add_executable(crd-hesap-quadrature-tests
    test_gauss.cpp
    test_integrate.cpp
    test_adaptive.cpp
    test_de.cpp
    test_oscillatory.cpp
    test_cubature.cpp
    test_cli.cpp
)

target_link_libraries(crd-hesap-quadrature-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-special
    crd-hesap-dense
    crd-hesap-quadrature
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-quadrature-tests)

catch_discover_tests(crd-hesap-quadrature-tests)
