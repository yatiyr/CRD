# crd-net -- the network foundation (NET.1 decision D1: the one owner of every path that can reach another machine).
#
# Ships the version and extension negotiation every Cerid wire protocol carries (NET-STD): offers, answers, GREASE
# in every build, ignore-unknown, downgrade detection and the transcript bytes the handshake binds, with the
# identifiers generated from the standards register (register_ids.hpp, scripts/gen-register-ids.py), and the generated
# wire formats (NET.3) with their frames, admission stages S6 and S7, quotas, reassembly and baseline deltas, and
# 4.2a's transport (addresses, UDP sockets, QUIC headers, the I/O loop and admission stages S1 to S3, QUIC frames and
# transport parameters, QUIC packets per encryption level, acknowledgements and loss recovery, byte streams and flow
# control, the connection with its streams and datagrams, the server behind the ingress and the client's host, byte-stream sockets and QMux over TLS with its host behind a poller, HTTP/3 with its QPACK field coding, and WebTransport sessions).
# Contract: docs/design/network-foundation.md.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-net STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-net
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    # A header-only read of hesap-stats, which links nothing here (the Philox precedent, ADR-0096 section 1): its
    # Philox stream, BBR's randomised probe timing. crd-hesap-net's headers come with its link below.
    PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/numerics/hesap-stats/include"
)

target_link_libraries(crd-net
    PUBLIC
        crd-core
        crd-containers
        crd-crypto # S7 asks S18's verdict (authorize_operation)
        crd-memory # reassembly buffers come from an injected allocator
    PRIVATE
        crd-compression # coded bodies: coding.hpp's models and coder
        crd-hesap-net # the congestion controllers' estimators: delivery rate, filters, delay trend, acknowledged rate
        crd-time # HostClock, the I/O loop's clock in a process (io_loop.cpp)
)

# Winsock for the UDP sockets (udp.cpp).
if(WIN32)
    target_link_libraries(crd-net PRIVATE ws2_32)
endif()

target_compile_features(crd-net PUBLIC cxx_std_20)
crd_enable_default_pch(crd-net)

# Cerid::net of the relocatable package.
crd_package_library(crd-net NAME net INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# What a schema with quantised fields adds (network foundation S26): hesap-net's formats, whose headers crd-net's public
# headers never include and the package does not install. A target that holds such a schema links this.
add_library(crd-net-quantized INTERFACE)
target_include_directories(crd-net-quantized INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/quantized>")
target_link_libraries(crd-net-quantized INTERFACE crd-net crd-hesap-net)
