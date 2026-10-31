# crd-crypto -- the cryptography foundation (NET.1 decision D2: its consumers are wider than the network, so it is a
# foundation owner that depends on no network code).
#
# Ships the algorithm identifiers generated from the NET-STD standards register (register_ids.hpp,
# scripts/gen-register-ids.py), the signature verification interface providers implement and the signed, monotonic
# crypto policy generations (NET-STD). The providers, the operating-system CSPRNG, the key store and the TLS and QUIC
# handshake integration land with NET.2. Contract: docs/design/network-foundation.md#crypto and #agility.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-crypto STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-crypto
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(crd-crypto
    PUBLIC
        crd-core
        crd-containers
)

target_compile_features(crd-crypto PUBLIC cxx_std_20)
crd_enable_default_pch(crd-crypto)

# Cerid::crypto of the relocatable package.
crd_package_library(crd-crypto NAME crypto INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
