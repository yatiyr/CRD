add_executable(crd-hesap-fft-tests
    test_fft.cpp
    test_nufft.cpp
    test_dct.cpp
    test_cli.cpp
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

crd_discover_tests(crd-hesap-fft-tests)

# The plans' out-of-memory fatals, one child process per mode (a fatal cannot run in-process): an allocator that
# refuses a plan's table or sub-plan leaves it out of memory, fatal in every build, never a null table written.
# Source and protocol: tests/support/diag/specimens/hesap_fft_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-hesap-fft-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/hesap_fft_fatal_specimen.cpp")
target_link_libraries(crd-hesap-fft-fatal-specimen PRIVATE crd-core crd-memory crd-hesap-fft)
crd_diag_fatal_tests(crd-hesap-fft-fatal-specimen PREFIX crd-hesap-fft-fatal OWNER crd-hesap-fft-tests
    MODES plan-hier-table-refused plan-stockham-table-refused plan-four-step-table-refused
          plan-four-step-subplan-refused bluestein-plan-refused nd-axis-plan-refused nd-bluestein-axis-refused
          sparse-plan-refused)
