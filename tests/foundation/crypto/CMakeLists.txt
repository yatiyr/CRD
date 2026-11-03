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
    test_mls_policy.cpp
    test_mls_resume.cpp
    test_mls_schedule.cpp
    test_mls_tree.cpp
    test_policy.cpp
    test_policy_layers.cpp
    test_provider.cpp
    test_quic.cpp
    test_quic_handshake.cpp
    test_quic_token.cpp
    test_register_ids.cpp
    test_replay.cpp
    test_tls.cpp
    test_tls_early.cpp
    test_tls_ech.cpp
    test_tls_policy.cpp
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
# working group's post-quantum ones (hpke-pq-vectors); on Android from their device mirrors, which the launcher pushes.
crd_test_device_path(_crd_wycheproof "${wycheproof_SOURCE_DIR}")
crd_test_device_path(_crd_mls_vectors "${mls-vectors_SOURCE_DIR}/test-vectors")
crd_test_device_path(_crd_hpke_vectors "${aws-lc_SOURCE_DIR}/crypto/hpke/test-vectors.json")
crd_test_device_path(_crd_hpke_pq_vectors "${hpke-pq-vectors_SOURCE_DIR}/test-vectors.json")
target_compile_definitions(crd-crypto-tests PRIVATE
    CRD_WYCHEPROOF_DIR="${_crd_wycheproof}"
    CRD_MLS_VECTORS_DIR="${_crd_mls_vectors}"
    CRD_HPKE_VECTORS="${_crd_hpke_vectors}"
    CRD_HPKE_PQ_VECTORS="${_crd_hpke_pq_vectors}")
crd_test_data(crd-crypto-tests "${wycheproof_SOURCE_DIR}/testvectors_v1" "${mls-vectors_SOURCE_DIR}/test-vectors"
    "${aws-lc_SOURCE_DIR}/crypto/hpke/test-vectors.json" "${hpke-pq-vectors_SOURCE_DIR}/test-vectors.json")

# What tls_key_log_compiled() must answer (network foundation S19): derived here from the shipping condition, apart
# from crd-crypto's own definition, so a shipping lane fails the moment key logging reaches a shipping build.
target_compile_definitions(crd-crypto-tests PRIVATE "CRD_TEST_EXPECT_TLS_KEY_LOG=$<IF:${_crd_shipping},0,1>")

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
    # The bootstrap headers are scripts/gen-policy-bootstrap.py's output for assets/crypto-policy (the build's) and
    # tests/foundation/crypto/data/policy-bootstrap (the populated shape test_policy.cpp verifies).
    add_test(NAME crd-crypto-policy-bootstrap-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-policy-bootstrap.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-crypto-policy-bootstrap-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
    # Every symbol the built AWS-LC libraries define carries the provider prefix (network foundation decision P5).
    add_test(NAME crd-check-provider-symbols
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/provider-prefix.py" check
                     --symbols "${CMAKE_SOURCE_DIR}/cmake/aws-lc-symbols.txt" --prefix ${CRD_PROVIDER_PREFIX}
                     "$<TARGET_FILE:crypto>" "$<TARGET_FILE:ssl>")
    set_tests_properties(crd-check-provider-symbols PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()

# Protocol state learning (docs/design/network-foundation.md#state-learning): each end's TlsSession learned through
# crd-crypto-tls-sul, held to its reviewed machine and to the models' properties. Runs where the program runs on the
# build host.
add_executable(crd-crypto-tls-sul state/tls_sul.cpp)
target_link_libraries(crd-crypto-tls-sul PRIVATE crd-crypto crd-core crd-containers)
if(Python3_FOUND AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME crd-crypto-state-learning
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-state-machine.py"
                     --sul "$<TARGET_FILE:crd-crypto-tls-sul>")
    set_tests_properties(crd-crypto-state-learning PROPERTIES TIMEOUT 1200 LABELS "crypto;state-learning"
                         ENVIRONMENT "PYTHONUTF8=1")
endif()

# The policy decoder and store, the provider and the TLS sessions under the bounded fuzz harness, with their committed
# corpora.
add_subdirectory(fuzz)

# The interoperability and board peers' OpenSSL 4.0 (CRD_BUILD_TLS_VS_OPENSSL, CRD_BUILD_MLS_VS_MLSPP or
# CRD_BUILD_CRYPTO_VS_PEERS; Linux: locally, and on the complete-tier linux-gcc-public-checks lane). OpenSSL builds by
# its own Configure in a separate project, static, libraries only, with zstd (the pinned one the engine uses) and zlib
# (the system's) for certificate compression (RFC 8879), so every program linking it links those too. The peers of each
# interoperability test meet only through its Python driver, each in its own process; the coexistence test and the
# boards link OpenSSL beside crd-crypto's prefixed AWS-LC (decision P5).
if(CRD_BUILD_TLS_VS_OPENSSL OR CRD_BUILD_MLS_VS_MLSPP OR CRD_BUILD_CRYPTO_VS_PEERS)
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

    # Decision P5: crd-crypto over its prefixed AWS-LC and OpenSSL in one program. Only openssl_coexist.c sees
    # OpenSSL's headers (an object library of its own); the program also runs crd-crypto's TLS and QUIC handshake
    # tests, so both TLS stacks work side by side.
    add_library(crd-openssl-coexist OBJECT interop/openssl_coexist.c)
    add_dependencies(crd-openssl-coexist crd-openssl-peer-build)
    target_include_directories(crd-openssl-coexist SYSTEM PRIVATE "${openssl_prefix}/install/include")
    add_executable(crd-crypto-coexist-tests interop/test_openssl_coexist.cpp test_tls.cpp test_tls_early.cpp
                                            test_tls_ech.cpp test_quic_handshake.cpp)
    target_link_libraries(crd-crypto-coexist-tests PRIVATE crd-openssl-coexist crd-asset-io crd-compression crd-core
                          crd-containers crd-crypto crd-memory crd-platform Catch2::Catch2WithMain crypto ssl
                          ${openssl_link})
    target_compile_definitions(crd-crypto-coexist-tests PRIVATE
                               "CRD_TEST_EXPECT_TLS_KEY_LOG=$<IF:${_crd_shipping},0,1>")
    set_target_properties(crd-crypto-coexist-tests PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    add_test(NAME crd-crypto-openssl-coexist COMMAND crd-crypto-coexist-tests)
    set_tests_properties(crd-crypto-openssl-coexist PROPERTIES TIMEOUT 600 LABELS "crypto;tls-interop")
endif()

# crd-crypto's boards (CRD_BUILD_CRYPTO_VS_PEERS, docs/bench): crd-crypto-board times crd-crypto and three peer arms in
# one process. board/board_peer.c is compiled once per peer library against its own headers: AWS-LC's (the provider's
# own targets, called directly), BoringSSL's and OpenSSL's. BoringSSL builds by its own CMake as a separate project from
# its pregenerated sources, static, crypto and ssl only, with every symbol prefixed crd_bssl (BORINGSSL_PREFIX and its
# checked-in prefix_symbols.h), so the three libraries of one family and OpenSSL bind each call to their own code. The
# peer arms are Cerid's own C: they meet the engine's warning set (crd-warnings) beside the peers' headers.
if(CRD_BUILD_CRYPTO_VS_PEERS)
    crd_pin(boringssl_version packages boringssl version)
    crd_pin(aws_lc_version packages aws-lc version)
    set(boringssl_prefix "${CMAKE_BINARY_DIR}/_deps/boringssl-peer-${boringssl_version}")
    set(boringssl_libraries "${boringssl_prefix}/build/libssl.a" "${boringssl_prefix}/build/libcrypto.a")
    ExternalProject_Add(crd-boringssl-peer-build
        SOURCE_DIR "${boringssl_SOURCE_DIR}"
        PREFIX "${boringssl_prefix}"
        BINARY_DIR "${boringssl_prefix}/build"
        CMAKE_ARGS "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                   -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBORINGSSL_PREFIX=crd_bssl
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target crypto ssl --parallel ${interop_jobs}
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS ${boringssl_libraries})

    add_library(crd-board-peer-awslc OBJECT board/board_peer.c)
    target_link_libraries(crd-board-peer-awslc PRIVATE crd-warnings crypto ssl)
    target_compile_definitions(crd-board-peer-awslc PRIVATE CRD_BOARD_PEER=crd_board_peer_awslc
                               "CRD_BOARD_PEER_NAME=\"aws-lc\"" "CRD_BOARD_PEER_VERSION=\"${aws_lc_version}\"")
    add_library(crd-board-peer-boringssl OBJECT board/board_peer.c)
    target_link_libraries(crd-board-peer-boringssl PRIVATE crd-warnings)
    add_dependencies(crd-board-peer-boringssl crd-boringssl-peer-build)
    target_include_directories(crd-board-peer-boringssl SYSTEM PRIVATE "${boringssl_SOURCE_DIR}/include")
    target_compile_definitions(crd-board-peer-boringssl PRIVATE BORINGSSL_PREFIX=crd_bssl
                               CRD_BOARD_PEER=crd_board_peer_boringssl "CRD_BOARD_PEER_NAME=\"boringssl\""
                               "CRD_BOARD_PEER_VERSION=\"${boringssl_version}\"")
    add_library(crd-board-peer-openssl OBJECT board/board_peer.c)
    target_link_libraries(crd-board-peer-openssl PRIVATE crd-warnings)
    add_dependencies(crd-board-peer-openssl crd-openssl-peer-build)
    target_include_directories(crd-board-peer-openssl SYSTEM PRIVATE "${openssl_prefix}/install/include")
    target_compile_definitions(crd-board-peer-openssl PRIVATE CRD_BOARD_PEER=crd_board_peer_openssl
                               "CRD_BOARD_PEER_NAME=\"openssl\"" "CRD_BOARD_PEER_VERSION=\"${openssl_version}\"")

    add_executable(crd-crypto-board board/crypto_board.cpp)
    target_link_libraries(crd-crypto-board PRIVATE crd-board-peer-awslc crd-board-peer-boringssl
                          crd-board-peer-openssl crd-crypto crd-core crd-containers crypto ssl ${boringssl_libraries}
                          ${openssl_link})
    set_target_properties(crd-crypto-board PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    add_test(NAME crd-crypto-board-smoke COMMAND crd-crypto-board --quick)
    set_tests_properties(crd-crypto-board-smoke PROPERTIES TIMEOUT 600 LABELS "crypto;board")
    message(STATUS "[crypto-vs-peers] the crypto boards against AWS-LC ${aws_lc_version}, BoringSSL "
                   "${boringssl_version} and OpenSSL ${openssl_version}")
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

# Against the pinned swtpm (CRD_BUILD_TPM_VS_SWTPM): crd-crypto-tests' key store cases with the TPM protector and
# device reaching a simulator of their own through tss2 (tpm_simulator_session.py), then a TPM whose owner hierarchy has
# an authorization.
if(CRD_BUILD_TPM_VS_SWTPM)
    add_test(NAME crd-crypto-tpm-swtpm
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tpm_simulator_session.py"
                     --swtpm "${CRD_SWTPM}" -- "$<TARGET_FILE:crd-crypto-tests>")
    set_tests_properties(crd-crypto-tpm-swtpm PROPERTIES TIMEOUT 1200 LABELS "crypto;tpm")
    message(STATUS "[tpm-vs-swtpm] the TPM protector and device against ${CRD_SWTPM}")
endif()

# Against the pinned SoftHSM (CRD_BUILD_PKCS11_VS_SOFTHSM): crd-crypto-tests' key store cases with the PKCS #11 device
# reaching a token of its own (pkcs11_token_session.py), which must be left empty, then a wrong PIN tried once.
if(CRD_BUILD_PKCS11_VS_SOFTHSM)
    add_test(NAME crd-crypto-pkcs11-softhsm
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/pkcs11_token_session.py"
                     --softhsm "${CRD_SOFTHSM}" -- "$<TARGET_FILE:crd-crypto-tests>")
    set_tests_properties(crd-crypto-pkcs11-softhsm PROPERTIES TIMEOUT 1200 LABELS "crypto;pkcs11")
    message(STATUS "[pkcs11-vs-softhsm] the PKCS #11 device against ${CRD_SOFTHSM}")
endif()

# NET.2's protocol model (CRD_BUILD_PROTOCOL_MODEL): every Tamarin model in model/ proven with the pinned Tamarin, each
# lemma held to its expected verdict (scripts/check-protocol-model.py).
if(CRD_BUILD_PROTOCOL_MODEL)
    add_test(NAME crd-crypto-protocol-model
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-protocol-model.py"
                     --tamarin "${CRD_TAMARIN}")
    set_tests_properties(crd-crypto-protocol-model PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE LABELS "crypto;model")
    message(STATUS "[protocol-model] the protocol models with ${CRD_TAMARIN}")
endif()

# The Android Keystore in an application process (CRD_BUILD_ANDROID_KEYSTORE_APP, decision K15): the key store's cases
# as a JNI library (android/keystore_app_main.cpp) inside an instrumentation APK (android/, built and signed by
# scripts/android-apk.py with the pinned kit), whose [android-keystore] case runs in the app's own Java VM, uid and
# SELinux domain on the emulator (scripts/android-device.py instrument).
if(CRD_BUILD_ANDROID_KEYSTORE_APP)
    add_library(crd-crypto-keystore-app SHARED test_key_store.cpp android/keystore_app_main.cpp)
    target_link_libraries(crd-crypto-keystore-app PRIVATE
        crd-containers crd-core crd-crypto crd-memory crd-platform Catch2::Catch2 crypto ssl)
    set(_crd_keystore_apk "${CMAKE_CURRENT_BINARY_DIR}/crd-crypto-keystore-app.apk")
    set(_crd_keystore_java "${CMAKE_CURRENT_SOURCE_DIR}/android/java")
    add_custom_command(OUTPUT "${_crd_keystore_apk}"
        COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/android-apk.py" build --sdk "${CRD_ANDROID_SDK}"
                --manifest "${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidManifest.xml" --java "${_crd_keystore_java}"
                --library "$<TARGET_FILE:crd-crypto-keystore-app>" --abi ${ANDROID_ABI}
                --min-api ${ANDROID_PLATFORM_LEVEL} --strip "${CMAKE_STRIP}"
                --work "${CMAKE_CURRENT_BINARY_DIR}/keystore-app" --output "${_crd_keystore_apk}"
        DEPENDS crd-crypto-keystore-app "${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidManifest.xml"
                "${_crd_keystore_java}/cerid/test/keystore/KeyStoreRunner.java"
                "${CMAKE_SOURCE_DIR}/scripts/android-apk.py" "${CMAKE_SOURCE_DIR}/scripts/install-android-emulator.py"
        COMMENT "Building the Android Keystore's instrumentation APK"
        VERBATIM)
    add_custom_target(crd-crypto-keystore-apk ALL DEPENDS "${_crd_keystore_apk}")
    add_test(NAME crd-crypto-keystore-app
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/android-device.py" instrument
                     --apk "${_crd_keystore_apk}" --package cerid.test.keystore --runner .KeyStoreRunner
                     -- "[android-keystore]")
    set_tests_properties(crd-crypto-keystore-app PROPERTIES TIMEOUT 600 LABELS "crypto;android-keystore")
    message(STATUS "[android-keystore-app] the Android Keystore in an instrumentation APK (${CRD_ANDROID_SDK})")
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
