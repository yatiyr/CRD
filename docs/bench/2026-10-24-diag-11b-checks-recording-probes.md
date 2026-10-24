# DIAG.11b — generation checks, recording and probes, per call

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md#slice-diag.11b); current rules: [AGENTS](../../AGENTS.md).

**Date** 2026-10-24 · **Host** i9-14900K (32 logical processors), 64 GiB, Windows 11 Pro 10.0.26200 and WSL2
(Ubuntu 24.04), idle (`build-bench.py host`) · **Compilers** MSVC 19.51 (VS 18), GCC 13.3 · **Program**
[`tests/bench/test_bench_diag.cpp`](../../tests/bench/test_bench_diag.cpp) in `crd-bench` · **Session**
[2026-10-24 checks, recording and probes](../sessions/2026-10-24-diag-11b-checks-recording-probes-cost.md)

## What is measured

The three mechanisms the [runbooks](../design/diagnostic-runbooks.md) listed as not boarded are not modes: the
generation check is in every lookup, recording is a `replay.record` request, and a probe is a call a boundary makes.
None has a build without it, so each is timed per call inside one binary against its own baseline (a raw index, a plain
run, an empty opaque call); the two-binary layout floor does not apply. Catch2 benchmarks, 100 samples each, two runs
per lane; each case checks its answers before it times anything (lookups find their values and refuse the stale
handles, the trace holds the run's 60,004 dispatches, the probes read the declared mode and a clean block).

Lanes: win-release (MSVC `/O2 /GL`, asserts off; `crd-bench.exe` `4ba3ab1650ca`) and linux-gcc-relwithdebinfo (GCC
`-O2` with link-time optimization and asserts on; `crd-bench` `a339b50ee929`, through `scripts/wsl-lane.sh run`). The
shipping presets build no benchmarks. Command: `<tree>/tests/bench/crd-bench "[bench-diag]" --benchmark-samples 100
--rng-seed 11`.

## Results

Each cell is the range of the two runs' means; a percentage is over the plain run of the same lane.

| Mechanism | Measured | win-release | linux-gcc-relwithdebinfo |
| --- | --- | ---: | ---: |
| Generation check | raw index (no check), per lookup | 0.18 .. 0.19 ns | 0.17 .. 0.18 ns |
| | `containers::SlotMap::get`, live handle | 0.71 .. 0.72 ns | 0.85 ns |
| | `containers::SlotMap::get`, stale handle (refused) | 0.77 .. 0.79 ns | 0.63 .. 0.64 ns |
| | `scene::SlotMap::is_alive` (entities) | 0.55 .. 0.56 ns | 0.54 .. 0.59 ns |
| Recording | `plan::run` of `main(20000)` (60,004 dispatches) | 542 .. 544 µs | 656 .. 703 µs |
| | `run_traced`, 4,096 events kept (`replay.record`'s default) | 813 .. 838 µs (+50% .. +55%) | 884 .. 955 µs (+35% .. +36%) |
| | `run_traced`, every event kept (the bound is 65,536) | 1,795 .. 1,803 µs (+232%) | 1,558 .. 1,732 µs (+137% .. +146%) |
| | `encode_record`, 4,096 events (78,324 bytes) | 174 .. 182 µs | 172 .. 194 µs |
| | `encode_record`, 60,004 events (1,121,940 bytes) | 2.76 .. 2.77 ms | 2.66 .. 2.95 ms |
| Probes | an opaque empty call (baseline) | 0.71 .. 0.74 ns | 0.84 .. 0.93 ns |
| | `call_keeping_fp_environment` around it (two reads, a compare) | 14.0 ns | 3.3 .. 3.6 ns |
| | `probe_values`, 1,024 finite `f32` (a 512-frame stereo block) | 2.32 .. 2.47 µs | 1.32 .. 1.44 µs |
| | `jobs::pressure_snapshot` (pool running) | 40 .. 42 ns | 54 .. 59 ns |
| | lifetime ledger `snapshot()` | 9.2 .. 9.4 ns | 10.4 .. 11.0 ns |
| | lifetime ledger `acquired` + `released` | 15.5 .. 16.3 ns | 14.5 .. 16.0 ns |

## What the numbers say

- **Generation check.** Bounds, occupancy and generation compares add 0.5 to 0.7 ns per lookup over a raw index in
  shuffled order; refusing a stale handle costs the same. The engine's `containers::SlotMap` user is the GPU identity
  registry, which looks up under its mutex at creation and retirement, never per frame; entity lookups go through
  `scene::SlotMap::is_alive`, about 0.55 ns each.
- **Recording.** A traced run pays per dispatch: about 4 to 5 ns averaged over the run with the default 4,096 events
  kept (each kept event reads up to four results at the next safe point; a dispatch past the bound only counts), and
  15 to 21 ns with every event kept. The record's encode then costs about 45 ns per kept event. This is the same shape
  as the CEIR profiling bridge (+220% on offline full, two cycle-counter reads per dispatch). It is paid once per
  `replay.record` request, never by an unrecorded run.
- **Probes.** A float-mode boundary costs 14 ns on MSVC (one `stmxcsr` a read) and 3.5 ns on GCC (which also reads
  the x87 control word); the driver witness is this pattern plus one relaxed counter add, around each submit and
  present: 14 ns is 0.004% of the 361 µs D3D12 `render` frame ([device validation board](2026-10-24-diag-11b-device-validation.md))
  per witnessed call. The audio boundary's value probe costs 1.3 to 2.4 ns per sample: 2.4 µs per 512-frame block,
  0.02% of the block's 10.7 ms at 48 kHz. A pressure or ledger snapshot (10 to 60 ns) is taken when a probe asks,
  not per frame, and a ledger record (15 ns) is made per acquisition and release of a counted resource.
- The MSVC and GCC float-mode figures differ fourfold; both are reported, neither is judged: no budget covers these
  calls (ADR-0133 ID-5 budgets the diagnostic modes, and none of these is a mode).
