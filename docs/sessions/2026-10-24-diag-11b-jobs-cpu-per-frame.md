# DIAG.11b clause (4): jobs judged on the CPU per frame, left-out events counted in stripes, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133 ID-5](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions). Rules:
> [AGENTS](../../AGENTS.md). Previous batch: [CEIR bridge](2026-10-24-diag-11b-ceir-bridge-cycle-counter.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. 40006451399 (head
`6a964937`) failed only win-asan's gate: four DX12 ray-tracing tests and an unexpected pass of the fifth TP-1
registration, the failure `a832934d` repairs; its hosted proof is a later run's win-asan job. 40039075298,
40029933433, 40029693899, 40022015924, 40020783027, 40018724265, 40014687187 and 40014471031 were cancelled with no
job. 40032224637 was running and 40041122184 pending; not waited on.

## The measurand

`crd-diag-overhead` records the CPU every thread of the process used over the timed frames (`cpu`: `read`, `unit`,
`timed`): `QueryProcessCycleTime` cycles on Windows (`GetProcessTimes` advances by whole clock ticks), and
`CLOCK_PROCESS_CPUTIME_ID` nanoseconds on POSIX. `--check` fails when it was not read, so the 12 `crd-diag-overhead-*`
CTests hold it on every lane. `diag-overhead.py` compares the CPU per frame like the frame time (paired ratios, seeded
bootstrap), judges each budget on both, and the worse verdict decides; a record without CPU time is an instrument
failure. On this host the unit is the 3.2 GHz TSC: the single-threaded `alloc` reads 3.21 cycles per nanosecond of
frame.

## Why jobs was unresolved

The pool's threads stay busy for the whole frame (CPU over frame time: 7.9 threads with an eight-thread pool, 3.95
with four), so the CPU per frame carries the frame time's spread. That spread is between processes, not between
modes: the per-process medians of one binary in one mode (12 processes, win-shipping-profile, off) spread by

| Pool threads (main included) | Affinity | Spread (standard deviation of the process medians) |
| ---: | --- | ---: |
| 8 | FFFF (8 cores, 16 logical) | 7.6% |
| 4 | FFFF | 2.0% |
| 4 | 5555 (8 cores, one logical each) | 2.2%, 2.2% |
| 4 | 55 (4 cores, one logical each) | 1.8% |
| 3 | 55 | 2.4% |

Eight threads that never rest on eight cores leave placement to decide the frame. The boards below run a four-thread
pool on 5555 (`--threads` now defaults to 4) and 96 to 288 processes per arm (0.42 s each), so an interval is about
±0.5 to ±1%.

## Boards

`diag-overhead.py run --workloads jobs --iterations 200 --warmup 20 --threads 4 --affinity 5555`, host idle
(`build-bench.py host`). Cost of the arm over its reference, frame time and CPU per frame, 95% intervals.

| Tuple, source | Processes | Disabled (off vs compiled-out) | Basic (basic vs off) | Full over off |
| --- | ---: | --- | --- | --- |
| win-shipping-profile / win-shipping, HEAD | 160 | -1.57% (-2.40 .. -0.70), CPU -1.29% (-1.98 .. -0.30): within | -0.67% (-1.27 .. -0.16), CPU -1.11% (-1.79 .. -0.73): within | +50.23%, CPU +49.85% |
| the same, stripes | 160 | +0.06% (-0.87 .. +0.61), CPU +0.62% (-0.29 .. +1.31): unresolved | -1.55% (-2.33 .. -0.71), CPU -1.81% (-2.65 .. -0.94): within | +48.26%, CPU +48.14% |
| linux-gcc-shipping (profiling compiled out), HEAD | 96 | not measured (5) | -0.71% (-1.51 .. -0.22), CPU -0.84% (-4.25 .. -0.63): within | +15.81%, CPU +12.85% |
| the same, stripes | 288 | not measured (5) | -1.59% (-2.35 .. -1.11), CPU -3.13% (-4.52 .. -1.07): within | |
| linux-gcc-relwithdebinfo, stripes | 288, 288 | not measured (5) | +0.11% (-0.45 .. +0.70), CPU +0.08%; +0.66% (+0.07 .. +1.07), CPU +0.20%: within | +39.46%, CPU +37.27% (HEAD) |

Binaries (SHA-256 prefixes): Windows HEAD `b239cd659607` against `a76cb98afb86`; linux-gcc-shipping HEAD
`dc4e1ee794a2`; linux-gcc-relwithdebinfo HEAD `004108dfbe6a`, without the count `1832549a6300`, stripes `ba17b35a24e6`.

Disabled: the off arm moved from -1.57% to +0.06% between two builds whose only change, the recorder's counter, off
never runs. That is the two-binary floor (1a), and the CPU interval's upper end (+1.31%) is inside it; the exact count
of 2026-10-23 (no statistics gate per jobs frame) stays the disabled verdict's basis until (1a) holds.

## The basic cost on linux-gcc-relwithdebinfo, attributed and repaired

At HEAD four boards read basic at +2.12%, +0.40%, +1.21% and +1.95% (288 processes: +1.45 .. +2.35): over 2% in
part, unresolved. By experiment (not committed, restored), `FlightRecorder::admit` without its
`filtered.fetch_add`, alternated with HEAD at 288 processes: HEAD +1.84% (+1.32 .. +2.31) and +1.73% (+0.70 .. +2.19),
without the count +0.64% (+0.03 .. +1.12) and +0.31% (-0.04 .. +0.75). Every thread wrote that one counter at every
left-out site, on the line of `keep_from`, which every site reads.

Repair (`engine/foundation/perf/src/flight_recorder.cpp`): left-out events are counted in 16 stripes of a cache line
each (`CacheLinePadded`), a thread always in the same stripe; `total_filtered()` sums them. Alternated with HEAD at 288
processes: HEAD +1.53% (+0.98 .. +2.12) and +1.98% (+1.38 .. +2.58), stripes +0.11% and +0.66% (rows above). The
counts stay exact: the harness's basic CTests require every site and frame event counted as left out, with four
threads; crd-perf-tests passes on win-debug (294 cases), win-shipping and linux-gcc-debug, and its 39 flight cases on
linux-clang-tsan with no report.

## Teeth

- `read_process_cpu` answering false (a warning-clean tooth): the 12 `crd-diag-overhead-*` CTests fail on win-debug;
  restored, rebuilt, 14 of 14 pass.
- Four mutations of `diag-overhead.py`, each fails `test-diag-overhead.py` (14 cases): the frame time alone deciding,
  a record without CPU time accepted, the CPU comparison reading the frame time, `worse` preferring within.

## Not done in this batch

- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads; each waits for the user's own approval in the
  conversation.
- Found here, queued first: full on jobs costs +48% to +50% over off on Windows, +39% on linux-gcc-relwithdebinfo and
  +16% on linux-gcc-shipping (recorder only) with a four-thread pool, where the eight-thread boards read about +4%; the
  recording word every kept event claims by compare-exchange is one line every thread writes.
