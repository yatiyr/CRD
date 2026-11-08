# 2026-11-08 — 4.2a: this side's connection IDs and stateless reset

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S42](../design/network-foundation.md#quic-connection-ids).
Sources: RFC 9000 sections 5.1.1, 5.1.2, 10.3 to 10.3.3, 19.15 and 19.16, read from the RFC Editor's text.

## CI first

- Dispatch 42472298588 (e32b3670) failed crd-repository-tools on every lane: `crd-fuzz-net-quic-datagram` had no
  `scripts/fuzz.py` entry. It now has one (39b6e1f5).
- win-arm64-debug failed C4324 on `QuicPacketProtection`, whose keys are 16-byte aligned. The class now carries the
  same warning guard as crypto's `AeadKey` (dc0200fc).
- Every other failure in that run was a GitHub 503/504 download timeout. Dispatches 42473131224 and 42475887688
  (NET.3's last lanes, the recovery fuzz legs) are still queued.

## What landed (capability `network.transport`)

- crd-crypto `QuicResetKey`: a token is the first 16 bytes of HMAC-SHA256(key, connection ID), so a fleet sharing
  the key resets connections it lost.
- `QuicConnectionIds`, with `QuicRoutedIds` over the loop's table:
  - after confirmation the peer holds its active_connection_id_limit of this side's IDs;
  - NEW_CONNECTION_ID is resent until acknowledged;
  - retired IDs are replaced, and `rotate_connection_ids()` uses Retire Prior To;
  - a server sends its stateless_reset_token;
  - closing hands every ID back.
- RETIRE_CONNECTION_ID is refused for a sequence never issued, for the packet's own ID and toward zero-length IDs.
- Detection: when a datagram's first packet does not open, its tail is compared in constant time with the tokens of
  the peer IDs this side used and has not retired. A match drains, from Closing too. Every datagram reaches the ID
  length plus 22 bytes.
- The ingress answers a short header for no connection with a reset that is shorter than its trigger and at least
  21 bytes, within the answer rate.

## Evidence

- win-debug (MSVC): crd-net-tests passes, 287 cases, 5 of them new. The crypto `[quic-token]` cases pass, one of
  them new, with independent HMAC vectors.
- 17 source mutations, all caught. One first survived: the index of the packet's own ID after an earlier
  retirement in the same packet. A test now covers it. Two were rewritten so that they compile.
- Fuzz oracles: `crd-fuzz-net-ingress` holds the reset size and token rules (new seed `short-unknown-long`), and
  `crd-fuzz-net-quic-connection` holds each host's table to its connection's IDs and ends with a forged and a real
  reset. Corpus replays pass on win-debug.

## Next

- 4.2a: Retry and NEW_TOKEN on the client with the server's NEW_TOKEN and the INVALID_TOKEN close, then compatible
  version negotiation's switch, 0-RTT, the IngressHandler, peer interop, TCP/TLS and the browser endpoints.
