# The erasure-code fuzz target (HESAP-NET). Its committed corpus is replayed on every lane and linux-clang-fuzz builds
# the libFuzzer executable from the same source. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-hesap-erasure   ReedSolomonCode::decode and encode against the restated refusal order, the dual code's
#                            checks and the lost sources, and the dispatched GF(2^8) dot product of every level this
#                            CPU runs against a shift-and-add evaluation.
crd_fuzz_target(crd-fuzz-hesap-erasure
    SOURCES fuzz_erasure.cpp LIBRARIES crd-hesap-comms crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
