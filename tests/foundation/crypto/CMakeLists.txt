# crd-crypto tests: the algorithm identifiers generated from the standards register and the signed crypto policy
# generations (NET-STD), the provider interface against Project Wycheproof's vectors, the TLS 1.3 sessions, QUIC packet
# protection against RFC 9001's and RFC 9369's sample packets, the TLS handshake over QUIC, QUIC's address-validation
# tokens, Encrypted Client Hello, the TLS exporter, capabilities, message schemas, anti-replay and early data, HPKE
# against RFC 9180's vectors and MLS (its layers through the key schedule, the secret tree, message framing and
# protection with the transcript hashes, the ratchet tree with TreeKEM, and groups: validation against a group, the
# GroupInfo and the Welcome) against the MLS working group's (NET.2).
add_executable(crd-crypto-tests
    test_capability.cpp
    test_hpke.cpp
    test_kem_prf.cpp
    test_mls.cpp
    test_mls_group.cpp
    test_mls_message.cpp
    test_mls_schedule.cpp
    test_mls_tree.cpp
    test_policy.cpp
    test_provider.cpp
    test_quic.cpp
    test_quic_handshake.cpp
    test_quic_token.cpp
    test_register_ids.cpp
    test_replay.cpp
    test_tls.cpp
    test_tls_early.cpp
    test_tls_ech.cpp
)

target_link_libraries(crd-crypto-tests PRIVATE
    crd-asset-io
    crd-compression
    crd-core
    crd-containers
    crd-crypto
    crd-memory
    crd-platform
    Catch2::Catch2WithMain
    # The provider itself, for the ML-KEM encapsulation vectors only: their known answers fix the randomness m,
    # which only the provider's deterministic encapsulation takes (test_kem_prf.cpp); its TLS stack, to read back the
    # lifetime of a ticket a client keeps and to run a server with a hostile certificate compressor (test_tls.cpp), and
    # to ask whether the CPU has AES instructions, which the HPKE AEAD a client picks for ECH follows
    # (test_tls_ech.cpp), and to age a client's ticket at rest for the provider's freshness check on early data
    # (test_tls_early.cpp).
    crypto
    ssl
)

# The pinned vectors are read from the package cache at run time (cmake/pins.json): Wycheproof's, the MLS working
# group's (mls-vectors) and RFC 9180's, which the pinned AWS-LC carries as crypto/hpke/test-vectors.json.
target_compile_definitions(crd-crypto-tests PRIVATE
    CRD_WYCHEPROOF_DIR="${wycheproof_SOURCE_DIR}"
    CRD_MLS_VECTORS_DIR="${mls-vectors_SOURCE_DIR}/test-vectors"
    CRD_HPKE_VECTORS="${aws-lc_SOURCE_DIR}/crypto/hpke/test-vectors.json")

crd_enable_default_pch(crd-crypto-tests)
crd_discover_tests(crd-crypto-tests)

# The register identifiers header is scripts/gen-register-ids.py's output for cmake/standards-register.json; the
# check compares every owner's header, so crd-net-register-ids-drift and this test run the same command.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME crd-crypto-register-ids-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-register-ids.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-crypto-register-ids-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()

# The policy decoder and store, the provider and the TLS sessions under the bounded fuzz harness, with their committed
# corpora.
add_subdirectory(fuzz)

# The TLS 1.3 and ECH interoperability test against the pinned OpenSSL 4.0 (CRD_BUILD_TLS_VS_OPENSSL; Linux: locally,
# and on the complete-tier linux-gcc-public-checks lane).
# OpenSSL builds by its own Configure in a separate project, static, libraries only, with zstd (the pinned one the
# engine uses) and zlib (the system's) for certificate compression (RFC 8879); crd-openssl-tls-peer links nothing else,
# and the two peers meet only through tls_interop.py, which relays their records between two processes, or their CRYPTO
# data per encryption level for the handshake over QUIC (OpenSSL's QUIC TLS interface against QuicHandshake).
if(CRD_BUILD_TLS_VS_OPENSSL)
    include(ExternalProject)
    find_package(Perl REQUIRED)
    find_program(CRD_MAKE_PROGRAM NAMES make gmake REQUIRED)
    find_package(Threads REQUIRED)
    find_package(ZLIB REQUIRED)
    cmake_host_system_information(RESULT openssl_jobs QUERY NUMBER_OF_LOGICAL_CORES)
    crd_pin(openssl_version packages openssl version)
    # The prefix names the version: a new pin builds afresh instead of keeping the stamps of the last one.
    set(openssl_prefix "${CMAKE_BINARY_DIR}/_deps/openssl-peer-${openssl_version}")
    set(openssl_libraries "${openssl_prefix}/install/lib/libssl.a" "${openssl_prefix}/install/lib/libcrypto.a")
    ExternalProject_Add(crd-openssl-peer-build
        SOURCE_DIR "${openssl_SOURCE_DIR}"
        PREFIX "${openssl_prefix}"
        BINARY_DIR "${openssl_prefix}/build"
        INSTALL_DIR "${openssl_prefix}/install"
        CONFIGURE_COMMAND "${PERL_EXECUTABLE}" "<SOURCE_DIR>/Configure" "CC=${CMAKE_C_COMPILER}" --prefix=<INSTALL_DIR>
                          --libdir=lib no-shared no-tests no-docs no-apps enable-zstd zlib
                          "--with-zstd-include=${zstd_SOURCE_DIR}/lib" "--with-zlib-include=${ZLIB_INCLUDE_DIRS}"
        BUILD_COMMAND "${CRD_MAKE_PROGRAM}" -j${openssl_jobs} build_libs
        INSTALL_COMMAND "${CRD_MAKE_PROGRAM}" install_dev
        BUILD_BYPRODUCTS ${openssl_libraries})
    add_executable(crd-openssl-tls-peer interop/openssl_tls_peer.c)
    add_dependencies(crd-openssl-tls-peer crd-openssl-peer-build)
    target_include_directories(crd-openssl-tls-peer SYSTEM PRIVATE "${openssl_prefix}/install/include")
    target_link_libraries(crd-openssl-tls-peer PRIVATE ${openssl_libraries} libzstd_static ZLIB::ZLIB Threads::Threads
                                                       ${CMAKE_DL_LIBS})
    set_target_properties(crd-openssl-tls-peer PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

    add_executable(crd-tls-peer interop/cerid_tls_peer.cpp)
    target_link_libraries(crd-tls-peer PRIVATE crd-crypto crd-core crd-containers)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME crd-crypto-tls-interop-openssl
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/tls_interop.py"
                     --cerid "$<TARGET_FILE:crd-tls-peer>" --openssl "$<TARGET_FILE:crd-openssl-tls-peer>")
    set_tests_properties(crd-crypto-tls-interop-openssl PROPERTIES TIMEOUT 600 LABELS "crypto;tls-interop")
    message(STATUS "[tls-vs-openssl] TLS 1.3 interoperability against OpenSSL ${openssl_version}")
endif()
