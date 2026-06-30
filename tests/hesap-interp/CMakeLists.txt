add_executable(crd-hesap-interp-tests
    test_piecewise.cpp
    test_cubic_spline.cpp
    test_akima_bary.cpp
    test_spectral_rational.cpp
    test_rbf.cpp
    test_grid.cpp
    test_kriging.cpp
    test_clough_tocher.cpp
)

target_link_libraries(crd-hesap-interp-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap-interp
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-interp-tests)

catch_discover_tests(crd-hesap-interp-tests)
