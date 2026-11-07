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
#   crd-fuzz-hesap-motion    The interpolation, extrapolation and rewind history on any snapshot, time and limit:
#                            every level against the restated expressions, nlerp finite and unit, slerp near the true
#                            arc, the history against a linear search over a plain list of stamps.
#   crd-fuzz-hesap-quantize  Bounded floats and smallest-three quaternions on any format, value and word a peer can
#                            send: formats against their restated conditions, every level bit for bit, every decoded
#                            word unit length and refused exactly as restated.
#   crd-fuzz-hesap-link      The loss and delay models, slot gate and medium grants on any setting and word: the loss
#                            machines and the table delay restated, refusals as restated and writing nothing, delays within
#                            their bounds and reproducible, departures in order and budgets respected.
#   crd-fuzz-hesap-raptorq   RFC 6330's RaptorQ on any ESIs and object parameters: refusals as restated, the solver's verdict
#                            against the rank of the literal matrix A, its answer against the encoder's and every symbol,
#                            the payload ID and OTI bit by bit, the object's blocks and sub-symbols covering it exactly.
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
crd_fuzz_target(crd-fuzz-hesap-motion
    SOURCES fuzz_motion.cpp LIBRARIES crd-hesap-net crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-motion")
crd_fuzz_target(crd-fuzz-hesap-link
    SOURCES fuzz_link.cpp LIBRARIES crd-hesap-net crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-link")
crd_fuzz_target(crd-fuzz-hesap-raptorq
    SOURCES fuzz_raptorq.cpp LIBRARIES crd-hesap-net crd-core CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-raptorq")
target_include_directories(crd-fuzz-hesap-raptorq PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-hesap-raptorq-libfuzzer)
    target_include_directories(crd-fuzz-hesap-raptorq-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
