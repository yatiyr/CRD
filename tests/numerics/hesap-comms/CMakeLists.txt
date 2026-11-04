add_executable(crd-hesap-comms-tests
    test_modulation.cpp
    test_pulse_shaping.cpp
    test_sync.cpp
    test_equalizer.cpp
    test_erasure.cpp
    test_flexfec.cpp
    test_channel.cpp
    test_ofdm.cpp
    test_cli.cpp
)

target_link_libraries(crd-hesap-comms-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-comms
    crd-hesap-stats
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-comms-tests)

crd_discover_tests(crd-hesap-comms-tests)

# ADR-0134 §8: the [kernels] cases once per compiled level (crd-hesap-comms-kernels-at-<level>).
crd_add_kernel_level_tests(crd-hesap-comms-tests "[kernels]")

# HESAP-NET: the erasure code and its kernels under the bounded fuzz harness, with its committed corpus.
add_subdirectory(fuzz)

# HESAP-NET's ISA-L oracle (CRD_BUILD_ERASURE_VS_ISAL, the root CMakeLists): ISA-L's portable erasure code, compiled
# from the pinned release as crd-isal-base-peer, holds the field, GM, encoding and decoding to a second implementation.
if(CRD_BUILD_ERASURE_VS_ISAL)
    add_library(crd-isal-base-peer STATIC "${isa-l_SOURCE_DIR}/erasure_code/ec_base.c")
    target_include_directories(crd-isal-base-peer SYSTEM PUBLIC "${isa-l_SOURCE_DIR}/include")
    target_include_directories(crd-isal-base-peer PRIVATE "${isa-l_SOURCE_DIR}/erasure_code")
    add_executable(crd-hesap-erasure-isal-tests test_erasure_isal.cpp)
    target_link_libraries(crd-hesap-erasure-isal-tests PRIVATE
        crd-core crd-containers crd-hesap-comms crd-hesap-stats crd-isal-base-peer Catch2::Catch2WithMain)
    crd_discover_tests(crd-hesap-erasure-isal-tests)
endif()
