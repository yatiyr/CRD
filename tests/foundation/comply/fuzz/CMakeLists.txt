# The rule generation fuzz target (COMPLY.0). Its committed corpus is replayed on every lane and linux-clang-fuzz builds
# the libFuzzer executable from the same source. Contract: docs/design/test-instruments.md; harness: tests/support/fuzz.
#   crd-fuzz-comply-rules   decode_rules with the contract's invariants and a canonical reprint, and generated
#                           generation sequences applied to one RuleStore against a model of monotonic adoption and a
#                           brute-force query.
crd_fuzz_target(crd-fuzz-comply-rules
    SOURCES fuzz_rules.cpp LIBRARIES crd-comply CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
