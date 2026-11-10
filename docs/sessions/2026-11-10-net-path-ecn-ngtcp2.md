# 2026-11-10 — NET-PATH: ECN interop with ngtcp2

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S67](../design/network-foundation.md#ecn).

CI first: every failed job of run 42736485342 (07994585) read (public-checks; GCC debug, release, shipping and ASan; GCC
ARM64 debug; Android; win-release, win-asan, clang-cl and win-arm64) fails only `crd-repository-tools` and
`crd-generated-sources` on `special_refs.inc`'s manifest, which 38b7a6dd records. Its interop tests pass. Nothing of
this track is open; no later run has finished.

## What landed

- `ngtcp2_quic_peer.c`: `IP_RECVTOS` and `recvmsg`, so each datagram's codepoint reaches `ngtcp2_conn_read_pkt`;
  `sendmsg` with an `IP_TOS` control message marks what ngtcp2 chose. One `ngtcp2_pkt_info` per write round, kept across
  `NGTCP2_ERR_WRITE_MORE` as ngtcp2 requires (each call had its own uninitialised one before). The report adds the
  datagrams marked ECT(0), the last packet's codepoint, the codepoints read and whether ngtcp2 set a slow-start threshold.
- `cerid_quic_peer.cpp` reports `ecn_ce` (CE the peer reported, all spaces) and `reductions` (the controller's).
- `interop_common.Relay` takes `mark`: unmarked (bleaching, as before), every datagram CE, or re-marked ECT(1).
- `quic_interop.py` checks both ends' ECN verdicts in every case. ngtcp2 marks at most its ten testing datagrams until
  it validates, so more than ten with the last one marked is its "capable". Six new cases each way: CE on every
  datagram (v1, v2), CE with 2% loss and jitter (v1, v2), ECT(1) re-marking (v1, v2).
- `http3_interop.py`: Cerid validates against nghttp3/ngtcp2 on the loopback and fails as `Missing` against quiche, which
  reports no counts, and behind the relay.

## Evidence

linux-gcc-public-checks:

- `crd-net-quic-interop-ngtcp2`: 114 of 114 cases. On the loopback both ends validate (an 8 MiB transfer: ngtcp2 marked
  6582 datagrams, read 7316 marked ECT(0)). Bleaching relay: Cerid `failed`/`missing`, ngtcp2 marked 2 and stopped.
  ECT(1) relay: Cerid `failed`/`exceeded`, ngtcp2 failed. CE relay: both capable; Cerid read 976 CE in ngtcp2's
  counts and made 470 reductions, and ngtcp2 set its slow-start threshold.
- `crd-net-http3-interop-nghttp3` 66 of 66, `crd-net-http3-interop-quiche` 54 of 54.

## Next

DPLPMTUD (RFC 8899, RFC 9000 section 14.3), then BBRv3.
