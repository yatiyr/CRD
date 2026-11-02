# crd-crypto tests: the algorithm identifiers generated from the standards register and the signed crypto policy
# generations (NET-STD), the provider interface against Project Wycheproof's vectors, the TLS 1.3 sessions, QUIC packet
# protection against RFC 9001's and RFC 9369's sample packets, the TLS handshake over QUIC, QUIC's address-validation
# tokens, Encrypted Client Hello, the TLS exporter, capabilities, message schemas, anti-replay and early data, HPKE
# against RFC 9180's vectors and MLS (its layers through the key schedule, the secret tree, message framing and
# protection with the transcript hashes, the ratchet tree with TreeKEM, groups: validation against a group, the
# GroupInfo and the Welcome, and proposals and commits with the passive clients) against the MLS working group's, and
# the key store with its DPAPI protector (NET.2).
add_executable(crd-crypto-tests
    test_capability.cpp
    test_hpke.cpp
    test_kem_prf.cpp
    test_key_store.cpp
    test_mls.cpp
    test_mls_commit.cpp
    test_mls_external.cpp
    test_mls_group.cpp
    test_mls_message.cpp
    test_mls_resume.cpp
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
# group's (mls-vectors), RFC 9180's, which the pinned AWS-LC carries as crypto/hpke/test-vectors.json, and the HPKE
# working group's post-quantum ones (hpke-pq-vectors).
target_compile_definitions(crd-crypto-tests PRIVATE
    CRD_WYCHEPROOF_DIR="${wycheproof_SOURCE_DIR}"
    CRD_MLS_VECTORS_DIR="${mls-vectors_SOURCE_DIR}/test-vectors"
    CRD_HPKE_VECTORS="${aws-lc_SOURCE_DIR}/crypto/hpke/test-vectors.json"
    CRD_HPKE_PQ_VECTORS="${hpke-pq-vectors_SOURCE_DIR}/test-vectors.json")

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

# The interoperability peers' OpenSSL 4.0 (CRD_BUILD_TLS_VS_OPENSSL or CRD_BUILD_MLS_VS_MLSPP; Linux: locally, and on
# the complete-tier linux-gcc-public-checks lane). OpenSSL builds by its own Configure in a separate project, static,
# libraries only, with zstd (the pinned one the engine uses) and zlib (the system's) for certificate compression (RFC
# 8879), so every program linking it links those too. The peers of each test meet only through its Python driver, each
# in its own process (AWS-LC and OpenSSL share symbol names).
if(CRD_BUILD_TLS_VS_OPENSSL OR CRD_BUILD_MLS_VS_MLSPP)
    include(ExternalProject)
    find_package(Perl REQUIRED)
    find_program(CRD_MAKE_PROGRAM NAMES make gmake REQUIRED)
    find_package(Threads REQUIRED)
    find_package(ZLIB REQUIRED)
    cmake_host_system_information(RESULT interop_jobs QUERY NUMBER_OF_LOGICAL_CORES)
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
        BUILD_COMMAND "${CRD_MAKE_PROGRAM}" -j${interop_jobs} build_libs
        INSTALL_COMMAND "${CRD_MAKE_PROGRAM}" install_dev
        BUILD_BYPRODUCTS ${openssl_libraries})
    set(openssl_link ${openssl_libraries} libzstd_static ZLIB::ZLIB Threads::Threads ${CMAKE_DL_LIBS})
endif()

# The TLS 1.3 and ECH interoperability test against OpenSSL (CRD_BUILD_TLS_VS_OPENSSL): crd-openssl-tls-peer links
# OpenSSL alone, and tls_interop.py relays the two peers' records between their processes, or their CRYPTO data per
# encryption level for the handshake over QUIC (OpenSSL's QUIC TLS interface against QuicHandshake).
if(CRD_BUILD_TLS_VS_OPENSSL OR CRD_BUILD_MLS_VS_MLSPP OR CRD_BUILD_MLS_VS_OPENMLS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
if(CRD_BUILD_TLS_VS_OPENSSL)
    add_executable(crd-openssl-tls-peer interop/openssl_tls_peer.c)
    add_dependencies(crd-openssl-tls-peer crd-openssl-peer-build)
    target_include_directories(crd-openssl-tls-peer SYSTEM PRIVATE "${openssl_prefix}/install/include")
    target_link_libraries(crd-openssl-tls-peer PRIVATE ${openssl_link})
    set_target_properties(crd-openssl-tls-peer PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

    add_executable(crd-tls-peer interop/cerid_tls_peer.cpp)
    target_link_libraries(crd-tls-peer PRIVATE crd-crypto crd-core crd-containers)

    add_test(NAME crd-crypto-tls-interop-openssl
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/tls_interop.py"
                     --cerid "$<TARGET_FILE:crd-tls-peer>" --openssl "$<TARGET_FILE:crd-openssl-tls-peer>")
    set_tests_properties(crd-crypto-tls-interop-openssl PROPERTIES TIMEOUT 600 LABELS "crypto;tls-interop")
    message(STATUS "[tls-vs-openssl] TLS 1.3 interoperability against OpenSSL ${openssl_version}")
endif()

# The MLS interoperability tests (decision M4): crd-mls-peer's members over crd-crypto and a peer library's create,
# join, propose, commit and protect in the same groups, relayed by mls_interop.py.
if(CRD_BUILD_MLS_VS_MLSPP OR CRD_BUILD_MLS_VS_OPENMLS)
    add_executable(crd-mls-peer interop/cerid_mls_peer.cpp)
    target_link_libraries(crd-mls-peer PRIVATE crd-crypto crd-core crd-containers)
endif()

# Against MLSpp (CRD_BUILD_MLS_VS_MLSPP): crd-mlspp-peer's members over MLSpp. nlohmann_json installs its CMake package
# for MLSpp's configure; MLSpp builds by its own CMake against the OpenSSL above (4.0, so with its post-quantum
# suites), its library and the three it links (bytes, tls_syntax, hpke) only, without its tests. Its headers need the
# definitions its hpke library publishes for that OpenSSL.
if(CRD_BUILD_MLS_VS_MLSPP)
    crd_pin(mlspp_commit packages mlspp commit)
    string(SUBSTRING "${mlspp_commit}" 0 8 mlspp_short)
    set(json_prefix "${CMAKE_BINARY_DIR}/_deps/nlohmann-json-peer")
    ExternalProject_Add(crd-nlohmann-json-build
        SOURCE_DIR "${nlohmann_json_SOURCE_DIR}"
        PREFIX "${json_prefix}"
        BINARY_DIR "${json_prefix}/build"
        INSTALL_DIR "${json_prefix}/install"
        CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>" -DJSON_BuildTests=OFF -DJSON_Install=ON)
    set(mlspp_prefix "${CMAKE_BINARY_DIR}/_deps/mlspp-peer-${mlspp_short}")
    set(mlspp_build "${mlspp_prefix}/build")
    set(mlspp_libraries "${mlspp_build}/libmlspp.a" "${mlspp_build}/lib/hpke/libhpke.a"
                        "${mlspp_build}/lib/tls_syntax/libtls_syntax.a" "${mlspp_build}/lib/bytes/libbytes.a")
    ExternalProject_Add(crd-mlspp-build
        SOURCE_DIR "${mlspp_SOURCE_DIR}"
        PREFIX "${mlspp_prefix}"
        BINARY_DIR "${mlspp_build}"
        CMAKE_ARGS "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}" -DCMAKE_BUILD_TYPE=Release -DTESTING=OFF
                   "-DCMAKE_PREFIX_PATH=${json_prefix}/install" "-DOPENSSL_ROOT_DIR=${openssl_prefix}/install"
                   -DOPENSSL_USE_STATIC_LIBS=ON
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target mlspp --parallel ${interop_jobs}
        INSTALL_COMMAND ""
        DEPENDS crd-openssl-peer-build crd-nlohmann-json-build
        BUILD_BYPRODUCTS ${mlspp_libraries})
    add_executable(crd-mlspp-peer interop/mlspp_mls_peer.cpp)
    add_dependencies(crd-mlspp-peer crd-mlspp-build)
    target_include_directories(crd-mlspp-peer SYSTEM PRIVATE
                               "${mlspp_SOURCE_DIR}/include" "${mlspp_build}/include"
                               "${mlspp_SOURCE_DIR}/lib/bytes/include" "${mlspp_SOURCE_DIR}/lib/tls_syntax/include"
                               "${mlspp_SOURCE_DIR}/lib/hpke/include" "${mlspp_SOURCE_DIR}/third_party"
                               "${openssl_prefix}/install/include")
    target_compile_definitions(crd-mlspp-peer PRIVATE WITH_OPENSSL3 WITH_PQ)
    target_link_libraries(crd-mlspp-peer PRIVATE ${mlspp_libraries} ${openssl_link})
    set_target_properties(crd-mlspp-peer PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

    add_test(NAME crd-crypto-mls-interop-mlspp
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/mls_interop.py"
                     --cerid "$<TARGET_FILE:crd-mls-peer>" --mlspp "$<TARGET_FILE:crd-mlspp-peer>")
    set_tests_properties(crd-crypto-mls-interop-mlspp PROPERTIES TIMEOUT 600 LABELS "crypto;mls-interop")
    message(STATUS "[mls-vs-mlspp] MLS interoperability against MLSpp ${mlspp_short}")
endif()

# Against the pinned gnome-keyring (CRD_BUILD_SECRET_SERVICE_VS_GNOME_KEYRING): crd-crypto-tests' hidden Secret Service
# cases in a session bus and scratch home of their own (secret_service_session.py), with the login collection
# unlocked, then seeded and locked.
if(CRD_BUILD_SECRET_SERVICE_VS_GNOME_KEYRING)
    foreach(mode unlocked locked)
        add_test(NAME crd-crypto-secret-service-${mode}
                 COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/secret_service_session.py"
                         --daemon "${CRD_GNOME_KEYRING}" --mode ${mode} -- "$<TARGET_FILE:crd-crypto-tests>")
        set_tests_properties(crd-crypto-secret-service-${mode} PROPERTIES TIMEOUT 600 LABELS "crypto;secret-service")
    endforeach()
    message(STATUS "[secret-service-vs-gnome-keyring] the Secret Service protector against ${CRD_GNOME_KEYRING}")
endif()

# Against OpenMLS (CRD_BUILD_MLS_VS_OPENMLS): crd-openmls-peer's members over OpenMLS, a Cargo package linted by
# Clippy and built with the pinned Rust toolchain (CRD_CARGO) by `cargo build --locked`, so every crate is the one
# Cargo.lock names, verified by its SHA-256; CARGO_HOME (the crate downloads) and the target directory stay inside this
# tree, and the peer's own warnings, the compiler's and Clippy's, fail the build. Cargo decides what to rebuild, so the
# target always runs it.
if(CRD_BUILD_MLS_VS_OPENMLS)
    set(openmls_manifest "${CMAKE_CURRENT_SOURCE_DIR}/interop/openmls_peer/Cargo.toml")
    set(openmls_target "${CMAKE_BINARY_DIR}/_deps/openmls-peer")
    set(openmls_peer "${openmls_target}/release/crd-openmls-peer")
    get_filename_component(rust_bin "${CRD_CARGO}" DIRECTORY)
    set(cargo_env "${CMAKE_COMMAND}" -E env "CARGO_HOME=${CMAKE_BINARY_DIR}/_deps/cargo-home" "CARGO=${CRD_CARGO}"
                  "RUSTC=${rust_bin}/rustc" "RUSTFLAGS=-D warnings")
    set(cargo_args --locked --release --manifest-path "${openmls_manifest}" --target-dir "${openmls_target}")
    add_custom_target(crd-openmls-peer ALL
        COMMAND ${cargo_env} "${rust_bin}/cargo-clippy" clippy ${cargo_args} -- -D warnings
        COMMAND ${cargo_env} "${CRD_CARGO}" build ${cargo_args}
        BYPRODUCTS "${openmls_peer}"
        COMMENT "[mls-vs-openmls] cargo clippy and cargo build --locked: crd-openmls-peer"
        USES_TERMINAL
        VERBATIM)
    add_test(NAME crd-crypto-mls-interop-openmls
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/interop/mls_interop.py"
                     --cerid "$<TARGET_FILE:crd-mls-peer>" --openmls "${openmls_peer}")
    set_tests_properties(crd-crypto-mls-interop-openmls PROPERTIES TIMEOUT 600 LABELS "crypto;mls-interop")
    message(STATUS "[mls-vs-openmls] MLS interoperability against OpenMLS (Cargo.lock), Rust from ${rust_bin}")
endif()
