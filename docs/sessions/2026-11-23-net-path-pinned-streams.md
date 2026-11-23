# 2026-11-23: NET-PATH, streams pinned to a path

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath)'s Scheduling
and Proof bullets, [S40](../design/network-foundation.md#quic-stream-set)'s Sending bullet, S75's Proof bullet.

CI first: nothing to fix. CI 45084440222 (b119357b) is pending; fuzz host-check 45084570688 dispatched on b119357b
(its fixed matrix has no net-quic-datagram leg).

## What landed

- The operator's follow-up first: over paths the connection fuzzer's server holds two of its client's IDs (RFC 9000's
  minimum), so a move may find none unused. With the waiting move mutated away, the stricter follow check fails 9 of
  83 libFuzzer finds and the committed input 8cfce1bc; with the kept revert mutated away too, the same (the waiting
  move covers that one alone). One find, ddaef1b3, joins `corpus-quic-connection`. The fixed code ran 50741 times clean.
- `QuicStreamSet::pin()`, `unpin()`, `pinned()`, `open_path()`, `stop_path()` and `QuicStreamRoute`: a pinned
  stream's data goes only on its path ID's slot, also while that slot yields its data or is a backup; its control and
  credit frames go on any slot carrying data. A stopped path unpins its streams: lost and unsent data goes anywhere.
- The connection routes every readiness query (`has_data`, `gate_data`, `has_content`, `own_ready`) and writes a
  yielding slot's pinned data (`write_streams`); `m_copies_open`/`copies_allowed` became `m_own_open`/`own_allowed`.
  `channel_link.cpp` maps the new `QuicStreamError::NoPath`.

## Evidence

- `test_quic_connection.cpp`, three `[multipath]` cases: chunks pinned to the slower path ID 1 read in 50 to 51 ms from
  both ends and with it backup, plain ones in 21 ms; path ID 1 dark: the plain stream unaffected, the pinned one waits
  and completes once it carries; a pinned backlog grows the slower path's window, and the faster path is marked
  application-limited within 2 ms (37 packets in a second, where an unrouted query took 360 ms and sent 3222); 160 KiB
  over lossy, duplicating, jittering links stays on path ID 1 until either end abandons it, then completes (4 seeds).
- `test_quic_stream_set.cpp`: refusals, routes, control frames never on a no-data route, lost data resent on its
  slot, a stop unpinning, the slot reopened, a 0-RTT rejection keeping open paths.
- `crd-fuzz-net-quic-stream-set`: slots opened and stopped, pins, routed takes with the routing and exactness oracles,
  every pin checked after a stop; libFuzzer 300 s and 241 s (3.15 M and 2.26 M executions), no artifact; a `pins` seed.
  `crd-fuzz-net-quic-connection` 300 s (37876 executions) clean; the committed corpus replays clean.
- Twelve source mutations, each rebuilt alone and caught (route ignoring the pin, control frames on a no-data route,
  has_frame unrouted, stop keeping pins, own_ready without streams, the yield branch not writing them, abandon or
  commit not telling the set, has_data and gate_data unrouted, the yield branch's mark, pin of an ended stream).
- linux-gcc-debug: 53 CTests pass (`crd-net`, every `crd-check|crd-no-|crd-fuzz-net` guard); linux-gcc-asan and
  linux-gcc-release `[quic-stream-set],[multipath]` pass (116 cases). LLVM 22.1.8 tidy clean on the seven touched
  sources; clang-format applied to added lines only.

## Next

NET-PATH's scheduling: an idle path's RTT kept fresh, PATH_ACK placement; then the fuzzers opening path IDs.
