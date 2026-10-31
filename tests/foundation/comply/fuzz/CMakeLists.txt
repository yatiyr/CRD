# The comply fuzz targets (COMPLY.0 and COMPLY.1). Their committed corpora are replayed on every lane and
# linux-clang-fuzz builds the libFuzzer executables from the same sources. Contract: docs/design/test-instruments.md;
# harness: tests/support/fuzz.
#   crd-fuzz-comply-rules       decode_rules with the contract's invariants and a canonical reprint, and generated
#                               generation sequences applied to one RuleStore against a model of monotonic adoption
#                               and a brute-force query.
#   crd-fuzz-comply-sanctions   decode_sanctions with the contract's invariants, the name matcher's folding and score
#                               bounds, and screening requests against a model of gaps, hits, dispositions,
#                               destinations and the verdict's precedence.
crd_fuzz_target(crd-fuzz-comply-rules
    SOURCES fuzz_rules.cpp LIBRARIES crd-comply CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/rules")
crd_fuzz_target(crd-fuzz-comply-sanctions
    SOURCES fuzz_sanctions.cpp LIBRARIES crd-comply crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/sanctions")
