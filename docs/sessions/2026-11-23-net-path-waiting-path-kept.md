# 2026-11-23: NET-PATH, a path a move waits on survives a full 4-tuple table

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S75](../design/network-foundation.md#migration), "IDs for
waiting paths". RFC 9000 sections 9.5 and 9.6.2 read again for it.

## The defect

libFuzzer's `crash-bb4b5a5c` (the last batch's follow-up) failed the follow oracle's `path_failures != 0U` on HEAD.
A trace showed the cause. The client sent its stream from its socket 2 to the server's preferred address. The server
was validating that path while the move waited (`m_deferred_move`), and its 4-tuple table for path ID 0 was full.
The client then probed from socket 0, and `take_path()` gave up the least recently used spare path. That was the move
target: it tied with an idle validated path on `last_ns`, and the lower slot won the tie. The move and its validation
were gone, with no failure recorded. The server neither followed nor validated until the client's next packet. The
same eviction had already used up three of the client's IDs earlier in the run.

## The fix

`QuicPaths::least_recent(kept)` skips `kept` as well as the active path and the fallback, so a full table always has
a path to give up. `take_path()` keeps the path a move waits on: the handshake slot's deferred move, or else the
slot's `waiting` path, which waits for a peer ID (section 9.5). A newcomer gives up another path.

## Evidence

- New raw-client case in `test_quic_connection.cpp`. A move to b waits for an ID, probes from three more addresses
  overflow the table, b stays validating, and the ID's arrival moves the server there. `test_quic_path.cpp` checks
  `least_recent(kept)`.
- Teeth: without the kept path the case fails 3 checks (b gone, no move, no migration).
- The input `bb4b5a5c...` is in `corpus-quic-connection` and replays clean, which covers the deferred-move branch.
- linux-gcc-debug: crd-net-tests passes; the connection, packet, cids and recovery corpora replay clean; libFuzzer ran
  on the fixed connection target.
- CI: win-asan in 45100344604 failed `Provider: one verifier and one key serve several threads` a third time. Its
  register entry now records this sighting: a stall, with two threads refused after 12.7 s and a second signing
  succeeding at once. The other three reds in that run are the LTO link fixed in aeb18868.

## Next

- PATH_ACK placement (S78 Scheduling).
