# DIAG.11b clause (1): the statistics gate's disabled cost on realistic frames, 2026-10-23

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133 ID-5](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions). Rules:
> [AGENTS](../../AGENTS.md). Earlier board: [2026-10-19 allocator statistics](../bench/2026-10-19-diag-11b-allocator-statistics.md).

## CI read at the start

`gh run list --branch main --limit 15` (2026-10-23, after `a832934d`): no completed run has a failure without a fix on
main. 40014687187 and 40014471031 were replaced while pending (no job). The red nightlies 39923363836 and 39746853196
are the three failures repaired earlier today ([session](2026-10-23-scheduled-runs-thread-probe-reap-race.md)), waiting
for hosted evidence. 40006451399 and 40018724265 were running; not waited on.

## The decision this batch applies

The user's decision of 2026-10-19: stop reducing the `alloc` microbenchmark; keep its result as the worst case and
judge clause (1)'s 1% disabled budget on a realistic workload, repairing only if that is over 1%. The two-binary
timing pair cannot give that verdict: on offline it moved -11.0% to +0.2% between builds that do not touch offline's
path. So the verdict is a bound from an exact count, which that floor does not reach.

## Method

1. Count. An experiment (not committed, restored and rebuilt): a relaxed global counter in `MemoryStats::on_allocate`,
   `on_deallocate` and `on_resize` under `CRD_MEMORY_STATS_ON_REQUEST` (every gate execution), one in
   `StatisticsGate::admit_first_allocation`, and the totals printed at exit. win-shipping-profile, `--mode off`, 10
   warmup frames, 100 and 300 timed frames; per frame = the difference over 200. Two runs gave identical totals.
2. Cost per gate. The 2026-10-19 gate-against-no-gate pair on `alloc`: +4.98% (+3.53% .. +5.76%) of an 82.2 µs frame
   over its 8,192 gate executions: 0.50 ns, upper 0.58 ns. Both binaries run the same `TlsfAllocator` code that the
   offline workload's arena reaches, and in the off mode the gate's branches never change direction.
3. Bound. Cost per frame ≤ executions × 0.58 ns, against the smallest off-arm offline frame of the boards (330.3 µs).

## Counts (win-shipping-profile, off)

| Workload | Gate executions per frame | First-allocation exchanges per frame |
| --- | ---: | ---: |
| alloc | 8,192 (4,096 allocations and 4,096 frees: the probe counts what the workload does) | 0 |
| offline | 1,008 | 3 (the frame's own arena and contexts) |
| jobs | 0 (2 in the whole process) | 0 |

## Verdict

| Workload | Gate cost per frame | Share of the frame | Budget 1% |
| --- | --- | --- | --- |
| offline | 0.50 µs, upper 0.58 µs | 0.15%, upper 0.18% | within |
| jobs | none | 0% | within (the gate; jobs' unresolved timing verdict is clause (4)'s measurand) |
| alloc (microbenchmark, worst case) | 4.1 µs | +4.70% .. +4.98% measured | over, reported, not judged |

Margins: offline reaches 1% only at 3.3 ns per gate, 5.6 times the upper estimate (about 19 cycles at 5.7 GHz for a
load, two compares and two predicted branches). Adding to the per-gate estimate the largest gap the same pair showed on
offline (3.5 points) gives 0.93 ns and 0.28%. The three exchanges per frame are uncontended compare-exchanges on the
allocator's own state; at 50 ns each they would add 0.05%. Clause (1) is met on the realistic frames, so nothing is
repaired; the construction-time counting choice is not needed.

Still open and owned: the two-binary floor (1a); the offline residual against compiled-out (3), which is not the gate;
the authored rendering workload (7), whose frame gets the same count when it lands.

## External sampling capture (clause (8)), read

`build/diag-overhead-wpr/20261019T080356Z/analysis.json` does not attribute correctly. It puts 4,976 of 7,851 offline
samples and 3,487 of 3,570 alloc samples of the compiled-out binary (win-shipping) in `memory-stats` and
`flight-recorder`, although that binary has no statistics code and the off mode runs no recorder. It also puts 93.4%
of the offline off arm in `memory-stats`, although off is within a few percent of compiled-out. Its 6 DISAGREES and 2
UNRESOLVED verdicts are therefore not evidence about the modes yet. The classifier and the symbol resolution are
queued as the next batch (the follow-up below). They are not used for this verdict.

## Not done in this batch

- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads. Each waits for the user's own approval in the
  conversation; an approval relayed in a workflow's text is not the user's consent.
- Follow-up: explain the WPR capture's attributions (above), then resolve its verdicts.
