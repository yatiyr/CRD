# 2026-11-23: NET-PATH, redundant datagrams

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S41](../design/network-foundation.md#quic-datagrams)'s
"Redundant datagrams" bullet and [S78](../design/network-foundation.md#multipath)'s Scheduling and Proof bullets.

CI first: nothing to fix. CI 45044166180's two reds (win-tidy, the Ninja 403) were fixed at HEAD before; fuzz
host-check 45051033311 passed; 45063142671 and 45057262362 are still queued.

## What landed

- `QuicDatagramSend::redundant` (latency-critical input and voice): a datagram goes once on every path slot carrying
  data (`copy_paths()`: a data path neither held dark nor held for backup). The first slot to write it sends it as
  any datagram; the others send copies, a slot that yields its data under `LowestRtt` too (`m_copies_open`,
  `copies_allowed()`), in its own window and pacer, which a waiting copy arms. Such a slot never sends a first copy
  or stream data.
- `QuicDatagrams` keeps the send ring in order, with each datagram's sending slots and per-slot cursors.
  `redundant_count` records hold each redundant datagram until every copy settled. Copies stop at the first
  acknowledgement, at `max_age_ns`, for a slot that leaves the paths carrying data, and for a path grown too small.
  `queue()` gives owed copies up before it answers Full, at `send_count` with outcomes too: a datagram whose copies
  were all lost then reports Lost (fix 03282d69).
- One report per datagram: Acked at the first acknowledged copy, Lost once every copy was lost and none can go. A plain
  datagram's ticket stays its id, so QMux and the single-path callers are unchanged.
- The connection fuzzer's follow oracle: a 102-byte input (now in the corpus) failed it on b818c4b5 too. The server's
  validation of the client's address failed after the client's last packet: both challenges were lost before the
  3-PTO timer, and the server reverted to its last validated address (RFC 9000 sections 8.2.4 and 9.3.2). The engine
  recovers on the client's next packet, which the oracle now sends and requires to bring the server back.

## Evidence

- `test_quic_datagram.cpp`: three `[multipath]` cases. `test_quic_connection.cpp`: one case over two paths 60 ms
  apart. In it each datagram arrives once per path, a stream rides path ID 0 alone (DataWatch), nothing goes to a
  backup path, and with path ID 0 dark each datagram arrives over path ID 1 within its one-way delay.
- 11 source mutations, each rebuilt alone and each caught. One that left `give_up_copies` unguarded hung a run; it
  was replaced and the process was killed.
- linux-gcc-debug (WSL): 49 CTests pass, including `crd-net-tests` and every `crd-check|crd-no-|crd-fuzz-net` guard.
  linux-gcc-release (LTO) and linux-gcc-asan `[quic-datagram],[multipath],[qmux]` pass.
- `crd-fuzz-net-quic-datagram` now checks against a model of the send ring (new seed `redundant`). linux-clang-fuzz
  ran 240 s, then 120 s with `-fork=2`: 161934 executions and no fault. `crd-fuzz-net-quic-connection` ran 300 s with
  `-fork=2`: 37538 executions and only the follow-oracle input, since fixed. All 1900 inputs replay clean.
- Pinned clang-tidy 22.1.8 is clean on every touched source; clang-format 20 was applied to added lines only.

## Next

NET-PATH's scheduling: streams or datagram flows pinned to a path (`QuicDatagramSend` takes the pin), an idle path's
RTT kept fresh, PATH_ACK placement; then the fuzzers opening path IDs.
