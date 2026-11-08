# 2026-11-08 — 4.2a: this side's connection IDs and stateless reset

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S42](../design/network-foundation.md#quic-connection-ids).
Sources: RFC 9000 sections 5.1.1, 5.1.2, 10.3 to 10.3.3, 19.15 and 19.16, read from the RFC Editor's text.

## CI first

- Dispatch 42472298588 (e32b3670) had two failures from this track, both fixed. `crd-fuzz-net-quic-datagram` had
  no `scripts/fuzz.py` entry (39b6e1f5). Win-arm64 failed C4324 on `QuicPacketProtection` (dc0200fc).
- Every other failure was a GitHub 503 or 504. NET.3's lanes (42473131224) and the recovery fuzz legs (42475887688)
  are still queued.

## What landed (capability `network.transport`)

- crd-crypto `QuicResetKey`: a token is HMAC-SHA256(key, connection ID) cut to 16 bytes, so a fleet that shares
  the key can reset connections it lost.
- `QuicConnectionIds` and `QuicRoutedIds`, over the loop's table:
  - the peer is kept at its ID limit, and NEW_CONNECTION_ID is resent until acknowledged;
  - a retired ID is replaced;
  - Retire Prior To rotates every ID;
  - a server sends stateless_reset_token;
  - closing hands every ID back.
- RETIRE_CONNECTION_ID is refused for a sequence never issued and for the packet's own ID.
- Detection is a constant-time compare against the tokens of used, unretired peer IDs, made when a datagram's first
  packet does not open. A match drains the connection. Datagrams are padded to the ID length plus 22 bytes.
- The ingress answers with a reset that is shorter than its trigger and at least 21 bytes. The I/O loop sends it.

## Evidence

- crd-net-tests passes on win-debug (MSVC, 287 cases) and linux-gcc-debug (289). The crypto `[quic-token]` cases
  pass, with independent HMAC vectors. LLVM 20 strict tidy is clean on the eleven changed sources and tests.
- 17 source mutations, all caught.
  - One first survived: the packet's own ID index after an earlier retirement in the same packet. A test now
    covers it.
  - The refusal toward zero-length IDs needs a raw server and has no test.
- Fuzz oracles:
  - the ingress holds each reset to its size and token rules;
  - the connection target holds each host's table to its connection's IDs;
  - a new seed rotates (checked once with a print);
  - confirmed runs end with a forged and a real reset.
- The corpus replays pass.

## Next

- 4.2a: Retry and NEW_TOKEN on the client (the server's NEW_TOKEN, the INVALID_TOKEN close), version negotiation's
  switch, 0-RTT, the IngressHandler, peer interop, TCP/TLS and the browser endpoints.
