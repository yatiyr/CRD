# The erasure-code fuzz targets (HESAP-NET). Their committed corpora are replayed on every lane and linux-clang-fuzz
# builds each libFuzzer executable from the same source. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-hesap-erasure   ReedSolomonCode::decode and encode against the restated refusal order, the dual code's
#                            checks and the lost sources, and the dispatched GF(2^8) dot product of every level this
#                            CPU runs against a shift-and-add evaluation.
#   crd-fuzz-hesap-flexfec   RFC 8627's FlexFEC: the header against a bit-by-bit parser, repair packets against
#                            the literal bit strings and every member's recovery, and the window decoder against a
#                            peeling of the index sets.
#   crd-fuzz-hesap-red       RFC 2198's redundant audio data: payloads against a reader restated bit by bit, and
#                            written packets against the payload built bit by bit and their read-back.
crd_fuzz_target(crd-fuzz-hesap-erasure
    SOURCES fuzz_erasure.cpp LIBRARIES crd-hesap-comms crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
crd_fuzz_target(crd-fuzz-hesap-flexfec
    SOURCES fuzz_flexfec.cpp LIBRARIES crd-hesap-comms crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-flexfec")
crd_fuzz_target(crd-fuzz-hesap-red
    SOURCES fuzz_red.cpp LIBRARIES crd-hesap-comms crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-red")
