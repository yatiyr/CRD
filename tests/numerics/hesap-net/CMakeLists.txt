# HESAP-NET's replicated-motion peer (CRD_BUILD_MOTION_VS_PEERS, the root CMakeLists): the snapshot interpolators cut
# verbatim from the pinned Snapshot.h and nlerp and slerp from the pinned quat4f.h, with the engine's floating-point
# contract. crd-hesap-motion-peer-tests holds hesap-net to them bit for bit; crd-hesap-motion-board times them.
if(CRD_BUILD_MOTION_VS_PEERS)
    file(READ "${CRD_NETWORKED_PHYSICS_SNAPSHOT}" snapshot_header)
    string(FIND "${snapshot_header}" "static void InterpolateSnapshot_Linear(" interpolation_slice_begin)
    string(FIND "${snapshot_header}" "struct Snapshot\n{" interpolation_slice_end)
    if(interpolation_slice_begin EQUAL -1 OR interpolation_slice_end LESS_EQUAL interpolation_slice_begin)
        message(FATAL_ERROR "[motion-vs-peers] the interpolators not found in ${CRD_NETWORKED_PHYSICS_SNAPSHOT}")
    endif()
    math(EXPR interpolation_slice_length "${interpolation_slice_end} - ${interpolation_slice_begin}")
    string(SUBSTRING "${snapshot_header}" ${interpolation_slice_begin} ${interpolation_slice_length} interpolation_slice)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/motion-slice/networked_physics_interpolation_slice.inc"
         CONTENT "${interpolation_slice}" @ONLY)

    file(READ "${CRD_VECTORIAL_QUAT4F}" quat4f_header)
    string(FIND "${quat4f_header}" "static inline quat4f nlerp(" quat4f_slice_begin)
    string(FIND "${quat4f_header}" "static inline quat4f multiply(" quat4f_slice_end)
    if(quat4f_slice_begin EQUAL -1 OR quat4f_slice_end LESS_EQUAL quat4f_slice_begin)
        message(FATAL_ERROR "[motion-vs-peers] nlerp and slerp not found in ${CRD_VECTORIAL_QUAT4F}")
    endif()
    math(EXPR quat4f_slice_length "${quat4f_slice_end} - ${quat4f_slice_begin}")
    string(SUBSTRING "${quat4f_header}" ${quat4f_slice_begin} ${quat4f_slice_length} quat4f_slice)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/motion-slice/vectorial_quat4f_slice.inc"
         CONTENT "${quat4f_slice}" @ONLY)

    add_library(crd-motion-peers STATIC motion_peers.cpp)
    target_include_directories(crd-motion-peers PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/motion-slice")
    target_include_directories(crd-motion-peers PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(crd-motion-peers PUBLIC cxx_std_20)
    target_compile_options(crd-motion-peers PRIVATE ${CRD_FP_CONTRACT_OPTIONS} $<$<CXX_COMPILER_ID:GNU,Clang>:-w>)
    set_target_properties(crd-motion-peers PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF CRD_ISA_ROLE peer)

    add_executable(crd-hesap-motion-peer-tests test_motion_peers.cpp)
    target_link_libraries(crd-hesap-motion-peer-tests PRIVATE
        crd-core crd-containers crd-hesap-net crd-motion-peers Catch2::Catch2WithMain)
    target_include_directories(crd-hesap-motion-peer-tests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests/support/kernel-levels")
    crd_discover_tests(crd-hesap-motion-peer-tests)

    add_executable(crd-hesap-motion-board motion_board.cpp)
    target_link_libraries(crd-hesap-motion-board PRIVATE crd-core crd-containers crd-hesap-net crd-motion-peers)
    target_include_directories(crd-hesap-motion-board PRIVATE "${PROJECT_SOURCE_DIR}/tests/support/kernel-levels")
    add_test(NAME crd-hesap-motion-board-smoke COMMAND crd-hesap-motion-board --quick)
    set_tests_properties(crd-hesap-motion-board-smoke PROPERTIES TIMEOUT 600 LABELS "board")
endif()
add_executable(crd-hesap-net-tests
    test_erasure.cpp
    test_flexfec.cpp
    test_red.cpp
    test_interleave.cpp
    test_estimators.cpp
    test_clock.cpp
    test_quantize.cpp
    test_motion.cpp
)

target_link_libraries(crd-hesap-net-tests PRIVATE
    crd-core
    crd-containers
    crd-hesap-net
    Catch2::Catch2WithMain
)

# test_motion.cpp restates the kernels' expressions, whose bits assume no contraction, as the kernels' own units do.
target_compile_options(crd-hesap-net-tests PRIVATE ${CRD_FP_CONTRACT_OPTIONS})

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

# HESAP-NET's libwebrtc peer (CRD_BUILD_ESTIMATORS_VS_WEBRTC, the root CMakeLists): the pinned branch's estimator and
# unit files verbatim, webrtc-shim/ only for their checks, logging and field-trial parsing and their one pow, the
# engine's. It takes the engine's floating-point contract, so the two sides round alike.
# crd-hesap-estimators-webrtc-tests holds the delay trend, the acknowledged rate, the media clock map and the capture
# aligner to it bit for bit; crd-hesap-estimators-board times the estimators, crd-hesap-clock-board the clock pair.
if(CRD_BUILD_ESTIMATORS_VS_WEBRTC)
    set(webrtc_estimators "${CRD_WEBRTC_BWE}/modules")
    add_library(crd-webrtc-bwe-peer STATIC
        "${webrtc_estimators}/congestion_controller/goog_cc/bitrate_estimator.cc"
        "${webrtc_estimators}/congestion_controller/goog_cc/inter_arrival_delta.cc"
        "${webrtc_estimators}/congestion_controller/goog_cc/trendline_estimator.cc"
        "${webrtc_estimators}/remote_bitrate_estimator/overuse_detector.cc"
        "${webrtc_estimators}/remote_bitrate_estimator/overuse_estimator.cc"
        "${CRD_WEBRTC_BWE}/rtc_base/rtp_to_ntp_estimator.cc"
        "${CRD_WEBRTC_BWE}/rtc_base/timestamp_aligner.cc")
    target_include_directories(crd-webrtc-bwe-peer SYSTEM PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/webrtc-shim" "${CRD_WEBRTC_BWE}")
    target_link_libraries(crd-webrtc-bwe-peer PUBLIC crd-math)
    target_compile_features(crd-webrtc-bwe-peer PUBLIC cxx_std_20)
    target_compile_options(crd-webrtc-bwe-peer PRIVATE ${CRD_FP_CONTRACT_OPTIONS} $<$<CXX_COMPILER_ID:GNU,Clang>:-w>)
    set_target_properties(crd-webrtc-bwe-peer PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF CRD_ISA_ROLE peer)

    add_executable(crd-hesap-estimators-webrtc-tests test_estimators_webrtc.cpp)
    target_link_libraries(crd-hesap-estimators-webrtc-tests PRIVATE
        crd-core crd-containers crd-hesap-net crd-webrtc-bwe-peer Catch2::Catch2WithMain)
    crd_discover_tests(crd-hesap-estimators-webrtc-tests)

    add_executable(crd-hesap-estimators-board estimators_board.cpp)
    target_link_libraries(crd-hesap-estimators-board PRIVATE crd-core crd-containers crd-hesap-net crd-webrtc-bwe-peer)
    add_test(NAME crd-hesap-estimators-board-smoke COMMAND crd-hesap-estimators-board --quick)
    set_tests_properties(crd-hesap-estimators-board-smoke PROPERTIES TIMEOUT 600 LABELS "board")
endif()

# HESAP-NET's ntpd peer (CRD_BUILD_CLOCK_VS_NTPD, the root CMakeLists): the pinned release's clock_filter(), cut
# verbatim from ntpd/ntp_proto.c here and compiled on ntpd_peer.c's few stand-ins, with the engine's floating-point contract.
# crd-hesap-clock-ntpd-tests holds ClockFilter to it bit for bit; crd-hesap-clock-board, with the libwebrtc peer, times
# them and reports the accuracy on the simulated paths.
if(CRD_BUILD_CLOCK_VS_NTPD)
    file(READ "${ntp_SOURCE_DIR}/ntpd/ntp_proto.c" ntp_proto)
    string(FIND "${ntp_proto}" "/*\n * clock_filter - add incoming" ntp_slice_begin)
    string(FIND "${ntp_proto}" "/*\n * clock_select - find" ntp_slice_end)
    if(ntp_slice_begin EQUAL -1 OR ntp_slice_end LESS_EQUAL ntp_slice_begin)
        message(FATAL_ERROR "[clock-vs-ntpd] clock_filter() not found in ${ntp_SOURCE_DIR}/ntpd/ntp_proto.c")
    endif()
    math(EXPR ntp_slice_length "${ntp_slice_end} - ${ntp_slice_begin}")
    string(SUBSTRING "${ntp_proto}" ${ntp_slice_begin} ${ntp_slice_length} ntp_slice)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ntpd-slice/ntpd_clock_filter_slice.inc"
         CONTENT "${ntp_slice}" @ONLY)

    add_library(crd-ntpd-peer STATIC ntpd_peer.c)
    target_include_directories(crd-ntpd-peer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ntpd-slice")
    target_include_directories(crd-ntpd-peer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(crd-ntpd-peer PRIVATE ${CRD_FP_CONTRACT_OPTIONS} $<$<C_COMPILER_ID:GNU,Clang>:-w>)
    target_link_libraries(crd-ntpd-peer PRIVATE m)
    set_target_properties(crd-ntpd-peer PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF CRD_ISA_ROLE peer)

    add_executable(crd-hesap-clock-ntpd-tests test_clock_ntpd.cpp)
    target_link_libraries(crd-hesap-clock-ntpd-tests PRIVATE
        crd-core crd-containers crd-hesap-net crd-ntpd-peer Catch2::Catch2WithMain)
    crd_discover_tests(crd-hesap-clock-ntpd-tests)

    if(CRD_BUILD_ESTIMATORS_VS_WEBRTC)
        add_executable(crd-hesap-clock-board clock_board.cpp)
        target_link_libraries(crd-hesap-clock-board PRIVATE
            crd-core crd-containers crd-hesap-net crd-ntpd-peer crd-webrtc-bwe-peer)
        add_test(NAME crd-hesap-clock-board-smoke COMMAND crd-hesap-clock-board --quick)
        set_tests_properties(crd-hesap-clock-board-smoke PROPERTIES TIMEOUT 600 LABELS "board")
    endif()
endif()

# HESAP-NET's quantisation peers (CRD_BUILD_QUANTIZE_VS_PEERS, the root CMakeLists): serialize.h as released and the
# compressed_quaternion structs cut verbatim from the pinned Snapshot.h, with the engine's floating-point contract.
# crd-hesap-quantize-peer-tests holds hesap-net to them bit for bit; crd-hesap-quantize-board times them.
if(CRD_BUILD_QUANTIZE_VS_PEERS)
    file(READ "${CRD_NETWORKED_PHYSICS_SNAPSHOT}" snapshot_header)
    string(FIND "${snapshot_header}" "template <int bits> struct compressed_quaternion\n" quaternion_slice_begin)
    string(FIND "${snapshot_header}" "template <typename Stream> inline void serialize_compressed_quaternion("
           quaternion_slice_end)
    if(quaternion_slice_begin EQUAL -1 OR quaternion_slice_end LESS_EQUAL quaternion_slice_begin)
        message(FATAL_ERROR "[quantize-vs-peers] compressed_quaternion not found in ${CRD_NETWORKED_PHYSICS_SNAPSHOT}")
    endif()
    math(EXPR quaternion_slice_length "${quaternion_slice_end} - ${quaternion_slice_begin}")
    string(SUBSTRING "${snapshot_header}" ${quaternion_slice_begin} ${quaternion_slice_length} quaternion_slice)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/quantize-slice/networked_physics_quaternion_slice.inc"
         CONTENT "${quaternion_slice}" @ONLY)

    add_library(crd-quantize-peers STATIC quantize_peers.cpp)
    target_include_directories(crd-quantize-peers PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/quantize-slice")
    target_include_directories(crd-quantize-peers SYSTEM PRIVATE "${serialize_SOURCE_DIR}")
    target_include_directories(crd-quantize-peers PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(crd-quantize-peers PUBLIC cxx_std_20)
    target_compile_options(crd-quantize-peers PRIVATE ${CRD_FP_CONTRACT_OPTIONS} $<$<CXX_COMPILER_ID:GNU,Clang>:-w>)
    set_target_properties(crd-quantize-peers PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF CRD_ISA_ROLE peer)

    add_executable(crd-hesap-quantize-peer-tests test_quantize_peers.cpp)
    target_link_libraries(crd-hesap-quantize-peer-tests PRIVATE
        crd-core crd-containers crd-hesap-net crd-quantize-peers Catch2::Catch2WithMain)
    target_include_directories(crd-hesap-quantize-peer-tests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests/support/kernel-levels")
    crd_discover_tests(crd-hesap-quantize-peer-tests)

    add_executable(crd-hesap-quantize-board quantize_board.cpp)
    target_link_libraries(crd-hesap-quantize-board PRIVATE crd-core crd-containers crd-hesap-net crd-quantize-peers)
    target_include_directories(crd-hesap-quantize-board PRIVATE "${PROJECT_SOURCE_DIR}/tests/support/kernel-levels")
    add_test(NAME crd-hesap-quantize-board-smoke COMMAND crd-hesap-quantize-board --quick)
    set_tests_properties(crd-hesap-quantize-board-smoke PROPERTIES TIMEOUT 600 LABELS "board")
endif()
