# crd-crypto tests: the algorithm identifiers generated from the standards register and the signed crypto policy
# generations (NET-STD), the provider interface against Project Wycheproof's vectors, the TLS 1.3 sessions and QUIC packet
# protection against RFC 9001's and RFC 9369's sample packets (NET.2).
add_executable(crd-crypto-tests
    test_kem_prf.cpp
    test_policy.cpp
    test_provider.cpp
    test_quic.cpp
    test_register_ids.cpp
    test_tls.cpp
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
    # lifetime of a ticket a client keeps and to run a server with a hostile certificate compressor (test_tls.cpp).
    crypto
    ssl
)

# The pinned Wycheproof vectors are read from the package cache at run time (cmake/pins.json: wycheproof).
target_compile_definitions(crd-crypto-tests PRIVATE CRD_WYCHEPROOF_DIR="${wycheproof_SOURCE_DIR}")

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

# The TLS 1.3 interoperability test against the pinned OpenSSL 3.5 (CRD_BUILD_TLS_VS_OPENSSL; Linux, local only).
# OpenSSL builds by its own Configure in a separate project, static, libraries only, with zstd (the pinned one the
# engine uses) and zlib (the system's) for certificate compression (RFC 8879); crd-openssl-tls-peer links nothing else,
# and the two peers meet only through tls_interop.py, which relays their records between two processes.
if(CRD_BUILD_TLS_VS_OPENSSL)
    include(ExternalProject)
    find_package(Perl REQUIRED)
    find_program(CRD_MAKE_PROGRAM NAMES make gmake REQUIRED)
    find_package(Threads REQUIRED)
    find_package(ZLIB REQUIRED)
    cmake_host_system_information(RESULT openssl_jobs QUERY NUMBER_OF_LOGICAL_CORES)
    crd_pin(openssl_version packages openssl version)
    set(openssl_prefix "${CMAKE_BINARY_DIR}/_deps/openssl-peer")
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
