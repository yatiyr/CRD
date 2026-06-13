add_executable(crd-hesap-fft-tests
    test_fft.cpp
    test_nufft.cpp
    test_dct.cpp
)

target_link_libraries(crd-hesap-fft-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-fft
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-fft-tests)

catch_discover_tests(crd-hesap-fft-tests)
