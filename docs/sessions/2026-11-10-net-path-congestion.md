# 2026-11-10 — NET-PATH: congestion control and pacing

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S66](../design/network-foundation.md#congestion).

CI first: dispatched run 42744641737 (45dbd882) and push run 42744624925 were still queued; nothing to read yet.

## What landed

- `crd/net/congestion.hpp`:
  - `CongestionController`, the versioned interface, and `CongestionPolicy`.
  - NewReno (RFC 9002 appendix B), CUBIC (RFC 9438, in integers) and HyStart++ (RFC 9406).
  - The spurious-loss undo, and `Pacer`.
- `QuicConnection` applies them:
  - 0-RTT and 1-RTT data go only when the window has room for a whole datagram and the pacer allows. The pacer's release time is a connection timer.
  - `set_congestion()` swaps the controller live and keeps the window.
  - The Retry reset is wired.
  - `QuicRecoveryEvents::on_spurious` reports each spurious loss together with the time it was declared.
- Two connection defects, found while proving this:
  - Capping a datagram to the window's spare room produced runs of 128 ACK-only datagrams. Data now waits until a whole datagram fits.
  - Handshake CRYPTO data held behind 1-RTT data, which a peer that is still handshaking never acknowledges, wedged both ends until the idle timeout. The handshake's CRYPTO data is no longer held by the window or the pacer.
- Tests that assumed an instant PING (`test_quic_client.cpp`) now wait for the paced one.
- Register:
  - New entries `cc.cubic` and `cc.hystart`; `quic.recovery` gains the new evidence.
  - Batch 12 had closed 4.2a without marking `cerid.game`, `quic.invariants`, `quic.tls`, `quic.datagram` and `quic.v2` implemented. They are now.
  - Because `quic.tls` and `quic.v2` became implemented, the cryptography inventory changed. The findings of both exports were restated and reviewed (counsel review stays open).

## Evidence

- linux-gcc-debug:
  - `crd-net-tests [net]`: 628 cases, 69.7 million assertions. The `[net-path]` cases are 16.
  - All fuzz corpus replays and `crd-check-*` guards pass (38 of 38).
- linux-gcc-public-checks: the QUIC interop with ngtcp2, HTTP/3 with nghttp3 and quiche, and WebTransport with Quinn and webtransport-go all pass with congestion control on (5 of 5).
- linux-clang-fuzz: `crd-fuzz-net-congestion`, seed 1, 120 s, 906k executions, no artifact.
- CUBIC's integer curve matches 0.4 (t − K)³ + W_max, computed in doubles, to within one byte at every point checked.
- Each change fails its test when removed:
  - Window gate: `watch.over` and the full-window case fail.
  - Pacer timer: the release-time checks fail.
  - Spurious event: the recovery case fails.
  - Handshake exemption: the twelve-connection hostile client test stalls until idle.

## Next

ECN marking and validation feeding `on_congestion`, then DPLPMTUD, then BBRv3. BBRv3's first step is to stamp delivery rate in `QuicSentPacket` and link `crd-hesap-net`.
