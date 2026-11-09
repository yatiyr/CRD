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

# QUIC headers and admission stages S1 to S3 (4.2a).
#   crd-fuzz-net-ingress   any datagram: header views inside it, varints in their shortest form, and through a server
#                          only held connections routed, only full Initials admitted, every answer within three times
#                          the datagram and parsing back as a Version Negotiation or a verified Retry, and a Retry
#                          token that does not validate closed by one Initial the client opens (INVALID_TOKEN).
crd_fuzz_target(crd-fuzz-net-ingress
    SOURCES fuzz_ingress.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-ingress")

# QUIC frames and transport parameters (4.2a).
#   crd-fuzz-net-quic-codec   any payload or parameter set: refusals consume and change nothing, accepted frames and
#                             parameters write back in their shortest form and read back the same, ACK ranges descend
#                             and survive a rewrite, exactly one reserved parameter is written, versions come from
#                             the inputs.
crd_fuzz_target(crd-fuzz-net-quic-codec
    SOURCES fuzz_quic_codec.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-quic-codec")

# QUIC packets per encryption level (4.2a).
#   crd-fuzz-net-quic-packet   any datagram through a server holding every level's keys: packets walked in order inside
#                              it, opened packets non-empty and never twice, every refusal named; and any frames through
#                              a writer and its peer: refusals as restated, floors and blocks reached, the shortest
#                              Length, the frames and their PADDING opened under the phase they were sealed in.
crd_fuzz_target(crd-fuzz-net-quic-packet
    SOURCES fuzz_quic_packet.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-quic-packet")

# QUIC acknowledgements and loss recovery (4.2a).
#   crd-fuzz-net-quic-recovery   any sends, ACK frames, received packets, timeouts and discards on one connection's
#                                recovery against a model of every packet: each settled once by its rule, losses
#                                before acknowledgements, unsent acknowledgements refused, ranges and timers sound.
crd_fuzz_target(crd-fuzz-net-quic-recovery
    SOURCES fuzz_quic_recovery.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-quic-recovery")

# QUIC byte streams and flow control (4.2a).
#   crd-fuzz-net-quic-stream   any frames, reads, writes, sends, acknowledgements, losses and window operations on one
#                              receive buffer, send buffer and window against per-byte models: every verdict, chunk,
#                              byte and end the models', refusals changing nothing, the window's limit bounded.
crd_fuzz_target(crd-fuzz-net-quic-stream
    SOURCES fuzz_quic_stream.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-quic-stream")

# A QUIC connection's streams (4.2a).
#   crd-fuzz-net-quic-stream-set   two ends' stream sets under any application calls, packets delivered in any order
#                                  and number, acknowledged or lost, and injected frames: has_frame() exact, every
#                                  frame fitting and round tripping, a well-behaved peer never refused, every byte read
#                                  the byte written, nothing allocated.
crd_fuzz_target(crd-fuzz-net-quic-stream-set
    SOURCES fuzz_quic_stream_set.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-quic-stream-set")

# A QUIC connection's datagrams (4.2a).
#   crd-fuzz-net-quic-datagram   a sender's and a receiver's datagrams under any queueing, frames written into any
#                                room, delivered any number of times, acknowledged or lost, time, polls, reads and
#                                injected frames: has_frame() exact, every frame round tripping with its payload, the
#                                receiver byte-exact in order, the size advertised enforced exactly, every id reported
#                                once, nothing allocated.
crd_fuzz_target(crd-fuzz-net-quic-datagram
    SOURCES fuzz_quic_datagram.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-quic-datagram")

# QMux, QUIC's streams and datagrams over TLS on a byte stream (4.2a).
#   crd-fuzz-net-qmux   a server behind a real TLS client under any records, frames, cuts, time, reads, pings, closes and
#                       the transport's end: whole records of allowed frames only, its parameters first, nothing after
#                       CONNECTION_CLOSE, the state only moving forward, nothing allocated.
crd_fuzz_target(crd-fuzz-net-qmux
    SOURCES fuzz_qmux.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-qmux")

# The TCP and TLS path's host (4.2a).
#   crd-fuzz-net-qmux-host   a server and a client host over pipes under any connects, resets, cuts, stalls, raw bytes,
#                            clock steps, closes, wakes, stale handles and accept failures: every connection reported
#                            closed once, its handle stale from then, every count consistent and all memory returned.
crd_fuzz_target(crd-fuzz-net-qmux-host
    SOURCES fuzz_qmux_host.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-qmux-host")

# A client's ticket store (4.2a).
#   crd-fuzz-net-ticket-cache   any keeps, takes and clock steps under any bounds, against a model of the contract:
#                               every ticket returned byte-exact and once, the newest of its binding, the size and
#                               every counter as predicted, nothing allocated.
crd_fuzz_target(crd-fuzz-net-ticket-cache
    SOURCES fuzz_ticket_cache.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-ticket-cache")

# A QUIC connection (4.2a).
#   crd-fuzz-net-quic-connection   a network script between a client and a server (deliveries, losses, duplicates,
#                                  reorders, corrupted bytes, time, a Retry), then a clean path: the handshake confirms
#                                  or a side closes, within the amplification limit and the Initial floor, nothing
#                                  allocated, and a confirmed pair carries a stream whole and agrees on the Retry, a
#                                  resumed one its 0-RTT stream whole or reset by a rejection; and any frames from a
#                                  raw peer in 1-RTT and in Initial packets: the server stays open or closes with an
#                                  error RFC 9000 names, its close the only frame it sends then.
crd_fuzz_target(crd-fuzz-net-quic-connection
    SOURCES fuzz_quic_connection.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-quic-connection")

# Frames and admission (NET.3).
#   crd-fuzz-net-admission   any bytes on a stream and in datagrams, against a restated frame layout and admission
#                            rule; a stream at every split reading as it reads whole; fragments of generated messages on
#                            a hostile path; every budget given back.
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
#   crd-fuzz-wire-delta-test        the same for baseline deltas: what a delta rebuilds, and that its delta is the
#                                   input.
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
