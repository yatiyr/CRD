add_executable(crd-hesap-dsp-tests
    test_filter.cpp
    test_windows.cpp
    test_fir.cpp
    test_firls_remez.cpp
    test_remez_typeiii.cpp
    test_fir_special.cpp
    test_iir.cpp
    test_iir_design.cpp
    test_rbj.cpp
    test_hilbert.cpp
    test_multirate.cpp
    test_waveforms.cpp
    test_adaptive.cpp
    test_ar.cpp
    test_transforms.cpp
    test_subspace.cpp
    test_multitaper.cpp
    test_sequences.cpp
    test_measurements.cpp
    test_elliptic.cpp
    test_filtering.cpp
    test_convolution.cpp
    test_spectral.cpp
)

target_link_libraries(crd-hesap-dsp-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-dsp
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-dsp-tests)

catch_discover_tests(crd-hesap-dsp-tests)
