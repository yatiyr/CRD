# 2026-11-10 — NET-PATH: ECN marking and validation

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S67](../design/network-foundation.md#ecn).

CI first: every failed job of run 42736485342 (07994585) fails only `crd-generated-sources` and `crd-repository-tools`,
both on `special_refs.inc`'s manifest, which 38b7a6dd already records. Nothing of this track is open.

## What landed

- `crd/net/ecn.hpp`: `Ecn` and `EcnCounts`, one type for sockets, the ACK codec, recovery and validation (QuicEcn and
  QuicEcnCounts are gone). `quic_space_index()` replaces two copies of `index_of`.
- Sockets: per-datagram codepoints read and marked on Linux (`IP_TOS`/`IPV6_TCLASS` controls in `recvmmsg` and
  `sendmmsg`) and Windows (`WSARecvMsg`, `WSASendMsg`), by the peer's own family on a dual-stack socket;
  `DatagramSocket::features()` reports them. Windows refuses a sender's CE: that datagram goes unmarked, marking stays.
- `QuicEcnValidator` (RFC 9000 13.4.2 and A.4): testing by ten packets or three unbacked-off PTOs, validation, the
  failures `Missing`, `Decreased`, `Exceeded`, `Short` and `Lost`, late acknowledgements unchecked, and CE on a
  validated path into `on_congestion` (B.7). `QuicRecovery::on_ack` reports what it needs; `QuicSentPacket::ecn`.
- `QuicConnectionConfig::ecn` (ECT(0) default, ECT(1) for L4S, Not-ECT none), `QuicConnection::marking()`, the host
  stamping it and clearing it for sockets that cannot mark, early datagrams keeping theirs.
- Fixtures: the hostile path bleaches, re-marks, drops marked datagrams and marks CE; the virtual socket carries codepoints.
- `crd-fuzz-net-quic-ecn` (new) and ECN oracles in the recovery and connection fuzzers; the interop peer reports `ecn`.

## Evidence

- linux-gcc-debug `crd-net-tests [net]`: 644 cases, 72.6 million assertions; every `crd-fuzz-net-*` replay and
  `crd-check-*` guard passes (`crd-check-environment-reads` segfaulted once and passed alone: this host).
- win-debug `crd-net-tests [net]`: 643 cases, 69.3 million assertions, the loopback codepoint test included.
- linux-clang-fuzz, seed 1: `quic-ecn` 1.26 M executions in 120 s and `quic-recovery` 609 k, no event.
  `quic-connection` stopped twice (a stack underflow in memcpy; a wild read in AWS-LC's GCM tag) on inputs that never
  replay: this host's known misexecution. Host-check seed 8403 is added for the hosted arbiter.
- linux-gcc-public-checks: `crd-net-webtransport-interop-quinn` 82 of 82 and `-go` 30 of 30, every Cerid end ECN
  `capable` on a clean path and `failed`/`missing` behind the bleaching relay.

## Next

ECN interop with ngtcp2 (its peer passes codepoints to `ngtcp2_conn_read_pkt` and marks from its packet info), then
DPLPMTUD, then BBRv3.
