# 2026-10-30 — SIMD-DISPATCH: win-asan's first run under its bound and the TickClock case's span

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (CI first; Left: the guard's first green run on every lane).
Before: [the batched transforms' levers](2026-10-30-simd-dispatch-jobs-fft-batch-picks.md).

## CI first

- Push run 40976392370 (c28aaa54) completed. Its eight linux-gcc and linux-clang-tsan failures are the
  test_cholesky.cpp:340 double promotion fixed at bc2bdb83. Its win-asan job failed one case, below.
- Push run 40993784639 (87695de9) is still running; four of its jobs have failed (linux-gcc-debug, -public-checks,
  -asan, -debug-scalar), all on `-Werror=range-loop-construct` at test_svd.cpp:1636 (a `const auto` copy of each
  `std::pair` in the non-finite svd case, 0eca4d88). The loop now binds `const auto&`.
- Nothing was cancelled or dispatched. Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run;
  none has run since. Nothing flips.

## win-asan under its bound

The first run at three cases a time ([the bound](2026-10-29-simd-dispatch-win-asan-bound.md)) ran all 8,123 cases
in 9,137 s (5 h 34 min serially at e496d16c), on an AMD EPYC 9V74 with 4 logical CPUs and 16.0 GiB, at least 11.8
GiB available throughout (13.1 before), commit peak 9.5 GiB. `crd-check-isa-isolation` passed in 295 s. The one
failure was `TickClock: reads stay on MonotonicClock's timeline through calibration and re-anchors`. The five ARM64
lanes passed; linux-gcc-arm64-asan took 1 h 25 min, so it needs no bound.

## The TickClock case

It failed `counter_mapped() == counter_usable()` (false == true), worst distance 0 ns, as win-shipping did in run
40869334591. [The 2026-10-29 note](2026-10-29-tick-clock-counter-behind.md) read that as a counter that ran
backwards, and d3b0daf1 settles such a counter in `Backwards`, where `counter_usable()` is false. This run already
had that commit, so the state was neither Mapped nor Backwards: the clock was still calibrating when the case
checked it, and the counter had not run backwards.

`bracket_reads` measured its 34 ms span from a MonotonicClock read taken before the first TickClock read. That first
read in a process runs the lazy CPU selection, and the selection pins the thread to every processor in turn. On a
runner with three cases at a time, that read, or one preemption inside the first 2 ms, can take the whole span. The
loop then ends after reads that all came within the calibration. Now the span starts at the end of the first
reading and ends at the start of a reading. So the last read starts at least 34 ms after the clock's first reading,
past the calibration, whatever the scheduler does. The failure message now prints the read count.

`Backwards` stays: no map of a counter found behind its calibration's start can keep a thread from stepping back.
The planted-origin case no longer names a runner as its cause.

## Checks

crd-time-tests and crd-hesap-dense-tests built on win-debug. The `[tick]` cases (7) and the non-finite svd case
passed. Allman braces, the plan and repository checks passed. The format findings on the two files are at or below
HEAD's counts.
