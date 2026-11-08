# 2026-11-08 — 4.2a: QUIC frames and transport parameters

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S35](../design/network-foundation.md#quic-frames).

## CI first

Nothing to fix: no run has completed. 42430818018 and 42435554329 hold a queued Preflight, 42428822989 and
42439810526 have queued jobs, 42443963304 (4d5649dc) is pending. The relayed user words about the GPL rewrite belong
to the other agent's work (operator note, 2026-11-07); this batch did not touch it.

## What landed (capability `network.transport`)

- `quic_frame.hpp`: RFC 9000 section 20.1's transport errors with RFC 9368's VERSION_NEGOTIATION_ERROR; every frame
  of section 19 and RFC 9221's DATAGRAM read into views of the payload and written in shortest encodings; ACK frames
  from descending ranges with ECN counts and `QuicAckRanges` to walk a read one; table 3's placement.
- `quic_transport_parameters.hpp`: section 18's parameters with RFC 9221's, RFC 9287's and RFC 9368's (identifiers
  from the register), a reserved parameter and version in every write, section 7.3's connection ID checks and RFC
  9368's compatible selection and downgrade checks.
- `crd-fuzz-net-quic-codec` with a 7-seed committed corpus.

## Evidence

- win-debug (MSVC): crd-net-tests 183 cases pass, 14 new (75,264 assertions in them). linux-gcc-debug: 184 cases
  pass. Corpus replay: 7 inputs on both.
- Byte for byte: RFC 9001 A.2's CRYPTO frame and client parameters, A.3's ACK and CRYPTO frames.
- Eleven source mutations (ACK bounds, the shortest type rule, 0-RTT placement, MAX_STREAMS, max_ack_delay, server-only
  and repeated parameters, the downgrade check, the Retry connection ID, the reserved parameter) each caught.
- linux-clang-fuzz: 3.84 M executions in 410 s, corpus 7 to 591, no artifacts.
- LLVM 20 strict tidy clean on all 7 new C++ files.

## Next

- Audit the first completed hosted run containing 9c7a6717 (closes NET.3) and this batch.
- 4.2a: packets per level on these frames (Initial, Handshake and 1-RTT protection through crd-crypto's keys, the
  client Initial padded to 1200, key discard), a connection driving `QuicHandshake` to 1-RTT over the ingress with
  PTO retransmission, the INVALID_TOKEN close; then interop against a pinned peer (ngtcp2 or picoquic).
