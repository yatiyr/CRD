# DIAG.11b clause (8): a TickClock reader stepped back at the hand-off to the first map

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b), clause (8) and follow-up (q). Contract:
> [`clocks.hpp`](../../engine/foundation/time/include/crd/time/clocks.hpp). Board:
> [tick clock hand-off](../bench/2026-10-26-diag-11b-tick-clock-hand-off.md). Previous session:
> [tick clock inlining](2026-10-26-diag-11b-tick-clock-inlining.md). Rules: [AGENTS](../../AGENTS.md).

## The hosted failure

Complete run 40274818980 (head `1cb54d07`), job windows-shipping (win-shipping), id 121415286412:
`TickClock: no thread's reads step back while any thread re-anchors` failed with `stepped_back.load() == 0` as
`1 == 0` (0.03 s, its own process). The case's four threads each compare a reading with their own previous one, so
it tests the promise the header makes ("it never steps back"), per thread; the test was right and is kept.

## The cause

Reproduced on the i9-14900K at once: win-shipping 8 of 300 fresh processes. An instrumented copy of the case (never
committed) printed every step: all of them were the first mapped read after an unmapped one, 2.3 to 2.5 ms into the
case (just past `kCalibrationNs`), 1 to 37 ns back, the previous reading always on MonotonicClock's 100 ns grid
(`last % 100 == 0`) and the mapped one just under the next grid line. No step was ever seen between two mapped reads,
so not at a re-anchor and not from the counter.

The mechanism, in `unmapped_now` before the fix: the reader that found the calibration span over read its pair
(the counter beside a MonotonicClock reading `n0`), then took the state (Calibrating → Mapping), then stored the map
anchored at `n0`. Meanwhile every other reader in Calibrating or Mapping went on returning fresh MonotonicClock
readings, later than `n0`. QueryPerformanceCounter truncates to 100 ns, so the true time at the anchor's tick lies up
to 100 ns past `n0`; a reader whose last reading landed on a grid line after `n0` read the map a few nanoseconds
under it on its next read. Hosted `ctest -j` makes the window wider (a publisher descheduled between its pair and
its store lets the other readers' readings run on past `n0` by the slope's error of the whole delay).

## The fix (`engine/foundation/time/src/clocks.cpp`)

- The publisher takes the state first (Calibrating → Mapping, sequentially consistent), then reads its pair, then
  reads a new floor and anchors at the later of the two.
- An unmapped read returns a MonotonicClock reading only after raising the floor to it (always a read-modify-write)
  and re-reading the state: if the state is still Calibrating the reading is returned, otherwise the read starts
  over. Whenever the re-read found Calibrating, the raise lies before the publisher's exchange in the sequentially
  consistent order and the publisher's read of the floor after it, so every reading returned is at or below the
  anchor, and a mapped read (the anchor plus a non-negative scaled count) never below it. The argument needs nothing
  about how `rdtsc` or QueryPerformanceCounter are ordered.
- A reader that finds the state at Mapping (the origin's or the first map's publication, one read_pair) waits for it
  instead of returning a reading; so does a reader that meets a re-anchor (odd sequence), as before, now through
  `wait_for_reanchor`: 64 pause turns, then a yield a turn, so a publisher that lost its processor to the waiters gets
  one back. No read waits for the calibration itself.
- The floor is the map's eighth 64-bit word (the line's padding), written only before the first map. The mapped read
  is unchanged: cdb of win-shipping-profile shows the same body, its one new instruction a call on the odd-sequence
  branch.
- Re-anchors needed no change: `reanchor` takes the sequence before its pair and anchors at or above the old map's
  value at its own later tick; the clamp of a count below the anchor to zero covers a reader whose counter read lands
  below it.

## The contract (`clocks.hpp`)

Decided from the header, ADR-0133 and the flight-recorder brief: the clock promises **per-thread monotonic** readings
through the calibration, the hand-off and every re-anchor, and one map for every thread, so different threads agree
as far as the processors' counters do. It does not promise an order agreeing with happens-before across threads:
`rdtsc` is not ordered with memory operations, and the flight recorder's single event order and exact trigger window
come from its claim word, not from timestamps ([brief](2026-10-26-flight-recorder-per-thread-rings-decision.md)).

## VM counters and migration

Per-core counter offsets on the hosted VM were not reproduced and are not claimed ruled out on Azure; the evidence
that the hosted step was this hand-off is the identical assertion on the same lane, one step in a fresh process whose
24 ms window holds the hand-off at 2 ms, and no mapped-phase step in thousands of local runs. Cross-processor
agreement of a mapped counter rests on the invariant-counter bit the host or hypervisor exposes; the clock does not
check it itself, and a per-thread clamp on the hot path was not added unmeasured. A new case moves each reader to the
next allowed processor every 64 reads (`SetThreadAffinityMask`, `sched_setaffinity`) through the hand-off and three
re-anchors; it stayed clean here (below) and runs on every hosted lane.

## Tests and teeth (`tests/foundation/time/test_clocks.cpp`)

- The hosted case kept its name and check; with asserts it starts the clock over, and every reader now reads until
  the first map is published (at most 10 s) and the case checks the counter is mapped exactly where it may be. A
  first run of the migration case failed that check twice in 300 runs (`false == true`, 110 moves against about 400):
  readers waiting for their next processor had ended the 24 ms before any read was 2 ms past the origin; reading
  until the map is published repaired the test, not the clock (800 runs clean afterwards).
- New: `... while threads move between processors` (the migration case) and, in builds with asserts,
  `... across many hand-offs to the first map`: 400 rounds of a reset with no reader running and four readers through
  the calibration and past the first map.
- Teeth 1, readers returning MonotonicClock while the state is Mapping (the fault's shape): win-shipping 11 of 300
  processes of the hosted case; win-debug the hand-off case failed 10 of 10 runs (at 200 rounds then, 1 to 8 steps a
  run, 3.9 on average over 20 runs, so the case was raised to 400 rounds). Restored and rebuilt.
- Teeth 2, the floor's re-read of the state removed: clean in 300 + 10 runs. With the publisher taking the state
  before its pair a reading returned before the exchange is also below `n0` whenever QueryPerformanceCounter is
  consistent across processors, as it is here; the re-read closes the case where it is not and cannot be provoked on
  this host. Restored and rebuilt.
- With the fix: win-shipping 1,000 of 1,000 of the hosted case, 800 of the migration case; 720 + 720 under twelve
  concurrent copies (`xargs -P 12`); win-debug 300 + 300 + 20 of the hand-off case and 20 of `[clocks]`; win-asan in
  vcvars 300 + 300 + 20 and the whole suite; linux-gcc-debug through `scripts/wsl-lane.sh`, the six TickClock CTests
  20 times each. LLVM-20 tidy clean on the three files.

## Cost

[Board](../bench/2026-10-26-diag-11b-tick-clock-hand-off.md), win-shipping-profile, four threads, affinity `5555`,
interleaved, 10 processes an arm: jobs full against full-system-clock −2.49% (−4.63 .. −1.76), within the 0%
budget (−2.22% on the inlining board); a kept site 70.2 ns, a zone 16.5 ns over off, as before.

Needs CI: the three cross-thread cases on every hosted lane, windows-shipping included.
