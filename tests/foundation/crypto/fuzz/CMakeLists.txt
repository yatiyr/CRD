# The policy generation fuzz target (NET-STD). Its committed corpus is replayed on every lane and linux-clang-fuzz
# builds the libFuzzer executable from the same source. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-crypto-policy   decode_policy against a restated layout, and generated generation sequences signed by
#                            the test signer through PolicyStore::apply against the contract's adoption and window
#                            rules.
crd_fuzz_target(crd-fuzz-crypto-policy
    SOURCES fuzz_policy.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
