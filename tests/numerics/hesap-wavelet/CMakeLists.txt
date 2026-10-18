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

crd_discover_tests(crd-hesap-wavelet-tests)

# The continuous wavelet transform's out-of-memory fatal, one child process per mode (a fatal cannot run in-process):
# an allocator that refuses a per-job scale plan leaves it out of memory, fatal in every build, never a null plan
# constructed into. Source and protocol: tests/support/diag/specimens/hesap_wavelet_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-hesap-wavelet-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/hesap_wavelet_fatal_specimen.cpp")
target_link_libraries(crd-hesap-wavelet-fatal-specimen PRIVATE crd-core crd-memory crd-jobs crd-hesap-wavelet)
crd_diag_fatal_tests(crd-hesap-wavelet-fatal-specimen PREFIX crd-hesap-wavelet-fatal OWNER crd-hesap-wavelet-tests
    MODES cwt-plan-refused)
