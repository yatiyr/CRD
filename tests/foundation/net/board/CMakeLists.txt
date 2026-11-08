# NET.3's serializer budget board (CRD_BUILD_SERIALIZE_VS_PEERS, the root CMakeLists): crd-net's generated codec against
# mas-bandwidth/serialize with networked-physics' smallest three (crd-networked-physics-peer), bitsery, zpp_bits,
# protobuf's format through protozero, FlatBuffers and Cap'n Proto, as released. Each peer is its own unit of peer code:
# no engine headers, its library's warnings off, no link-time optimisation, the engine's floating-point contract
# (serialize's floats need it).

# FlatBuffers' and Cap'n Proto's schema compilers, and Cap'n Proto's runtime, each from its own CMake as a separate
# project, so their flags and targets stay out of the engine's build; each prefix names its version, so a new pin
# builds afresh. FlatBuffers' runtime is header only.
include(ExternalProject)
find_package(Threads REQUIRED)
cmake_host_system_information(RESULT peer_jobs QUERY NUMBER_OF_LOGICAL_CORES)
crd_pin(flatbuffers_version packages flatbuffers version)
crd_pin(capnproto_version packages capnproto version)
set(peer_tool_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                   -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF)

set(flatbuffers_prefix "${CMAKE_BINARY_DIR}/_deps/flatbuffers-peer-${flatbuffers_version}")
set(flatc "${flatbuffers_prefix}/build/flatc${CMAKE_EXECUTABLE_SUFFIX}")
ExternalProject_Add(crd-flatbuffers-peer-build
    SOURCE_DIR "${flatbuffers_SOURCE_DIR}"
    PREFIX "${flatbuffers_prefix}"
    BINARY_DIR "${flatbuffers_prefix}/build"
    CMAKE_ARGS ${peer_tool_args} -DFLATBUFFERS_BUILD_TESTS=OFF -DFLATBUFFERS_INSTALL=OFF
               -DFLATBUFFERS_BUILD_FLATLIB=OFF -DFLATBUFFERS_BUILD_FLATC=ON -DFLATBUFFERS_BUILD_FLATHASH=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target flatc --parallel ${peer_jobs}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${flatc}")

set(capnproto_prefix "${CMAKE_BINARY_DIR}/_deps/capnproto-peer-${capnproto_version}")
set(capnproto_build "${capnproto_prefix}/build/src")
set(capnp "${capnproto_build}/capnp/capnp${CMAKE_EXECUTABLE_SUFFIX}")
set(capnpc_cpp "${capnproto_build}/capnp/capnpc-c++${CMAKE_EXECUTABLE_SUFFIX}")
set(capnproto_libraries "${capnproto_build}/capnp/${CMAKE_STATIC_LIBRARY_PREFIX}capnp${CMAKE_STATIC_LIBRARY_SUFFIX}"
                        "${capnproto_build}/kj/${CMAKE_STATIC_LIBRARY_PREFIX}kj${CMAKE_STATIC_LIBRARY_SUFFIX}")
ExternalProject_Add(crd-capnproto-peer-build
    SOURCE_DIR "${capnproto_SOURCE_DIR}/c++"
    PREFIX "${capnproto_prefix}"
    BINARY_DIR "${capnproto_prefix}/build"
    CMAKE_ARGS ${peer_tool_args} -DBUILD_TESTING=OFF -DWITH_OPENSSL=OFF -DWITH_ZLIB=OFF -DWITH_FIBERS=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target capnp_tool capnpc_cpp capnp kj
                  --parallel ${peer_jobs}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${capnp}" "${capnpc_cpp}" ${capnproto_libraries})

# The two schemas' C++, generated into the build tree on every change of a schema or a compiler.
set(peer_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${peer_generated}/board_generated.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${peer_generated}"
    COMMAND "${flatc}" --cpp -o "${peer_generated}" "${CMAKE_CURRENT_SOURCE_DIR}/board.fbs"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/board.fbs" crd-flatbuffers-peer-build "${flatc}"
    COMMENT "flatc: board.fbs" VERBATIM)
add_custom_command(OUTPUT "${peer_generated}/board.capnp.h" "${peer_generated}/board.capnp.c++"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${peer_generated}"
    COMMAND "${capnp}" compile --no-standard-import "-I${capnproto_SOURCE_DIR}/c++/src"
            "--src-prefix=${CMAKE_CURRENT_SOURCE_DIR}" "-o${capnpc_cpp}:${peer_generated}"
            "${CMAKE_CURRENT_SOURCE_DIR}/board.capnp"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/board.capnp" crd-capnproto-peer-build "${capnp}" "${capnpc_cpp}"
    COMMENT "capnp: board.capnp" VERBATIM)
set_source_files_properties("${peer_generated}/board.capnp.c++" PROPERTIES LANGUAGE CXX)

add_library(crd-serialize-peers STATIC peer_serialize.cpp peer_bitsery.cpp peer_zpp_bits.cpp peer_protozero.cpp
            peer_flatbuffers.cpp peer_capnproto.cpp "${peer_generated}/board_generated.h"
            "${peer_generated}/board.capnp.h" "${peer_generated}/board.capnp.c++")
add_dependencies(crd-serialize-peers crd-flatbuffers-peer-build crd-capnproto-peer-build)
target_link_libraries(crd-serialize-peers PRIVATE crd-networked-physics-peer ${capnproto_libraries} Threads::Threads)
target_include_directories(crd-serialize-peers SYSTEM PRIVATE
    "${bitsery_SOURCE_DIR}/include" "${zpp_bits_SOURCE_DIR}" "${protozero_SOURCE_DIR}/include"
    "${flatbuffers_SOURCE_DIR}/include" "${capnproto_SOURCE_DIR}/c++/src" "${peer_generated}")
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
