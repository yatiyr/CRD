# 2026-10-25 — DIAG.11b clause (8): the overhead workloads under WPR sampling, judged again

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Contract: [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b); session:
> [2026-10-25](../sessions/2026-10-25-diag-11b-external-sampling.md); owner row:
> [DIAG.11b](../ROADMAP.md#slice-diag.11b). Earlier read: [2026-10-24](../sessions/2026-10-24-diag-11b-sampling-attribution.md).

- **Machine/config:** Intel i9-14900K (8 performance cores, 32 logical CPUs), 63.7 GiB, Windows 11 10.0.26200, MSVC
  19.51 (toolset 14.51.36231). Windows Performance Toolkit 11.7 (`wpr -start CPU -filemode`, 1 ms sampled profile with
  stacks; `xperf -a dumper -stacktimeshifting`). Visual Studio closed (the user's elevated
  `scripts\elevated-captures.bat`, 2026-10-25 12:08Z).
- **Binaries:** `build/win-shipping-profile/tests/diag-overhead/crd-diag-overhead.exe` (SHA-256 `58a639e9…`, linked
  2026-10-24 06:20Z) and the compiled-out `build/win-shipping/...` (SHA-256 `502af136…`, linked 2026-10-24 12:30Z),
  both still the files in the trees when this was read. They were not built from one source: the baseline holds the
  rendering workload's device-validation option (`--validation`, commit `d7836fbb`, 10:26Z) and the profiling binary
  does not (0 of the 7 `validation` strings; the harness code they share runs no rendering).
- **Harness:** [`scripts/sample-overhead-wpr.py`](../../scripts/sample-overhead-wpr.py) `analyze` (no elevation) of
  `build/diag-overhead-wpr/20261025T120843Z`; each arm two processes, interleaved and rotated; alloc 40,000, jobs
  9,000, offline 10,000 and io 6,000 frames per process; the harness's default eight-thread jobs pool, every process on
  affinity `FFFF` (16 logical processors of the eight performance cores). Thread-shape boards:
  [`scripts/diag-overhead.py`](../../scripts/diag-overhead.py) `run`.

## Verdicts, as recorded and re-analysed

The recorded method compared the share estimate (1 - share_off) / (1 - share_arm) - 1 with the median frame ratio.
The method of 2026-10-25 (the session's three repairs) compares the sampled CPU per frame with the harness's CPU per
frame (or the frame time where frames run on one thread), places the added CPU in the diagnostic frames, their call
sites and elsewhere, and judges a two-binary row only on the out-of-line diagnostic code off runs.

| Row | Measured (frame time) | As recorded | Re-analysed: sampled CPU | Diagnostic frames | Their call sites | Elsewhere | Re-analysed verdict |
|---|---:|---|---:|---:|---:|---:|---|
| alloc off over compiled-out | +10.0% | -0.1%, DISAGREES | +8.4% | -0.1% | | +8.5% (`TlsfAllocator::try_allocate` +7.7%) | AGREES |
| alloc basic over off | -0.1% | +0.2%, AGREES | +0.4% | +0.2% | +0.1% | +0.1% | AGREES |
| alloc full over off | +2.1% | +0.7%, AGREES | +3.2% | +0.7% | +1.7% | +0.7% | AGREES |
| jobs off over compiled-out | +9.6% | +0.0%, DISAGREES | +8.9% | +0.0% | | +8.9% (`FiberPool::acquire` +11.8%) | AGREES (magnitude not compared: 8.3 threads busy, no CPU measurand kept) |
| jobs basic over off | -1.8% | +0.1%, AGREES | | | | | UNRESOLVED: basic's two processes 393.1 and 470.5 µs (19.7%) |
| jobs full over off | -14.0% | +14.3%, DISAGREES | -7.5% | +11.6% | +8.6% (`sum_slice`: the sites' payloads) | -27.7% (`FiberPool::acquire` -29.8%) | DISAGREES: the frame is the pool's contention |
| offline off over compiled-out | -6.1% | -0.0%, DISAGREES | -4.9% | -0.0% | | -4.9% (`run_seq` -5.1%) | AGREES |
| offline basic over off | -0.2% | +0.1%, AGREES | -0.8% | +0.1% | +0.1% | -1.0% | AGREES |
| offline full over off | +258.2% | +134.5%, DISAGREES | +252.6% | +202.3% | +44.1% (`run_seq`) | +6.3% | AGREES |
| io off over compiled-out | +1.2% | -0.0%, AGREES | +0.9% | +0.0% | | +0.9% | AGREES (1.9 threads busy) |
| io basic over off | -0.5% | +0.0%, AGREES | +0.0% | +0.0% | +0.0% | -0.1% | AGREES (1.9 threads busy) |
| io full over off | +1.4% | +0.5%, AGREES | +1.0% | +0.5% | +0.9% | -0.5% | AGREES (1.9 threads busy) |

Percentages of a row are of the cheaper cell's sampled CPU per frame. Sampled CPU per frame (off): alloc 0.089 ms,
jobs 3.619 ms, offline 0.349 ms, io 1.200 ms; threads busy per frame (sampled CPU over the median frame): alloc and
offline 1.0, io 1.9, jobs 8.2 to 8.9.

## Off over compiled-out: where the gap sits

Samples per 1,000 frames, deciding function, compiled-out against off:

| Workload | Total | Largest moves |
|---|---|---|
| alloc | 81.9 → 88.8 | `TlsfAllocator::try_allocate` 39.8 → 46.1 (the statistics gate is force-inlined there); `deallocate` 24.4 → 25.1 |
| jobs | 3,322.5 → 3,619.0 | `FiberPool::acquire` 1,538.6 → 1,930.9; `Scheduler::try_pop` 280.4 → 240.1; `Scheduler::push` 100.4 → 79.1 |
| offline | 366.9 → 348.9 | `run_seq` 274.4 → 255.6; `Array<i64>::resize` 33.2 → 30.5 |

Out-of-line diagnostic samples per 1,000 frames: off 0.0 to 0.3 in every workload, compiled-out 0.0 to 0.3 (`perf`
helpers the harness itself calls). Teeth on this trace: counting `TlsfAllocator::try_allocate` as diagnostic makes
alloc's row DISAGREE ("off runs its out-of-line diagnostic code (+7.7%"), counting `FiberPool::acquire` makes jobs'
row DISAGREE (+11.8%).

## Offline full: the CEIR bridge's cost

Samples per 1,000 frames, off 348.9 → full 1,230.2 (+252.6%):

| Where | Off | Full | Of off's CPU |
|---|---:|---:|---:|
| `adapter_pre` (the bridge's per-op hook; its inlined `rdtsc` read) | 0.0 | 406.1 | +116.4% |
| `adapter_post` | 0.0 | 298.3 | +85.5% |
| `profiled_run`, the recorder and the profiler | 0.0 | 1.2 | +0.3% |
| `run_seq` (the executor that calls both hooks per dispatched op) | 255.6 | 408.7 | +43.9% |
| everything else | 93.3 | 115.9 | +6.5% |

`run_seq`'s added samples are at its call sites (cdb on the trace's PDB): the reload of the loop index from its stack
slot and of the sequence after `call rax` (+0x9ED..+0xA0F, 18.9 per 1,000 frames at +0xA0F) and the dependent loads
of the next instruction at the loop head (+0x1EA, 4.5 → 41.0). The cycle-counter read has no frame of its own
(`CycleCounter::now` is inlined into the hooks).

## Jobs: the trace's thread shape against the boards' shape

`diag-overhead.py run --workloads jobs --processes 24 --iterations 2000 --warmup 50`, the same two binaries, run
after the trace (light load):

| Shape | Arm | p50 µs | Cost (95% interval) | CPU per frame | CPU cost (95% interval) | Verdict |
|---|---|---:|---|---:|---|---|
| 8 threads, `FFFF` (the trace's) | compiled-out | 404.0 | reference | 10.68 Mcycles | | |
| | off | 416.4 | +1.63% (-6.43 .. +7.22) | 10.99 Mcycles | +3.23% (-6.23 .. +5.94) | unresolved (1%) |
| | basic | 397.4 | -1.52% (-8.87 .. +2.34) | 10.25 Mcycles | -6.04% (-11.63 .. -0.33) | unresolved (2%) |
| | full | 380.3 | -5.82% (-11.79 .. -1.69) | 9.74 Mcycles | -8.25% (-14.06 .. -5.13) | reported |
| 4 threads, `5555` (the boards') | compiled-out | 357.1 | reference | 4.52 Mcycles | | |
| | off | 351.9 | -1.68% (-2.21 .. -0.08) | 4.45 Mcycles | -1.71% (-2.39 .. -0.20) | within (1%) |
| | basic | 358.5 | +1.69% (+0.95 .. +2.66) | 4.53 Mcycles | +1.72% (+0.76 .. +2.52) | unresolved (2%) |
| | full | 481.5 | +37.12% (+35.77 .. +38.40) | 6.10 Mcycles | +37.04% (+35.80 .. +37.98) | reported |

At eight threads full is cheaper than off in the harness's own CPU as well; at the boards' four it costs +37% on both
measurands. This 24-process board does not replace the 96 to 288-process budget boards of the row; basic's upper
end here (+2.66%) is a 24-process interval.

## Verdict

Of the six rows recorded as DISAGREES, three were the method (the two-binary rows), one the estimator (offline
full), one is unresolved (jobs basic, its processes disagree) and one is a real effect of the recording's shape (jobs
full: off's eight workers spend 37% of their CPU on the fiber pool's free-list compare-exchange, which full's per-job
work relieves). No diagnostic cost is hidden. The repaired `record` runs the boards' shape and keeps the CPU
measurand; its recording is the user's next elevated step.
