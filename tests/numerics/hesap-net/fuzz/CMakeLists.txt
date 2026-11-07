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
#   crd-fuzz-hesap-estimators  The RTT, jitter, delay-trend and rate estimators on any feedback and clock a peer can
#                            send: restated bounds and updates, every state finite, refusals that change nothing.
#   crd-fuzz-hesap-clock     The clock filters on any timestamps a peer can write: the exchange restated, the filter's
#                            order, stages and event against its definitions, the line finite, refusals that change
#                            nothing, the media map and capture aligner against their invariants.
#   crd-fuzz-hesap-quantize  Bounded floats and smallest-three quaternions on any format, value and word a peer can
#                            send: formats against their restated conditions, every level bit for bit, every decoded
#                            word unit length and refused exactly as restated.
crd_fuzz_target(crd-fuzz-hesap-erasure
    SOURCES fuzz_erasure.cpp LIBRARIES crd-hesap-net crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
crd_fuzz_target(crd-fuzz-hesap-flexfec
    SOURCES fuzz_flexfec.cpp LIBRARIES crd-hesap-net crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-flexfec")
crd_fuzz_target(crd-fuzz-hesap-red
    SOURCES fuzz_red.cpp LIBRARIES crd-hesap-net crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-red")
crd_fuzz_target(crd-fuzz-hesap-estimators
    SOURCES fuzz_estimators.cpp LIBRARIES crd-hesap-net crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-estimators")
crd_fuzz_target(crd-fuzz-hesap-clock
    SOURCES fuzz_clock.cpp LIBRARIES crd-hesap-net crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-clock")
crd_fuzz_target(crd-fuzz-hesap-quantize
    SOURCES fuzz_quantize.cpp LIBRARIES crd-hesap-net crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-quantize")
