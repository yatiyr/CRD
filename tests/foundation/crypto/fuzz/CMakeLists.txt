# The crypto fuzz targets (NET-STD, NET.2). Their committed corpora are replayed on every lane and linux-clang-fuzz
# builds the libFuzzer executables from the same sources. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-crypto-capability decode_capability and decode_revocations against restated layouts, and generated lives
#                              (a hybrid-signed root, delegations narrower or widened, a revocation generation, an
#                              audience, a time and a presentation) against a restated containment rule and a model of
#                              the verification order, with any flipped byte of a verified token refused.
#   crd-fuzz-crypto-policy     decode_policy against a restated layout, and generated generation sequences signed by
#                              the test signer through PolicyStore::apply against the contract's adoption and window
#                              rules.
#   crd-fuzz-crypto-provider   the provider interface on hostile input: AEAD openings that must re-seal to their input,
#                              verdicts and refusals by length, key agreement with arbitrary peers, HKDF's prefix
#                              property and sign-then-verify from arbitrary seeds.
#   crd-fuzz-crypto-quic       QUIC packet protection on hostile packets, Retries, packet numbers and address-validation
#                              tokens: header protection refuses exactly the unsampleable packets and round-trips, an
#                              opened packet re-seals to its input, a Retry verifies exactly when its tag is the
#                              computed one, packet numbers decode to the closest candidate, and a token's verdict is
#                              the one its restated format, binding and lifetime give.
#   crd-fuzz-crypto-tls        TLS 1.3 sessions and QUIC handshakes fed hostile bytes (records, or CRYPTO data per
#                              level) as a server and as a client: never established, only the documented answers,
#                              failure is final, every buffer stays bounded and no client Application secret is
#                              reached; and
#                              compressed Certificates decoded only to reachable claims within the bound.
crd_fuzz_target(crd-fuzz-crypto-capability
    SOURCES fuzz_capability.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-capability")
crd_fuzz_target(crd-fuzz-crypto-policy
    SOURCES fuzz_policy.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
crd_fuzz_target(crd-fuzz-crypto-provider
    SOURCES fuzz_provider.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-provider")
crd_fuzz_target(crd-fuzz-crypto-tls
    SOURCES fuzz_tls.cpp LIBRARIES crd-crypto crd-compression CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-tls")
crd_fuzz_target(crd-fuzz-crypto-quic
    SOURCES fuzz_quic.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-quic")
