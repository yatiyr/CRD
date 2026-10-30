# 2026-10-29 — TickClock: a counter found behind its calibration's start

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (CI first); the clock is DIAG.11b clause (8)'s.

**Correction (2026-10-30):** the case failed again with this fix in place, so the counter had not run backwards.
The case's span had ended while the clock was still calibrating
([the span](2026-10-30-simd-dispatch-tick-clock-span-win-asan-bound-run.md#the-tickclock-case)).

## CI first

- Push run 40869334591 (e496d16c), win-shipping (job 123441695383): `TickClock: reads stay on MonotonicClock's
  timeline through calibration and re-anchors` failed `counter_mapped() == counter_usable()` (false == true), worst
  distance 0 ns. New; every other completed failure was traced in earlier batches.
- Scheduled run 40849112570 and push run 40869334591 are still running; nothing was cancelled. DIAG.11c and DIAG.12
  stay Needs CI (no green complete-tier run).

## Why the counter was never mapped

Each case runs in its own process (the log's `Filters:` line), and shipping has no `test_reset`, so the clock started
Unstarted. Worst 0 ns over 34 ms of reads means every read was a MonotonicClock reading: a mapped read lands off
QueryPerformanceCounter's 100 ns grid. So the state never reached Mapped:

- Unstarted and Mapping: impossible (reads were returned; one thread).
- System: needs `prefer_system()` (no caller in crd-time-tests) or a non-invariant counter, but `counter_usable()`
  reads the same function-local CPU selection and returned true.
- Calibrating: the only path left. After kCalibrationNs the first-map attempt found `measured_slope` 0, and with
  MonotonicClock 2 ms past the origin that means the counter stood at or below the origin's reading. The runner's
  counter ran backwards (its processors' TSCs disagree although CPUID calls them invariant).

The clock stayed correct for readers, but it re-attempted the map, a CAS and eight MonotonicClock reads, on every
read for good, and `counter_usable()` reported CPUID's promise, not what the calibration saw.

## Fix

- A calibration that finds the counter behind its start settles in a new state, `Backwards`: reads are
  MonotonicClock's for good, `warm()` returns, `prefer_system()` answers true, and `counter_usable()` is false.
  A map of such a counter could step a thread back, so it is never mapped.
- Test seam `TickClock::test_calibrate_from(origin)` (asserts builds only). The new case plants an origin past every
  reading and checks the settled state, exact MonotonicClock reads with zero slack, and that a fresh calibration maps
  again where CPUID allows it.

## Checks

- crd-time-tests built on win-debug; its `[tick]` cases pass (7 cases). Allman braces, the plan and the repository
  checks pass; format findings on the added lines follow the file's existing aligned style.
