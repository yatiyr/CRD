# 2026-11-21: NET-PATH, per-socket send admission

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S33](../design/network-foundation.md#sockets)
(socket sets, send admission) and [S75](../design/network-foundation.md#migration).

## CI first

Run 44854356220 (d17003bd): win-tidy under LLVM 22.1.8 passed. The failures there are the licence manifest's
recorded bytes, which b0882ee0 already records, and the registered TP-5 (MikkTSpace UBSan) on linux-gcc-asan shard 2.
win-arm64-debug, win-asan and linux-clang-tsan were still running.

## What landed

- `SendQueue` admission: `sockets` share the slots with a guaranteed share each (slots / (2 x sockets), at least
  one where the slots allow); a socket takes a slot only while the others' missing shares stay free. `room(socket)`,
  `held()`, per-socket `queued(socket)` (recounted at each flush); `push` reports a refusal; a socket the queue's
  sockets lack is refused at once.
- `QuicConnection::send(out, now, held_sockets)`: a path on a held socket gets nothing and changes nothing;
  `held_back()` says something waits. The host writes only for admitted sockets, counts a push refused, and serves a
  held connection whose ACK or pacer deadline has passed after `send_retry_ns` instead of at the same instant (that
  spin stalled the new stalled-socket test).
- Defect fixed: a server's CONNECTION_REFUSED left by socket 0 whatever socket its Initial reached.
- Defect fixed (fuzz-found, present at HEAD): a server whose padded Initial the amplification limit could not carry
  sent nothing, so its owed ACK of 0-RTT reported a timer due that nothing served. It now sends what later spaces owe
  unpadded within the credit, the PTO staying disarmed for the held Initial. The input is kept in the corpus.

## Evidence

- win-debug (MSVC 14.51): all 989 `crd-net-tests` cases (4 new); WSL linux-gcc-debug (GCC 13, -Werror): 794 `[net]`
  cases and the connection corpus CTest.
- Teeth: admission off fails the loop, client and queue tests; the refusal on socket 0 fails the server test; the
  active path's held check removed fails the connection test; without the retry floor the client test stalls.
- linux-clang-fuzz `crd-fuzz-net-quic-connection` with the new 0xF8 op (held sockets; a held send changes no counter or
  packet number): the first run found the stall above; after the fix 481 s, 62042 runs, nothing found (scratch corpus).
- clang-tidy 22.1.8 clean on the 14 changed sources and headers; formatting checked on added lines; Allman.

## Next

preferred_address (RFC 9000 section 9.6) on a server's second socket; interop with ngtcp2's migrating client and
server through `QuicClient::migrate()` in `cerid_quic_peer`; LEO handover awareness.
