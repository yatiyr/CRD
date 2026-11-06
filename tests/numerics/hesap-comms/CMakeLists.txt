add_executable(crd-hesap-comms-tests
    test_modulation.cpp
    test_pulse_shaping.cpp
    test_sync.cpp
    test_equalizer.cpp
    test_erasure.cpp
    test_flexfec.cpp
    test_red.cpp
    test_interleave.cpp
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
crd_add_kernel_level_tests(crd-hesap-comms-tests "[kernels]" SDE_CPUS icx)

# HESAP-NET: the erasure code and its kernels under the bounded fuzz harness, with its committed corpus.
add_subdirectory(fuzz)

# HESAP-NET's ISA-L peer (CRD_BUILD_ERASURE_VS_ISAL, the root CMakeLists): the pinned release's portable erasure code
# and, with CRD_ISAL_ASSEMBLY, its own x86-64 source list, compiled as crd-isal-peer. crd-hesap-erasure-isal-tests holds
# the field, GM, encoding and decoding to it, every ISA body included; crd-hesap-erasure-board times the two.
if(CRD_BUILD_ERASURE_VS_ISAL)
    set(isal_peer_sources "${isa-l_SOURCE_DIR}/erasure_code/ec_base.c")
    if(CRD_ISAL_ASSEMBLY)
        include("${isa-l_SOURCE_DIR}/cmake/erasure_code.cmake")
        list(TRANSFORM ERASURE_CODE_X86_64_SOURCES PREPEND "${isa-l_SOURCE_DIR}/")
        list(APPEND isal_peer_sources ${ERASURE_CODE_X86_64_SOURCES})
        list(FILTER ERASURE_CODE_X86_64_SOURCES INCLUDE REGEX "\.asm$")
        set_source_files_properties(${ERASURE_CODE_X86_64_SOURCES} PROPERTIES LANGUAGE ASM_NASM)
    endif()
    add_library(crd-isal-peer STATIC ${isal_peer_sources} isal_arms.c)
    target_include_directories(crd-isal-peer SYSTEM PUBLIC "${isa-l_SOURCE_DIR}/include")
    target_include_directories(crd-isal-peer PRIVATE "${isa-l_SOURCE_DIR}/erasure_code")
    target_compile_definitions(crd-isal-peer PRIVATE ISAL_DEPRECATED_INTERNAL)
    target_compile_definitions(crd-isal-peer PUBLIC CRD_ISAL_ASSEMBLY=$<BOOL:${CRD_ISAL_ASSEMBLY}>)
    set_target_properties(crd-isal-peer PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF CRD_ISA_ROLE peer)

    add_executable(crd-hesap-erasure-isal-tests test_erasure_isal.cpp)
    target_link_libraries(crd-hesap-erasure-isal-tests PRIVATE
        crd-core crd-containers crd-hesap-comms crd-hesap-stats crd-isal-peer Catch2::Catch2WithMain)
    crd_discover_tests(crd-hesap-erasure-isal-tests)

    add_executable(crd-hesap-erasure-board erasure_board.cpp)
    target_link_libraries(crd-hesap-erasure-board PRIVATE crd-core crd-containers crd-hesap-comms crd-isal-peer)
    add_test(NAME crd-hesap-erasure-board-smoke COMMAND crd-hesap-erasure-board --quick)
    set_tests_properties(crd-hesap-erasure-board-smoke PROPERTIES TIMEOUT 600 LABELS "board")
endif()
