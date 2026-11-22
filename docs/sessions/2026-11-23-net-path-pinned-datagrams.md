# 2026-11-23: NET-PATH, datagrams pinned to a path

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath)'s Scheduling
and Proof bullets.

CI first: nothing to fix. Fuzz host-checks 45078830552 (e76f25a0) and 45073088098 (f32832fd) and CI 45073010169 and
45078813303 are still queued; no job of them has failed.

## What landed

- `QuicDatagramSend::pinned` with `path_id`: a flow kept on one path. `queue()` takes a path ID open in a slot
  (`QuicDatagrams::open_path()` from `commit_path`, slot 0 holding path ID 0 from the start), refuses one open nowhere
  (`NoPath`) and a redundant pin (`Unsupported`). Only that slot writes it; other slots' cursors pass it by.
- It goes even while its path yields its data under LowestRtt or is the peer's backup, inside that path's window and
  pacer. A yielding slot with its own work waiting (a copy or a pinned datagram) is now held as data (`on_limited`,
  pacer armed) and marked application-limited only with nothing of its own waiting.
- Its path stopping first (`begin_abandon` calls `stop_path()`) drops it where it stands: Lost with outcomes, counted
  `unrouted`. The ring frees in order, so plain datagrams other paths sent wait behind an unsent pinned head; when
  queue() needs that room the head gives way (`give_way`, Lost, `unrouted`), as owed copies do. Pinned ones with
  nothing sent behind them never give way: the application waits.
- A datagram grown too large for the path is dropped where it stands (`drop_unsent`), so an entry waiting before it
  (pinned elsewhere, or owing copies) is no longer popped as Expired.
- `channel_link.cpp` and `http3.cpp` map the new `NoPath` (they never pin).

## Evidence

- `test_quic_connection.cpp`: three `[multipath]` cases. Path ID 1 is 30 ms slower each way and yields its data (a stream
  beside it rides path ID 0 alone, DataWatch); each datagram pinned to it arrives once in 50 to 51 ms, from either end
  and with path ID 1 marked backup, path ID 0's in 21 ms; a pin to path ID 2 is refused; three waiting when the client
  abandons path ID 1 are Lost and never sent. A backlog pinned to the yielding path grows its window from 12000 to
  above 48000 bytes in a second and never marks its delivery rate application-limited. A third case: path ID 1 down,
  one pinned datagram lost into it, the next waiting while it is held dark; 38 plain ones each arrive on path ID 0 in
  21 ms, none refused Full, the waiting one giving way once (unrouted 1, path ID 1 never abandoned).
- `test_quic_datagram.cpp`: three cases (the give-way by count with a pinned head, pinned ones alone refused Full by
  count and by bytes, an unsent redundant one dropped as too large once with its record freed; every refusal, a
  yielding slot writing only its own pin, a pin behind a plain one it passes by, a stop between plain ones, the slot
  reused under a new path ID, the oversized drop with a pinned one before it, expiry, a rejected 0-RTT; both outcome
  modes); the "path grown too small" section now keeps the owed copy.
- `crd-fuzz-net-quic-datagram`: operations from 0xF0 open or stop a slot, and queue() pins once one did; the model
  checks NoPath and Unsupported, the writing slot, the give-way, the unrouted count and the drain on every slot.
  libFuzzer ran 260 s and, after the give-way, 240 s more (133383 and 120282 executions, no artifact; 860 scratch
  inputs, not committed). Two seeds (`pinned`, `pinned-gives-way`) join the committed corpus.
- Twelve source mutations, each rebuilt alone and caught by the unit or connection cases (the gate's old app-limited
  branch, a pin writable anywhere, the fast path ignoring pins, stop keeping pins, queue ignoring open slots, the drop's
  unreported count, abandon without stop, commit without open, the cursor passing its own pin, the old oversized
  prefix pop, the give-way never taken, and taken for pinned ones alone); eight of them also by the fuzz replay of
  the scratch inputs, three of the four tried by the committed corpus alone.
- linux-gcc-debug: 53 CTests pass (`crd-net`, every `crd-check|crd-no-|crd-fuzz-net` guard). linux-gcc-release
  and linux-gcc-asan `[quic-datagram],[multipath]` pass. Pinned clang-tidy
  22.1.8 clean on the seven touched sources; clang-format 20 applied to added lines only.

## Next

NET-PATH's scheduling: streams pinned to a path (the S78 rule: their data goes on any path once the path stops), an
idle path's RTT kept fresh, PATH_ACK placement; then the fuzzers opening path IDs.
