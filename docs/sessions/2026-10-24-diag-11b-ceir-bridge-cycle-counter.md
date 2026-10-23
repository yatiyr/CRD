# DIAG.11b clause (6): the CEIR bridge timed by the cycle counter, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b). Rules: [AGENTS](../../AGENTS.md). Previous batch:
> [flight recorder record path](2026-10-24-diag-11b-flight-recorder-lock-free-record.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. 40006451399 (head
6a964937) failed only win-asan: four DXR gates and `UNEXPECTED PASSED: registered TP-1`, the WARP read a832934d
repaired after that head; its proof is 40032224637 (head 5066e492), in progress. 40039075298 pending. The other
completed push runs were cancelled with no job. Not waited on, nothing cancelled, no dispatch.

## The finding

The last clause (6) mechanism: the CEIR profiling bridge (`engine/execution/ceir-host/src/plan_perf.cpp`) times every
dispatched instruction. Its hooks take no lock, block nowhere and allocate nothing: the state is a stack-local
`ProfState` of the run. Its cost, by experiment on win-shipping-profile (`diag-overhead.py run --workloads offline
--arms off,full`, 16 processes, 200 frames after 20, 8 threads, affinity FFFF, host idle by `build-bench.py host`),
one build per variant of the bridge:

| Variant | off p50 µs | full p50 µs | full over off (95% interval) |
| --- | ---: | ---: | --- |
| HEAD (two `MonotonicClock::now()` per dispatch) | 371.9 | 2053.6 | +454.60% (+447.59% .. +457.01%) |
| hooks without the clock reads | 373.1 | 467.9 | +24.96% (+24.20% .. +25.92%) |
| no hooks | 328.9 | 331.1 | +0.69% (+0.37% .. +1.16%) |

So the cost is the clock: about 1.6 ms of the 1.7 ms full adds per frame. The no-hooks
build's off arm moved -12% though it does not touch off's path, the layout floor of (1a) again.

## The change

The hooks read `crd::time::CycleCounter` and add ticks per Op class; `profiled_run` reads the monotonic clock and the
counter around the run and converts the ticks to seconds once, so the self-times sum to at most the run's wall time.
A counter that steps back (a non-invariant TSC across cores) adds no ticks; a run shorter than either clock's
resolution leaves every self-time zero. `PlanProfile` and the seam are unchanged.

Cost, the same method, the two sources built and measured alternately:

| Build | off p50 µs | full p50 µs | full over off (95% interval) |
| --- | ---: | ---: | --- |
| cycle counter 1 | 371.8 | 1188.3 | +219.81% (+217.39% .. +222.06%) |
| HEAD 1 | 369.6 | 2043.7 | +453.30% (+451.57% .. +454.84%) |
| cycle counter 2 | 371.7 | 1187.3 | +219.48% (+218.90% .. +220.85%) |
| cycle counter 3 | 372.9 | 1191.0 | +219.41% (+217.75% .. +221.12%) |
| HEAD 2 | 370.4 | 2046.2 | +451.86% (+450.70% .. +453.75%) |
| cycle counter 4 | 371.7 | 1192.1 | +220.30% (+219.63% .. +221.86%) |
| HEAD 3 | 370.0 | 2041.8 | +452.78% (+450.47% .. +455.31%) |

The clock's share fell from about 1.6 ms to about 0.7 ms per frame. What remains is the price of exact parent-pause self-time: a timestamp at every event
boundary, two per dispatch, against a dispatch of a few nanoseconds. One read per dispatch would move the executor's
time between dispatches onto the next op, a change of `PlanProfile`'s meaning, so it is not made. Full mode has no
numeric budget; the number is reported. The Linux tuples' numbers stay the 2026-10-16 board's.

## Evidence

- `tests/execution/ceir-host/test_plan_perf.cpp`, the LOOP case: the self-times sum to at most the run's wall time,
  bracketed outside `profiled_run`. Tooth: ticks read as seconds without the conversion fail it (`209540.0 <=
  0.0000741`); restored, rebuilt and rerun green.
- crd-ceir-host-tests passes on win-debug and linux-gcc-debug (55 cases), its three bridge cases on
  win-shipping-profile; the 12 `crd-diag-overhead-*` CTests pass on win-debug and linux-gcc-debug.

## Not done in this batch

- (1a) the two-binary layout floor is next on the row.
- (l) zstd 1.5.7 and (o) clang-tidy 20 are downloads; the user's own messages in this run approve neither.
