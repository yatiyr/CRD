add_executable(crd-hesap-special-tests
    test_special.cpp
    test_batch.cpp
    test_bessel.cpp
    test_orthopoly.cpp
    test_expint.cpp
)

target_link_libraries(crd-hesap-special-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-special
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-special-tests)

catch_discover_tests(crd-hesap-special-tests)
