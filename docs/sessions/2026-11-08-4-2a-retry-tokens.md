# 2026-11-08 — 4.2a: Retry, NEW_TOKEN and INVALID_TOKEN

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S43](../design/network-foundation.md#quic-retry). Sources: RFC 9000
sections 7.2, 7.3, 8.1.2, 8.1.3, 17.2.2, 17.2.5 and 19.7 (17.2.5 and 19.7 read again from the RFC Editor's text);
RFC 9001 sections 5.2 and 5.8; RFC 9002 sections 6.2.2 and 6.3. The user's relayed words about the GPL rewrite belong
to the other agent (operator note, 2026-11-07); this batch did not touch that work.

## CI first

- Dispatch 42472298588 (e32b3670): linux-clang-arm64-asan and linux-gcc-arm64-asan failed two of this track's tests.
  Three ingress tests answered decisions whose datagram was a dead temporary (stack-use-after-scope). The packet test
  helper passed an empty span's null data to memcpy (UBSan). Fixed in f3cd695a. `answer()` now documents that it
  reads the classified datagram.
- crd-repository-tools failed on every lane for the fuzz.py entry, which 39b6e1f5 already fixed. 900348cb's run and
  NET.3's complete-tier dispatch 42473131224 have not started.

## What landed (capability `network.transport`)

- A client takes one Retry. Every other one is dropped and counted: a bad tag, another ID, no token, another version,
  its own ID again, a token over 256 bytes, or a Retry after a server packet. Taking it re-keys the Initial, carries
  the token and keeps the packet numbers. `QuicRecovery::retry` starts recovery over and takes the first flight's
  time as the RTT estimate. The server's retry_source_connection_id is checked.
- A server sends the host's NEW_TOKEN token (`QuicAccept::new_token`) once the handshake is confirmed, and resends it
  until it is acknowledged. A server Initial that carries a token is dropped.
- The ingress answers a Retry token that does not validate with one Initial that closes the connection with
  INVALID_TOKEN, within the answer rate. The I/O loop sends it.

## Evidence

- crd-net-tests passes on linux-gcc-asan (294 cases).
- Fuzz corpora replay clean. The new seeds `initial-v1-forged-retry-token` and `script-retry` reach the close
  oracle and the 0xFD Retry script.
- 12 source mutations, all caught: a second Retry, the same ID, no tag check, no re-keying, no CRYPTO resend, no
  RTT estimate, no retry_source check, no NEW_TOKEN, no close, the wrong close code, a server token taken, an empty
  token taken.
- Not yet run: the MSVC, clang-cl and ARM64 lanes (CI), and interop with a peer's Retry and NEW_TOKEN.

## Next

- 4.2a: compatible version negotiation's switch, a bounded 0-RTT reorder buffer and 0-RTT, the connection behind the
  IngressHandler (it mints the NEW_TOKEN token), interop with a pinned peer, the TCP/TLS path and the browser
  endpoints.
