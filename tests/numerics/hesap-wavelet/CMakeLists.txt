add_executable(crd-hesap-wavelet-tests
    test_families.cpp
    test_dwt.cpp
    test_swt_wpt.cpp
    test_cwt.cpp
    test_dwt2_denoise.cpp
    test_cli.cpp
    test_modwt.cpp
)

target_link_libraries(crd-hesap-wavelet-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-wavelet
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-wavelet-tests)

catch_discover_tests(crd-hesap-wavelet-tests)
