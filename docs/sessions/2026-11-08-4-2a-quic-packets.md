# 2026-11-08 — 4.2a: QUIC packets per encryption level

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S36](../design/network-foundation.md#quic-packets).

## CI first

Nothing of this track failed. Dispatch 42428822989 (20a1cfcd) failed `crd-no-untagged-physical-numeric` on
bessel.hpp (already fixed by 40ad3b23, the rewrite agent's area) and the mikktspace UBSan shift (TP-5, registered).
The complete-tier dispatches 42430818018 and 42435554329 are still queued at Preflight, so NET.3's hosted audit
waits. The relayed user words about the GPL rewrite belong to the other agent's work; this batch did not touch it.

## What landed (capability `network.transport`)

- `quic_packet.hpp`: `QuicPacketWriter` (header, frames, PADDING to the sample's floor, the Initial floor and the
  channel's block, the shortest Length, seal, header protection; the QUIC bit greased on request),
  `QuicDatagramPackets` (coalesced packets, a foreign Destination Connection ID ends the walk) and
  `QuicPacketProtection` (keys per level from secrets or from `QuicHandshake`, RFC 9001 section 4.9's discards,
  windows per space, reserved bits and empty packets after the AEAD, the integrity limit, key phases with the next
  key derived ahead, initiated, followed and checked per section 6, a transport's own update interval).
- `quic_long_type_bits` in quic_header.hpp (the Retry writer uses it); `QuicLevel` moved from tls.hpp to quic.hpp.
- RFC 9001 and RFC 9369's sample packets moved to one shared test header, `quic_vectors.hpp`.
- `crd-fuzz-net-quic-packet` with 8 committed seeds whose verdicts the seed sink checks.

## Evidence

- win-debug (MSVC): crd-net-tests 192 cases pass, 9 new; crd-crypto-tests `[quic]` pass. linux-gcc-debug, -release
  and -relwithdebinfo: 193 cases, `[quic]` and `[quic-handshake]` pass, the packet corpus replays clean. The release
  lanes' LTO link first refused `frames()[0]` in three test sites (`-Wnull-dereference`); they now write through a
  checked helper.
- Byte for byte: RFC 9001 A.2 (1200-byte client Initial), A.3 and A.5 and RFC 9369's, written from their frames.
- A real hybrid TLS 1.3 handshake carried in these packets to 1-RTT in versions 1 and 2, HANDSHAKE_DONE confirming.
- Eighteen source mutations each caught: the reserved bits, empty packets, the QUIC bit, the foreign walk, the sample
  floor, the shortest Length, duplicates, each of the four key discards, the three section 6.4 and 6.2 checks, and
  the four conditions on initiating an update (one survivor at first, the acknowledgement rule; its test now holds).
- linux-clang-fuzz: 1.55 M executions in 421 s, corpus 8 to 245, no artifacts.
- LLVM 20 strict tidy clean on the changed C++ files.

## Next

- Audit NET.3's first completed complete-tier run (9c7a6717 onwards) and close it.
- 4.2a: the connection on these packets (CRYPTO reassembly, acknowledgements and RFC 9002 loss recovery with PTO,
  streams and flow control, DATAGRAM, connection IDs and stateless reset, the INVALID_TOKEN close, a bounded 0-RTT
  reorder buffer), then interop against a pinned peer (ngtcp2 or picoquic).
