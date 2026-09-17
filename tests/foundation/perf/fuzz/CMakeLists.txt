# The diagnostic-bundle ingestion fuzz target (REPO.DEV.9; DIAG.5d(e2)): import_bundle -- the bounded, non-executing
# importer -- replayed over its committed adversarial corpus on every lane and fuzzed with libFuzzer under
# CRD_ENABLE_FUZZER (linux-clang-fuzz). This is the fuzzed proof of the DIAG.5d acceptance clause "reject traversal /
# oversized / decompression-bomb inputs". Contract: docs/design/test-instruments.md; harness: tests/support/fuzz.
crd_fuzz_target(crd-fuzz-perf-bundle
    SOURCES fuzz_bundle_import.cpp LIBRARIES crd-perf CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
