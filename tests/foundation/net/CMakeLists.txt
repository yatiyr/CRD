# crd-net tests (NET-STD, NET.3, 4.2a): the negotiation codec, the identifiers generated from the standards register, the
# generated wire formats, whose test schema holds quantised fields (crd-net-quantized) and fills the S18 artefact
# (crd-crypto), frames, admission, quotas and reassembly, integrity hashes and padding, state hashes and baseline deltas;
# 4.2a's addresses and UDP sockets on the host's loopback, QUIC headers, admission stages S1 to S3 and the I/O loop,
# QUIC frames and transport parameters, QUIC packets per level, acknowledgements and loss recovery, byte streams and
# flow control, the connection's stream set and datagrams, the connection carrying them, the server behind the
# ingress and the client's host, byte-stream sockets on the loopback and QMux over them.
add_executable(crd-net-tests
    test_admission.cpp
    test_coded_frames.cpp
    test_delta.cpp
    test_endpoint.cpp
    test_frame_integrity.cpp
    test_http3.cpp
    test_http_fields.cpp
    test_http_message.cpp
    test_ingress.cpp
    test_io_loop.cpp
    test_negotiation.cpp
    test_qmux.cpp
    test_qmux_host.cpp
    test_qpack.cpp
    test_quic_client.cpp
    test_quic_connection.cpp
    test_quic_datagram.cpp
    test_quic_frame.cpp
    test_quic_keyed.cpp
    test_quic_packet.cpp
    test_quic_recovery.cpp
    test_quic_server.cpp
    test_quic_stream.cpp
    test_quic_stream_set.cpp
    test_quic_transport_parameters.cpp
    test_register_ids.cpp
    test_state_hash.cpp
    test_stream_poller.cpp
    test_stream_socket.cpp
    test_ticket_cache.cpp
    test_udp.cpp
    test_wire.cpp
)

# RFC 9001's sample packets (quic_vectors.hpp) are shared with crd-crypto's packet protection tests.
target_include_directories(crd-net-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../crypto")

target_link_libraries(crd-net-tests PRIVATE
    crd-core
    crd-compression
    crd-containers
    crd-crypto
    crd-net
    crd-net-quantized
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-net-tests)
crd_discover_tests(crd-net-tests)

# The register identifiers header is scripts/gen-register-ids.py's output for cmake/standards-register.json.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME crd-net-register-ids-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-register-ids.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-net-register-ids-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
    # The wire formats are scripts/gen-wire-schemas.py's output for their authored schemas, and the generator's own
    # refusals (recursion, a changed revision, a bound that does not fit, ...) hold it.
    add_test(NAME crd-net-wire-schemas-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-wire-schemas.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-net-wire-schemas-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
    add_test(NAME crd-net-wire-schemas-tool
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-wire-schemas.py")
    set_tests_properties(crd-net-wire-schemas-tool PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()

# The negotiation decoder and handshake under the bounded fuzz harness, with its committed corpus.
add_subdirectory(fuzz)

# The QUIC and HTTP/3 interoperability tests against ngtcp2 and nghttp3 (CRD_BUILD_QUIC_VS_NGTCP2): crd-ngtcp2-quic-peer
# links ngtcp2, nghttp3 and the peers' OpenSSL alone, and quic_interop.py and http3_interop.py run it against
# crd-quic-peer over the loopback.
if(CRD_BUILD_QUIC_VS_NGTCP2)
    include(ExternalProject)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_package(Threads REQUIRED)
    find_package(ZLIB REQUIRED)
    cmake_host_system_information(RESULT interop_jobs QUERY NUMBER_OF_LOGICAL_CORES)
    crd_pin(ngtcp2_version packages ngtcp2 version)
    # The two HAVE_ values answer the probes ngtcp2's configure would link: this OpenSSL is static and needs zstd and
    # zlib, which that probe does not name.
    set(ngtcp2_prefix "${CMAKE_BINARY_DIR}/_deps/ngtcp2-peer-${ngtcp2_version}")
    set(ngtcp2_libraries "${ngtcp2_prefix}/install/lib/libngtcp2_crypto_ossl.a" "${ngtcp2_prefix}/install/lib/libngtcp2.a")
    ExternalProject_Add(crd-ngtcp2-build
        SOURCE_DIR "${ngtcp2_SOURCE_DIR}"
        PREFIX "${ngtcp2_prefix}"
        BINARY_DIR "${ngtcp2_prefix}/build"
        INSTALL_DIR "${ngtcp2_prefix}/install"
        CMAKE_ARGS "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" -DCMAKE_BUILD_TYPE=Release
                   "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>" -DCMAKE_INSTALL_LIBDIR=lib -DENABLE_LIB_ONLY=ON
                   -DENABLE_SHARED_LIB=OFF -DENABLE_STATIC_LIB=ON -DENABLE_OPENSSL=ON -DBUILD_TESTING=OFF
                   "-DOPENSSL_ROOT_DIR=${CRD_OPENSSL_PEER_ROOT}" -DOPENSSL_USE_STATIC_LIBS=ON
                   -DHAVE_SSL_PROVIDE_QUIC_DATA=0 -DHAVE_SSL_SET_QUIC_TLS_CBS=1
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel ${interop_jobs}
        DEPENDS crd-openssl-peer-build
        BUILD_BYPRODUCTS ${ngtcp2_libraries})
    crd_pin(nghttp3_version packages nghttp3 version)
    set(nghttp3_prefix "${CMAKE_BINARY_DIR}/_deps/nghttp3-peer-${nghttp3_version}")
    set(nghttp3_library "${nghttp3_prefix}/install/lib/libnghttp3.a")
    ExternalProject_Add(crd-nghttp3-build
        SOURCE_DIR "${nghttp3_SOURCE_DIR}"
        PREFIX "${nghttp3_prefix}"
        BINARY_DIR "${nghttp3_prefix}/build"
        INSTALL_DIR "${nghttp3_prefix}/install"
        CMAKE_ARGS "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                   -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>" -DCMAKE_INSTALL_LIBDIR=lib
                   -DENABLE_LIB_ONLY=ON -DENABLE_SHARED_LIB=OFF -DENABLE_STATIC_LIB=ON -DBUILD_TESTING=OFF
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel ${interop_jobs}
        BUILD_BYPRODUCTS ${nghttp3_library})
    add_executable(crd-ngtcp2-quic-peer interop/ngtcp2_quic_peer.c interop/ngtcp2_http3.c)
    add_dependencies(crd-ngtcp2-quic-peer crd-ngtcp2-build crd-nghttp3-build crd-openssl-peer-build)
    target_include_directories(crd-ngtcp2-quic-peer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../crypto/interop")
    target_include_directories(crd-ngtcp2-quic-peer SYSTEM PRIVATE "${ngtcp2_prefix}/install/include"
                                                                    "${nghttp3_prefix}/install/include"
                                                                    "${CRD_OPENSSL_PEER_ROOT}/include")
    target_link_libraries(crd-ngtcp2-quic-peer PRIVATE ${ngtcp2_libraries} "${nghttp3_library}" ${CRD_OPENSSL_PEER_LINK})
    set_target_properties(crd-ngtcp2-quic-peer PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

    add_executable(crd-quic-peer interop/cerid_quic_peer.cpp)
    target_link_libraries(crd-quic-peer PRIVATE crd-core crd-containers crd-crypto crd-memory crd-net)

    add_test(NAME crd-net-quic-interop-ngtcp2
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/quic_interop.py"
                     --cerid "$<TARGET_FILE:crd-quic-peer>" --ngtcp2 "$<TARGET_FILE:crd-ngtcp2-quic-peer>")
    set_tests_properties(crd-net-quic-interop-ngtcp2 PROPERTIES TIMEOUT 1500 LABELS "net;quic-interop")
    add_test(NAME crd-net-http3-interop-nghttp3
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/http3_interop.py"
                     --cerid "$<TARGET_FILE:crd-quic-peer>" --nghttp3 "$<TARGET_FILE:crd-ngtcp2-quic-peer>")
    set_tests_properties(crd-net-http3-interop-nghttp3 PROPERTIES TIMEOUT 1500 LABELS "net;http3-interop")
    message(STATUS "[quic-vs-ngtcp2] QUIC and HTTP/3 interoperability against ngtcp2 ${ngtcp2_version} and nghttp3 ${nghttp3_version}")
endif()

# The QMux interoperability test against the `qmux` crate (CRD_BUILD_QMUX_VS_CRATE): crd-qmux-crate-peer is a Cargo
# package over the crate and rustls, and qmux_interop.py runs it against crd-qmux-peer over the loopback.
if(CRD_BUILD_QMUX_VS_CRATE)
    crd_add_cargo_peer(crd-qmux-crate-peer "${CMAKE_CURRENT_SOURCE_DIR}/interop/qmux_crate_peer/Cargo.toml"
                       qmux-crate-peer crd-qmux-crate-peer qmux-vs-crate)
    add_executable(crd-qmux-peer interop/cerid_qmux_peer.cpp)
    target_link_libraries(crd-qmux-peer PRIVATE crd-net)
    add_dependencies(crd-qmux-peer crd-qmux-crate-peer)
    add_test(NAME crd-net-qmux-interop-crate
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/qmux_interop.py"
                     --cerid "$<TARGET_FILE:crd-qmux-peer>" --crate "${crd-qmux-crate-peer_OUTPUT}")
    set_tests_properties(crd-net-qmux-interop-crate PROPERTIES TIMEOUT 1200 LABELS "net;qmux-interop")
endif()

# QPACK against the offline interop corpus (CRD_BUILD_QPACK_VS_QIFS): crd-qpack-replay decodes what other encoders wrote
# and qpack_interop.py compares it with the QIF text and sets crd-net's encoder beside them.
if(CRD_BUILD_QPACK_VS_QIFS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(crd-qpack-replay interop/cerid_qpack_replay.cpp)
    target_link_libraries(crd-qpack-replay PRIVATE crd-memory crd-net)
    add_test(NAME crd-net-qpack-interop-qifs
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/qpack_interop.py"
                     --replay "$<TARGET_FILE:crd-qpack-replay>" --corpus "${qifs_SOURCE_DIR}")
    set_tests_properties(crd-net-qpack-interop-qifs PROPERTIES TIMEOUT 900 LABELS "net;qpack-interop")
endif()

# NET.3's serializer budget board against its pinned peers (CRD_BUILD_SERIALIZE_VS_PEERS, the root CMakeLists).
if(CRD_BUILD_SERIALIZE_VS_PEERS)
    add_subdirectory(board)
endif()
