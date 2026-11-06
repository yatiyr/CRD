add_executable(crd-hesap-net-tests
    test_erasure.cpp
    test_flexfec.cpp
    test_red.cpp
    test_interleave.cpp
)

target_link_libraries(crd-hesap-net-tests PRIVATE
    crd-core
    crd-containers
    crd-hesap-net
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-net-tests)

crd_discover_tests(crd-hesap-net-tests)

# ADR-0134 §8: the [kernels] cases once per compiled level (crd-hesap-net-kernels-at-<level>).
crd_add_kernel_level_tests(crd-hesap-net-tests "[kernels]" SDE_CPUS icx)

# The erasure code and its kernels under the bounded fuzz harness, with its committed corpus.
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
        crd-core crd-containers crd-hesap-net crd-isal-peer Catch2::Catch2WithMain)
    crd_discover_tests(crd-hesap-erasure-isal-tests)

    add_executable(crd-hesap-erasure-board erasure_board.cpp)
    target_link_libraries(crd-hesap-erasure-board PRIVATE crd-core crd-containers crd-hesap-net crd-isal-peer)
    add_test(NAME crd-hesap-erasure-board-smoke COMMAND crd-hesap-erasure-board --quick)
    set_tests_properties(crd-hesap-erasure-board-smoke PROPERTIES TIMEOUT 600 LABELS "board")
endif()

# HESAP-NET's OpenFEC peer (CRD_BUILD_ERASURE_VS_OPENFEC, the root CMakeLists): the pinned release's Reed-Solomon codec
# over GF(2^m), compiled as crd-openfec-peer with the profile its own build generates (no assembly option).
if(CRD_BUILD_ERASURE_VS_OPENFEC)
    set(openfec_rs "${openfec_SOURCE_DIR}/src/lib_stable/reed-solomon_gf_2_m")
    # OpenFEC's codec profile names its generated header as ../../lib_common/of_build_config.h, its own source
    # directory: the downloaded tree stays read only, so an empty directory two levels down resolves that spelling
    # to the binary tree.
    set(openfec_config "${CMAKE_CURRENT_BINARY_DIR}/openfec-config")
    set(ASSEMBLY_SSE_OPT OFF)
    configure_file("${openfec_SOURCE_DIR}/src/lib_common/of_build_config.h.in"
                   "${openfec_config}/lib_common/of_build_config.h")
    file(MAKE_DIRECTORY "${openfec_config}/profile/codec")
    add_library(crd-openfec-peer STATIC
        openfec_peer.c
        "${openfec_SOURCE_DIR}/src/lib_common/of_mem.c"
        "${openfec_rs}/of_reed-solomon_gf_2_m_api.c"
        "${openfec_rs}/galois_field_codes_utils/of_galois_field_code.c"
        "${openfec_rs}/galois_field_codes_utils/algebra_2_4.c"
        "${openfec_rs}/galois_field_codes_utils/algebra_2_8.c")
    target_include_directories(crd-openfec-peer PRIVATE
        "${openfec_config}/lib_common" "${openfec_config}/profile/codec" "${openfec_SOURCE_DIR}/src/lib_common"
        "${openfec_rs}")
    # Its own build derives the endianness from a CMake probe; every Cerid target is little-endian.
    target_compile_definitions(crd-openfec-peer PRIVATE OF_USE_REED_SOLOMON_2_M_CODEC OPENFEC_LITTLE_ENDIAN)
    target_compile_options(crd-openfec-peer PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:-w>)
    set_target_properties(crd-openfec-peer PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF CRD_ISA_ROLE peer)

    add_executable(crd-hesap-erasure-openfec-tests test_erasure_openfec.cpp)
    target_link_libraries(crd-hesap-erasure-openfec-tests PRIVATE
        crd-core crd-containers crd-hesap-net crd-openfec-peer Catch2::Catch2WithMain)
    crd_discover_tests(crd-hesap-erasure-openfec-tests)
endif()
