add_executable(crd-hesap-special-tests
    test_special.cpp
    test_batch.cpp
    test_special_kernels.cpp
    test_bessel.cpp
    test_orthopoly.cpp
    test_expint.cpp
    test_cli.cpp
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

crd_discover_tests(crd-hesap-special-tests)

# ADR-0134 §8: the [kernels] cases once per compiled level (crd-hesap-special-kernels-at-<level>).
crd_add_kernel_level_tests(crd-hesap-special-tests "[kernels]")
