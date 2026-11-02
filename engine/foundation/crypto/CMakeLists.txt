# crd-crypto -- the cryptography foundation (NET.1 decision D2: its consumers are wider than the network, so it is a
# foundation owner that depends on no network code).
#
# Ships the algorithm identifiers generated from the NET-STD standards register (register_ids.hpp,
# scripts/gen-register-ids.py), the signature verification interface and the signed, monotonic crypto policy
# generations (NET-STD), and the provider interface over the pinned AWS-LC (provider.hpp, NET.2): randomness, AEAD,
# HKDF, signatures, key agreement, key encapsulation and the table PRF, TLS 1.3 sessions over its TLS stack (tls.hpp),
# whose certificates compress through crd-compression, QUIC packet protection and its handshake (quic.hpp, tls.hpp),
# capabilities and message schemas (capability.hpp), anti-replay windows and filters (replay.hpp), hybrid public key
# encryption (hpke.hpp) and MLS (mls.hpp, its messages in mls_message.hpp and its ratchet tree in mls_tree.hpp). The
# key store lands with a later NET.2 slice. Contract:
# docs/design/network-foundation.md#crypto, #agility, #provider, #tls-sessions, #quic-protection, #capability-tokens,
# #anti-replay, #early-data, #message-schemas, #hpke and #mls.

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
    PRIVATE
        crd-compression # NET.2 decision S10: TLS certificate compression (RFC 8879) through the engine's zstd and zlib
        ssl
        crypto
)

target_compile_features(crd-crypto PUBLIC cxx_std_20)
crd_enable_default_pch(crd-crypto)

# Cerid::crypto of the relocatable package, with AWS-LC's libssl and libcrypto exported link-only beside it
# (Cerid::aws-lc-ssl, Cerid::aws-lc-crypto): their headers are private to this module, so the package installs none and
# a consumer reaches them only through provider.hpp and tls.hpp.
crd_package_library(crd-crypto NAME crypto INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
crd_package_dependency(ssl NAME aws-lc-ssl)
crd_package_dependency(crypto NAME aws-lc-crypto)
