# The CKIR ingestion fuzz targets (REPO.DEV.9): the .ckir text loader (ckir_read) and the graph blob loader
# (deserialize_graph), each replayed over its committed corpus on every lane and fuzzed with libFuzzer under
# CRD_ENABLE_FUZZER. Contract: docs/design/test-instruments.md; harness: tests/support/fuzz.
crd_fuzz_target(crd-fuzz-ckir-text
    SOURCES fuzz_ckir_text.cpp LIBRARIES crd-kir CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/text")
crd_fuzz_target(crd-fuzz-ckir-binary
    SOURCES fuzz_ckir_binary.cpp LIBRARIES crd-kir CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/binary")
