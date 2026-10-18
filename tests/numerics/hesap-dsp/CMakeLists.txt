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
    test_cli.cpp
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

crd_discover_tests(crd-hesap-dsp-tests)

# The convolver's and the spectral estimators' out-of-memory fatals, one child process per mode (a fatal cannot run
# in-process): an allocator that refuses a real-FFT plan leaves them out of memory, fatal in every build, never a null
# plan constructed into. Source and protocol: tests/support/diag/specimens/hesap_dsp_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-hesap-dsp-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/hesap_dsp_fatal_specimen.cpp")
target_link_libraries(crd-hesap-dsp-fatal-specimen PRIVATE crd-core crd-memory crd-jobs crd-hesap-dsp)
crd_diag_fatal_tests(crd-hesap-dsp-fatal-specimen PREFIX crd-hesap-dsp-fatal OWNER crd-hesap-dsp-tests
    MODES convolver-plan-refused welch-plan-refused stft-plan-refused)
