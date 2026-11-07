# NET.3's serializer budget board (CRD_BUILD_SERIALIZE_VS_PEERS, the root CMakeLists): crd-net's generated codec against
# mas-bandwidth/serialize with networked-physics' smallest three (crd-networked-physics-peer), bitsery, zpp_bits and
# protobuf's format through protozero, as released. Each peer is its own unit of peer code: no engine headers, its
# library's warnings off, no link-time optimisation, the engine's floating-point contract (serialize's floats need it).
add_library(crd-serialize-peers STATIC peer_serialize.cpp peer_bitsery.cpp peer_zpp_bits.cpp peer_protozero.cpp)
target_link_libraries(crd-serialize-peers PRIVATE crd-networked-physics-peer)
target_include_directories(crd-serialize-peers SYSTEM PRIVATE
    "${bitsery_SOURCE_DIR}/include" "${zpp_bits_SOURCE_DIR}" "${protozero_SOURCE_DIR}/include")
target_include_directories(crd-serialize-peers PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(crd-serialize-peers PUBLIC cxx_std_20)
target_compile_options(crd-serialize-peers PRIVATE ${CRD_FP_CONTRACT_OPTIONS} $<$<CXX_COMPILER_ID:GNU,Clang>:-w>)
set_target_properties(crd-serialize-peers PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF CRD_ISA_ROLE peer)

add_executable(crd-net-serialize-board serialize_board.cpp)
target_include_directories(crd-net-serialize-board PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
# The quantised fields inline hesap-net's laws here: the engine's floating-point contract keeps them serialize's.
target_compile_options(crd-net-serialize-board PRIVATE ${CRD_FP_CONTRACT_OPTIONS})
target_link_libraries(crd-net-serialize-board PRIVATE
    crd-core crd-containers crd-net crd-net-quantized crd-serialize-peers)
add_test(NAME crd-net-serialize-board-smoke COMMAND crd-net-serialize-board --quick)
set_tests_properties(crd-net-serialize-board-smoke PROPERTIES TIMEOUT 600 LABELS "board")
