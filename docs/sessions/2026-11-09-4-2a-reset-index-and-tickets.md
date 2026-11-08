# 2026-11-09 — 4.2a: a reset found by its token, the client's ticket cache

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S48](../design/network-foundation.md#quic-reset-index) and
[S49](../design/network-foundation.md#ticket-cache). Sources: RFC 9000 sections 10.3.1, 10.3.2 and 21.11, RFC 9846
sections 4.6.1 and C.4, RFC 9369 section 5 (RFC Editor text).

CI first: run 42489017698 (dad40502) and the finished jobs of 42510394122 (5e685840) failed on four things 954d098a had
already fixed (the version-change key check where key logging is compiled out, two tidy local constants, C4324 on the
fuzz harness's `Pair`, and win-arm64). win-debug-scalar failed once on `ntp-4.2.8p18.tar.gz` (a checksum mismatch on
downloads.nwtime.org's bytes; the same pin passed on linux-gcc-debug-scalar in the same run), a transient, left alone.

## What landed

- The reset index by token (`QuicHost`): the connection tells the host through `QuicConnectionIds` when a peer token
  becomes live (known and sent to) and when its ID retires or the connection closes; the host matches the last 16 bytes
  of a short header for no connection and requires the token's connection's peer address. This also repairs a defect:
  5ca261ef's 22-byte IPv6 peer key was over the table's 20 bytes, so no IPv6 peer's reset was ever found.
- One token under two IDs of a connection is PROTOCOL_VIOLATION (RFC 9000 section 10.3.2's MAY).
- `TicketCache` (ticket_cache.hpp), the store a `QuicClient` uses unless its configuration names one.

## Evidence

- MSVC win-debug: crd-net-tests 339 cases, 25.4 million assertions. New: three connections to one server with the reset
  ending the one it names; an IPv6 peer with a zone; a retired token and a token from another address ending nothing;
  exactly when a host learns and forgets a token; a client's own cache resuming with 0-RTT in both versions, off, or
  replaced; expiry within a turn and before the next connect; `test_ticket_cache.cpp`'s six cases.
- 25 source mutations on the host, connection, client and cache: 24 caught. Two were caught only after a test was added
  (a double forget) or the mutation repaired; the host's size gate (`datagram.size < 21`) survives on MSVC because
  reading before a short datagram finds nothing there: it is a bounds check only the sanitizers can observe.
- `crd-fuzz-net-ticket-cache` against a model of the contract (3 seeds replay); `crd-fuzz-net-quic-connection`'s
  hosts now keep a token table, with its size held to the IDs held and to zero after Closed.

## Next

- Audit CI for this batch's commits; then 4.2a's remaining work: interop with a pinned peer implementation, the TCP and
  TLS path and the browser endpoints.
