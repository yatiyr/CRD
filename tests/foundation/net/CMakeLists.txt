# crd-net tests (NET-STD, NET.3, 4.2a): the negotiation codec, the identifiers generated from the standards register, the
# generated wire formats, whose test schema holds quantised fields (crd-net-quantized) and fills the S18 artefact
# (crd-crypto), frames, admission, quotas and reassembly, integrity hashes and padding, state hashes and baseline deltas;
# 4.2a's addresses and UDP sockets on the host's loopback, QUIC headers, admission stages S1 to S3 and the I/O loop,
# QUIC frames and transport parameters, QUIC packets per level, acknowledgements and loss recovery, byte streams and
# flow control, the connection's stream set and datagrams, the connection carrying them, the server behind the
# ingress and the client's host, byte-stream sockets on the loopback and QMux over them, also as WebSocket tunnels over
# HTTP/2 and HTTP/3.
add_executable(crd-net-tests
    test_admission.cpp
    test_channel_link.cpp
    test_coded_frames.cpp
    test_delta.cpp
    test_endpoint.cpp
    test_frame_integrity.cpp
    test_hpack.cpp
    test_http2.cpp
    test_http3.cpp
    test_http3_webtransport.cpp
    test_webtransport.cpp
    test_websocket.cpp
    test_websocket_tunnel.cpp
    test_http_fields.cpp
    test_http_message.cpp
    test_ingress.cpp
    test_io_loop.cpp
    test_negotiation.cpp
    test_qmux.cpp
    test_qmux_host.cpp
    test_qmux_http2.cpp
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

# crd-net's end of the QUIC and HTTP/3 interoperability tests, whichever peers this configuration builds.
if(CRD_BUILD_QUIC_VS_NGTCP2 OR CRD_BUILD_HTTP3_VS_QUICHE OR CRD_BUILD_WEBTRANSPORT_VS_QUINN OR CRD_BUILD_QMUX_VS_CRATE)
    add_executable(crd-quic-peer interop/cerid_quic_peer.cpp)
    target_link_libraries(crd-quic-peer PRIVATE crd-core crd-containers crd-crypto crd-memory crd-net)
endif()

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

# The HTTP/3 interoperability test against quiche (CRD_BUILD_HTTP3_VS_QUICHE): crd-quiche-peer is a Cargo package over
# the crate and BoringSSL, and http3_interop.py runs it against crd-quic-peer over the loopback.
if(CRD_BUILD_HTTP3_VS_QUICHE)
    crd_add_cargo_peer(crd-quiche-peer "${CMAKE_CURRENT_SOURCE_DIR}/interop/quiche_peer/Cargo.toml"
                       quiche-peer crd-quiche-peer http3-vs-quiche)
    add_dependencies(crd-quic-peer crd-quiche-peer)
    add_test(NAME crd-net-http3-interop-quiche
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/http3_interop.py"
                     --cerid "$<TARGET_FILE:crd-quic-peer>" --quiche "${crd-quiche-peer_OUTPUT}")
    set_tests_properties(crd-net-http3-interop-quiche PROPERTIES TIMEOUT 1200 LABELS "net;http3-interop")
endif()

# The WebTransport interoperability test against web-transport-quinn (CRD_BUILD_WEBTRANSPORT_VS_QUINN):
# crd-webtransport-quinn-peer is a Cargo package over the crate and rustls, and webtransport_interop.py runs it against
# crd-quic-peer over the loopback, and crd-quic-peer against itself in both revisions.
if(CRD_BUILD_WEBTRANSPORT_VS_QUINN)
    crd_add_cargo_peer(crd-webtransport-quinn-peer "${CMAKE_CURRENT_SOURCE_DIR}/interop/webtransport_peer/Cargo.toml"
                       webtransport-quinn-peer crd-webtransport-quinn-peer webtransport-vs-quinn)
    add_dependencies(crd-quic-peer crd-webtransport-quinn-peer)
    add_test(NAME crd-net-webtransport-interop-quinn
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/webtransport_interop.py"
                     --cerid "$<TARGET_FILE:crd-quic-peer>" --quinn "${crd-webtransport-quinn-peer_OUTPUT}")
    set_tests_properties(crd-net-webtransport-interop-quinn PROPERTIES TIMEOUT 1500 LABELS "net;webtransport-interop")
endif()

# The same against webtransport-go (CRD_BUILD_WEBTRANSPORT_VS_GO): crd-webtransport-go-peer is a Go module over
# webtransport-go and quic-go, and the driver runs only its cases here: the `webtransport-h3` spelling and flow control.
if(CRD_BUILD_WEBTRANSPORT_VS_GO)
    crd_add_go_peer(crd-webtransport-go-peer "${CMAKE_CURRENT_SOURCE_DIR}/interop/webtransport_go_peer"
                    webtransport-go-peer crd-webtransport-go-peer webtransport-vs-go)
    add_dependencies(crd-quic-peer crd-webtransport-go-peer)
    add_test(NAME crd-net-webtransport-interop-go
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/webtransport_interop.py"
                     --cerid "$<TARGET_FILE:crd-quic-peer>" --go "${crd-webtransport-go-peer_OUTPUT}" --only "with go")
    set_tests_properties(crd-net-webtransport-interop-go PROPERTIES TIMEOUT 1200 LABELS "net;webtransport-interop")
endif()

# The QMux interoperability test against the `qmux` crate (CRD_BUILD_QMUX_VS_CRATE): crd-qmux-crate-peer is a Cargo
# package over the crate and rustls (and the `h2` and `h3` crates for the WebSocket tunnels), and qmux_interop.py runs
# it against crd-qmux-peer over the loopback, and against crd-quic-peer for the tunnels over HTTP/3.
if(CRD_BUILD_QMUX_VS_CRATE)
    crd_add_cargo_peer(crd-qmux-crate-peer "${CMAKE_CURRENT_SOURCE_DIR}/interop/qmux_crate_peer/Cargo.toml"
                       qmux-crate-peer crd-qmux-crate-peer qmux-vs-crate
                       PATCH h3 "${h3-crates_SOURCE_DIR}/h3" h3-quinn "${h3-crates_SOURCE_DIR}/h3-quinn")
    add_executable(crd-qmux-peer interop/cerid_qmux_peer.cpp)
    target_link_libraries(crd-qmux-peer PRIVATE crd-net)
    add_dependencies(crd-qmux-peer crd-qmux-crate-peer)
    add_dependencies(crd-quic-peer crd-qmux-crate-peer)
    add_test(NAME crd-net-qmux-interop-crate
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/qmux_interop.py"
                     --cerid "$<TARGET_FILE:crd-qmux-peer>" --quic "$<TARGET_FILE:crd-quic-peer>"
                     --crate "${crd-qmux-crate-peer_OUTPUT}")
    set_tests_properties(crd-net-qmux-interop-crate PROPERTIES TIMEOUT 2400 LABELS "net;qmux-interop")
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

# HTTP/2 against nghttp2 and h2spec (CRD_BUILD_HTTP2_VS_PEERS): crd-nghttp2-peer links nghttp2 alone, and
# http2_interop.py runs it, and h2spec, against crd-http2-peer over the loopback.
if(CRD_BUILD_HTTP2_VS_PEERS)
    include(ExternalProject)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    cmake_host_system_information(RESULT http2_interop_jobs QUERY NUMBER_OF_LOGICAL_CORES)
    crd_pin(nghttp2_version packages nghttp2 version)
    set(nghttp2_prefix "${CMAKE_BINARY_DIR}/_deps/nghttp2-peer-${nghttp2_version}")
    set(nghttp2_library "${nghttp2_prefix}/install/lib/libnghttp2.a")
    ExternalProject_Add(crd-nghttp2-build
        SOURCE_DIR "${nghttp2_SOURCE_DIR}"
        PREFIX "${nghttp2_prefix}"
        BINARY_DIR "${nghttp2_prefix}/build"
        INSTALL_DIR "${nghttp2_prefix}/install"
        CMAKE_ARGS "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                   -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>" -DCMAKE_INSTALL_LIBDIR=lib
                   -DENABLE_LIB_ONLY=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DENABLE_DOC=OFF
                   -DBUILD_TESTING=OFF
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel ${http2_interop_jobs}
        BUILD_BYPRODUCTS "${nghttp2_library}")
    add_executable(crd-nghttp2-peer interop/nghttp2_http2_peer.c)
    add_dependencies(crd-nghttp2-peer crd-nghttp2-build)
    target_include_directories(crd-nghttp2-peer SYSTEM PRIVATE "${nghttp2_prefix}/install/include")
    target_compile_definitions(crd-nghttp2-peer PRIVATE NGHTTP2_STATICLIB)
    target_link_libraries(crd-nghttp2-peer PRIVATE "${nghttp2_library}")
    set_target_properties(crd-nghttp2-peer PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    add_executable(crd-http2-peer interop/cerid_http2_peer.cpp)
    target_link_libraries(crd-http2-peer PRIVATE crd-memory crd-net)
    add_test(NAME crd-net-http2-interop
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/http2_interop.py"
                     --cerid "$<TARGET_FILE:crd-http2-peer>" --nghttp2 "$<TARGET_FILE:crd-nghttp2-peer>"
                     --h2spec "${h2spec_SOURCE_DIR}/h2spec")
    set_tests_properties(crd-net-http2-interop PROPERTIES TIMEOUT 1500 LABELS "net;http2-interop")
    message(STATUS "[http2-vs-peers] HTTP/2 interoperability against nghttp2 ${nghttp2_version} and h2spec")
endif()

# HPACK against the hpack-test-case corpus (CRD_BUILD_HPACK_VS_CORPUS): crd-hpack-replay decodes what other encoders wrote
# and hpack_interop.py compares it with each story and sets crd-net's encoder beside them.
if(CRD_BUILD_HPACK_VS_CORPUS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(crd-hpack-replay interop/cerid_hpack_replay.cpp)
    target_link_libraries(crd-hpack-replay PRIVATE crd-memory crd-net)
    add_test(NAME crd-net-hpack-interop-corpus
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/hpack_interop.py"
                     --replay "$<TARGET_FILE:crd-hpack-replay>" --corpus "${hpack_test_case_SOURCE_DIR}")
    set_tests_properties(crd-net-hpack-interop-corpus PROPERTIES TIMEOUT 900 LABELS "net;hpack-interop")
endif()

# NET.3's serializer budget board against its pinned peers (CRD_BUILD_SERIALIZE_VS_PEERS, the root CMakeLists).
if(CRD_BUILD_SERIALIZE_VS_PEERS)
    add_subdirectory(board)
endif()
