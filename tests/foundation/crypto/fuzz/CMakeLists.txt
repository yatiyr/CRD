# The crypto fuzz targets (NET-STD, NET.2). Their committed corpora are replayed on every lane and linux-clang-fuzz
# builds the libFuzzer executables from the same sources. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-crypto-policy     decode_policy against a restated layout, and generated generation sequences signed by
#                              the test signer through PolicyStore::apply against the contract's adoption and window
#                              rules.
#   crd-fuzz-crypto-provider   the provider interface on hostile input: AEAD openings that must re-seal to their input,
#                              verdicts and refusals by length, key agreement with arbitrary peers, HKDF's prefix
#                              property and sign-then-verify from arbitrary seeds.
#   crd-fuzz-crypto-tls        TLS 1.3 sessions fed hostile bytes as a server and as a client: never established, only
#                              the documented answers, failure is final and every buffer stays bounded.
crd_fuzz_target(crd-fuzz-crypto-policy
    SOURCES fuzz_policy.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
crd_fuzz_target(crd-fuzz-crypto-provider
    SOURCES fuzz_provider.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-provider")
crd_fuzz_target(crd-fuzz-crypto-tls
    SOURCES fuzz_tls.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-tls")
