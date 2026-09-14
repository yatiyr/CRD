# The CEIR ingestion fuzz targets (REPO.DEV.9): the textual loader (parse) and the binary loader (deserialize), each
# replayed over its committed corpus on every lane and fuzzed with libFuzzer under CRD_ENABLE_FUZZER.
# Contract: docs/design/test-instruments.md; harness: tests/support/fuzz.
crd_fuzz_target(crd-fuzz-ceir-text
    SOURCES fuzz_ceir_text.cpp LIBRARIES crd-ceir CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/text")
crd_fuzz_target(crd-fuzz-ceir-binary
    SOURCES fuzz_ceir_binary.cpp LIBRARIES crd-ceir CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/binary")
