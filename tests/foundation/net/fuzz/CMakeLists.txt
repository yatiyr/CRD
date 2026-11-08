# The negotiation fuzz target (NET-STD). Its committed corpus is replayed on every lane and linux-clang-fuzz builds
# the libFuzzer executable from the same source. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-net-negotiation   decode_negotiation, select_negotiation, write_answer, verify_answer and write_offer
#                              against a restated wire layout and the contract's selection and downgrade rules.
crd_fuzz_target(crd-fuzz-net-negotiation
    SOURCES fuzz_negotiation.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")

# Addresses and endpoints (4.2a).
#   crd-fuzz-net-endpoint   any text as an address and as an endpoint: one printed text per value that reads back to it,
#                           and on Linux the same verdict and bytes as glibc's inet_pton.
crd_fuzz_target(crd-fuzz-net-endpoint
    SOURCES fuzz_endpoint.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-endpoint")

# Frames and admission (NET.3).
#   crd-fuzz-net-admission   any bytes on a stream and in datagrams, against a restated frame layout and admission rule; a
#                            stream at every split reading as it reads whole; fragments of generated messages on a hostile
#                            path; every budget given back.
crd_fuzz_target(crd-fuzz-net-admission
    SOURCES fuzz_admission.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-admission")

# Coded frames (NET.3).
#   crd-fuzz-net-coded-frames   any header and body on coded channels, against a restated rule of what a coded frame may
#                               declare, costs and delivers: every budget given back, and only a body its writer hashed
#                               delivered.
crd_fuzz_target(crd-fuzz-net-coded-frames
    SOURCES fuzz_coded_frames.cpp LIBRARIES crd-net crd-compression
    CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-coded-frames")

# State hashes (NET.3).
#   crd-fuzz-net-state-hash   any script of records and reports against an unbounded restatement of the exchange.
crd_fuzz_target(crd-fuzz-net-state-hash
    SOURCES fuzz_state_hash.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-state-hash")

# The generated wire formats (NET.3): scripts/gen-wire-schemas.py writes one target per schema.
#   crd-fuzz-wire-session-control   every message of the session control channel's generated decoder.
#   crd-fuzz-wire-test              every field type, shape and revision rule of the schema language.
#   crd-fuzz-wire-delta-test        the same for baseline deltas: what a delta rebuilds, and that its delta is the input.
crd_fuzz_target(crd-fuzz-wire-session-control
    SOURCES fuzz_wire_session_control.cpp LIBRARIES crd-net
    CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-wire-session-control")
crd_fuzz_target(crd-fuzz-wire-test
    SOURCES fuzz_wire_test.cpp LIBRARIES crd-net-quantized CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-wire-test")
target_include_directories(crd-fuzz-wire-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-wire-test-libfuzzer)
    target_include_directories(crd-fuzz-wire-test-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
crd_fuzz_target(crd-fuzz-wire-delta-test
    SOURCES fuzz_wire_delta_test.cpp LIBRARIES crd-net-quantized
    CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-wire-delta-test")
target_include_directories(crd-fuzz-wire-delta-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-wire-delta-test-libfuzzer)
    target_include_directories(crd-fuzz-wire-delta-test-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

# Delta streams (NET.3).
#   crd-fuzz-net-delta-stream   any frame bodies against a reader: what it delivers passes the generated decoder and its
#                               hash, its acknowledgements rise, and a writer on a hostile path never makes it deliver
#                               anything but what was written.
crd_fuzz_target(crd-fuzz-net-delta-stream
    SOURCES fuzz_delta_stream.cpp LIBRARIES crd-net-quantized
    CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-delta-stream")
target_include_directories(crd-fuzz-net-delta-stream PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-net-delta-stream-libfuzzer)
    target_include_directories(crd-fuzz-net-delta-stream-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
