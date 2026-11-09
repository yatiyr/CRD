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

# WebSocket over HTTP/1.1 (4.2a).
#   crd-fuzz-net-websocket   the parsers alone, then a server and a client behind a real TLS peer under any plaintext,
#                            frames, opening handshakes, time, reads, closes and the transport's end: one head, then
#                            frames legal for its side, nothing after a Close or a refusal, nothing allocated.
crd_fuzz_target(crd-fuzz-net-websocket
    SOURCES fuzz_websocket.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-websocket")

# QMux's 0-RTT (4.2a).
#   crd-fuzz-net-qmux-early   a server reading hostile early data, answering at 0.5-RTT, and a client sending early data
#                             to a real TLS server that accepts, refuses or answers anything: what each side sends stays
#                             valid, nothing counts as delivered before the answer, nothing allocated.
crd_fuzz_target(crd-fuzz-net-qmux-early
    SOURCES fuzz_qmux_early.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-qmux-early")

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

# QPACK (4.2a).
#   crd-fuzz-net-qpack          any bytes on both of its streams and as sections, a table written apart that predicts
#                               every instruction, field and blocked stream, encoder and decoder over a delayed stream,
#                               and Huffman round trips.
crd_fuzz_target(crd-fuzz-net-qpack
    SOURCES fuzz_qpack.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-qpack")
target_include_directories(crd-fuzz-net-qpack PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-net-qpack-libfuzzer)
    target_include_directories(crd-fuzz-net-qpack-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

# HPACK (4.2a).
#   crd-fuzz-net-hpack          any block under any limits, a table written apart that predicts every field, refusal and
#                               capacity, and encoder and decoder across SETTINGS changes.
crd_fuzz_target(crd-fuzz-net-hpack
    SOURCES fuzz_hpack.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-hpack")
target_include_directories(crd-fuzz-net-hpack PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-net-hpack-libfuzzer)
    target_include_directories(crd-fuzz-net-hpack-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

# HTTP/2 (4.2a).
#   crd-fuzz-net-http2   a hostile peer at a server and at a client, any bytes cut anywhere: views inside the input,
#                        streams and output within their limits, nothing after a failure; and two honest sides over a
#                        wire that cuts anywhere, every byte of both bodies arriving in order.
crd_fuzz_target(crd-fuzz-net-http2
    SOURCES fuzz_http2.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-http2")
target_include_directories(crd-fuzz-net-http2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-net-http2-libfuzzer)
    target_include_directories(crd-fuzz-net-http2-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

# An HTTP/3 connection (4.2a).
#   crd-fuzz-net-http3          a hostile peer writing any streams, frames, ends and resets at a server and a client,
#                               whose application calls every method at any time; both sides honest over any frame sizes
#                               and a held encoder stream.
crd_fuzz_target(crd-fuzz-net-http3
    SOURCES fuzz_http3.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-http3")
target_include_directories(crd-fuzz-net-http3 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-net-http3-libfuzzer)
    target_include_directories(crd-fuzz-net-http3-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

# WebTransport sessions (4.2a).
#   crd-fuzz-net-webtransport   a peer driving HTTP/3 by hand against sessions on a server and a client, whose
#                               application calls every method at any time; both sides honest under tight flow control,
#                               nothing lost or leaked.
crd_fuzz_target(crd-fuzz-net-webtransport
    SOURCES fuzz_webtransport.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-webtransport")
target_include_directories(crd-fuzz-net-webtransport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-net-webtransport-libfuzzer)
    target_include_directories(crd-fuzz-net-webtransport-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

# A session's channels (4.2a).
#   crd-fuzz-net-channel-link   a hostile peer of a link on each path, every quota coming back when it goes; honest ends
#                               under tight buffers, every message of a reliable channel whole and in order.
crd_fuzz_target(crd-fuzz-net-channel-link
    SOURCES fuzz_channel_link.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-channel-link")
target_include_directories(crd-fuzz-net-channel-link PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-net-channel-link-libfuzzer)
    target_include_directories(crd-fuzz-net-channel-link-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

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
