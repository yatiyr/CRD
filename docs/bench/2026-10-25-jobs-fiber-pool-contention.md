# 2026-10-25 — JOBS-HARD.3: the fiber pool's contention, before and after

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [JOBS-HARD.3](../ROADMAP.md#slice-jobs-hard.3); session:
> [2026-10-25](../sessions/2026-10-25-jobs-fiber-pool-contention.md); contract:
> [ADR-0033](../decisions/0033-jobs-implementation-architecture.md) decisions 8 and 9. The contention was found in the
> [external sampling board](2026-10-25-diag-11b-external-sampling.md).

- **Machine/config:** Intel i9-14900K (8 performance cores with 16 logical processors 0-15, 16 efficient cores,
  32 logical processors), 63.7 GiB, Windows 11 10.0.26200; MSVC 19.51 for win-shipping-profile (profiling compiled
  in) and win-shipping (compiled out); WSL2 Ubuntu 24.04 with GCC 13.3 for linux-gcc-shipping (compiled out) and its
  profiling twin (`CRD_LANE_ROOT=~/cerid-profile-scratch wsl-lane.sh configure linux-gcc-shipping
  -DCRD_ENABLE_PROFILING=ON`); Valgrind 3.22 cachegrind.
- **Load:** the history boards ran on an idle host (`build-bench.py host`: idle, under 1 core of other load); the probes ran between them without a check of their own. The final boards ran while an interactive game used 1.1
  to 3.8 cores (`build-bench.py host`: CONTENDED); every arm of a board is interleaved and paired by repetition, so
  drift lands on before and after alike, but the p95/p99 columns of the final Windows boards are wider than on an idle
  host and the 24-process intervals are not the 96-288-process budget boards.
- **Harness:** [`scripts/diag-overhead.py`](../../scripts/diag-overhead.py) `run --before-exe` (the before-off,
  before-basic and before-full arms of the build made before the change, interleaved with compiled-out, off, basic and
  full; 50 warmup frames; the jobs workload, 2048 jobs a frame each summing 512 squares) and `instructions
  --before-exe`; the free-list tally of `test_fiber_keep.cpp` (`FiberPool::acquisitions()`).
- **Binaries (SHA-256 prefixes):** before, c4a56808: win-shipping-profile `66d5164db847`, win-shipping `0847ab4726e2`,
  linux-gcc-shipping `ab72f4e7d72c`, its twin `8989a36da659`. Final, cdcc3e79: win-shipping-profile `6c36c9e1476e`,
  win-shipping `8da810de748e`, linux-gcc-shipping `20e48244a3d2`, twin `da9dae8f6bee`.
- **Peers:** none. The change is internal to crd-jobs and is measured against the same harness before it. Other fiber
  job systems (FiberTaskingLib, marl, enkiTS) are not installed here; installing one is a download that waits for the
  user's approval, and a fair comparison needs the same workload ported to each. N/A, checked.

## Final: cdcc3e79 against c4a56808

Each cell: the before arm's p50 → the final arm's p50 (µs), the paired frame-time change, the paired CPU-per-frame
change, each the median over the repetitions.

| Source | Build | Shape | off: p50 µs, frame, CPU | basic | full |
|---|---|---|---|---|---|
| cdcc3e79 | win-shipping-profile | 4 threads on 5555, 24 × 2000 | 373 → 224, -39.9%, -42.4% | 383 → 226, -40.6%, -43.0% | 509 → 352, -30.9%, -32.9% |
| cdcc3e79 | win-shipping-profile | 8 threads on FFFF, 24 × 2000 | 399 → 199, -50.2%, -67.0% | 403 → 195, -52.0%, -64.9% | 401 → 278, -29.3%, -49.7% |
| cdcc3e79 | win-shipping-profile | 16 threads on FFFF, 24 × 2000 | 1992 → 227, -88.6%, -86.2% | 1901 → 232, -87.7%, -86.2% | 1827 → 301, -83.4%, -81.3% |
| cdcc3e79 | linux-gcc-shipping twin | 4 threads on 5555, 24 × 2000 | 366 → 197, -46.1%, -57.1% | 352 → 196, -44.2%, -54.6% | 454 → 279, -38.4%, -44.6% |
| cdcc3e79 | linux-gcc-shipping twin | 8 threads on FFFF, 12 × 1000 | 479 → 250, -46.8%, -68.6% | 463 → 247, -46.8%, -68.6% | 434 → 270, -38.0%, -61.2% |
| cdcc3e79 | linux-gcc-shipping twin | 16 threads on FFFF, 12 × 1000 | 15814 → 315, -98.0%, -87.6% | 15808 → 313, -98.0%, -88.0% | 15805 → 378, -97.6%, -87.8% |

Windows boards in full:

### win-shipping-profile, jobs, 4 threads on 5555

Windows 11 (AMD64), 32 logical processors; 24 processes per arm, 2000 timed frames after 50 warmup each; affinity 5555.

| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | CPU per frame | CPU cost (95% interval) | Verdict | Payloads / recorded / filtered | Captures (bytes) | Resident growth |
|---|---|---:|---:|---:|---:|---|---:|---|---|---|---|---:|
| jobs | compiled-out | 225.4 | 597.2 | 996.3 | 110.1 | reference | 2.72 Mcycles |  |  | 0 / 0 / 0 | 0 of 0 (0) | 3.3 MiB |
| jobs | off | 224.4 | 592.8 | 998.4 | 109.6 | -0.80% vs compiled-out (-1.49% .. +0.11%) | 2.70 Mcycles | -0.83% (-1.48% .. -0.36%) | within (1%) | 0 / 0 / 0 | 0 of 0 (0) | 3.3 MiB |
| jobs | basic | 226.4 | 607.2 | 1008.3 | 110.5 | +1.05% vs off (-0.58% .. +2.09%) | 2.72 Mcycles | +1.11% (-0.63% .. +2.12%) | unresolved (2%) | 0 / 0 / 100810800 | 72 of 72 (207720) | 3.9 MiB |
| jobs | full | 352.2 | 916.4 | 1220.0 | 172.0 | +57.13% vs off (+56.24% .. +58.49%) | 4.25 Mcycles | +57.08% (+56.41% .. +58.13%) | reported | 100761600 / 100810800 / 0 | 72 of 72 (3910947) | 5.4 MiB |
| jobs | before-off | 372.9 | 1013.4 | 1345.2 | 182.1 | reference | 4.72 Mcycles |  |  | 0 / 0 / 0 | 0 of 0 (0) | 3.3 MiB |
| jobs | before-basic | 382.9 | 1025.1 | 1361.0 | 187.0 | reference | 4.77 Mcycles |  |  | 0 / 0 / 100810800 | 72 of 72 (207720) | 3.9 MiB |
| jobs | before-full | 509.0 | 1253.2 | 1569.6 | 248.5 | reference | 6.33 Mcycles |  |  | 100761600 / 100810800 / 0 | 72 of 72 (3910920) | 5.4 MiB |
| jobs | off vs before-off |  |  |  |  | -39.88% (-40.16% .. -39.51%) |  | -42.39% (-43.54% .. -41.57%) | reported |  |  |  |
| jobs | basic vs before-basic |  |  |  |  | -40.56% (-42.31% .. -39.85%) |  | -43.03% (-44.59% .. -42.19%) | reported |  |  |  |
| jobs | full vs before-full |  |  |  |  | -30.89% (-31.47% .. -29.94%) |  | -32.85% (-33.55% .. -31.99%) | reported |  |  |  |
| jobs | basic storage | | | | | ring 152 KiB, resident growth over off 0.55 MiB | | | within (32 MiB) | | | |

### win-shipping-profile, jobs, 8 threads on FFFF

Windows 11 (AMD64), 32 logical processors; 24 processes per arm, 2000 timed frames after 50 warmup each; affinity FFFF.

| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | CPU per frame | CPU cost (95% interval) | Verdict | Payloads / recorded / filtered | Captures (bytes) | Resident growth |
|---|---|---:|---:|---:|---:|---|---:|---|---|---|---|---:|
| jobs | compiled-out | 198.3 | 435.7 | 634.8 | 96.8 | reference | 3.56 Mcycles |  |  | 0 / 0 / 0 | 0 of 0 (0) | 3.8 MiB |
| jobs | off | 198.8 | 440.3 | 657.6 | 97.1 | +1.11% vs compiled-out (-1.98% .. +3.38%) | 3.54 Mcycles | +0.14% (-2.60% .. +1.29%) | unresolved (1%) | 0 / 0 / 0 | 0 of 0 (0) | 3.8 MiB |
| jobs | basic | 195.3 | 401.6 | 546.5 | 95.4 | -2.22% vs off (-3.53% .. +0.19%) | 3.67 Mcycles | +4.34% (+3.17% .. +6.16%) | over (2%) | 0 / 0 / 100810800 | 72 of 72 (207720) | 4.3 MiB |
| jobs | full | 277.9 | 543.1 | 705.4 | 135.7 | +40.33% vs off (+36.86% .. +42.95%) | 5.35 Mcycles | +51.67% (+49.11% .. +54.16%) | reported | 100761600 / 100810800 / 0 | 72 of 72 (3910944) | 6.3 MiB |
| jobs | before-off | 399.2 | 961.1 | 1613.4 | 194.9 | reference | 10.77 Mcycles |  |  | 0 / 0 / 0 | 0 of 0 (0) | 3.8 MiB |
| jobs | before-basic | 402.9 | 654.2 | 1126.6 | 196.7 | reference | 10.42 Mcycles |  |  | 0 / 0 / 100810800 | 72 of 72 (207720) | 4.3 MiB |
| jobs | before-full | 401.1 | 751.9 | 1339.0 | 195.8 | reference | 10.62 Mcycles |  |  | 100761600 / 100810800 / 0 | 72 of 72 (3910893) | 6.3 MiB |
| jobs | off vs before-off |  |  |  |  | -50.24% (-52.78% .. -48.03%) |  | -67.01% (-68.19% .. -66.23%) | reported |  |  |  |
| jobs | basic vs before-basic |  |  |  |  | -51.95% (-53.53% .. -50.75%) |  | -64.95% (-66.15% .. -63.74%) | reported |  |  |  |
| jobs | full vs before-full |  |  |  |  | -29.35% (-33.04% .. -28.42%) |  | -49.66% (-50.81% .. -49.18%) | reported |  |  |  |
| jobs | basic storage | | | | | ring 152 KiB, resident growth over off 0.59 MiB | | | within (32 MiB) | | | |

### win-shipping-profile, jobs, 16 threads on FFFF

Windows 11 (AMD64), 32 logical processors; 24 processes per arm, 2000 timed frames after 50 warmup each; affinity FFFF.

| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | CPU per frame | CPU cost (95% interval) | Verdict | Payloads / recorded / filtered | Captures (bytes) | Resident growth |
|---|---|---:|---:|---:|---:|---|---:|---|---|---|---|---:|
| jobs | compiled-out | 228.8 | 440.3 | 654.2 | 111.7 | reference | 4.17 Mcycles |  |  | 0 / 0 / 0 | 0 of 0 (0) | 4.5 MiB |
| jobs | off | 227.2 | 430.2 | 639.3 | 110.9 | -0.16% vs compiled-out (-1.00% .. +1.12%) | 4.17 Mcycles | +0.11% (-0.53% .. +0.63%) | unresolved (1%) | 0 / 0 / 0 | 0 of 0 (0) | 4.5 MiB |
| jobs | basic | 232.0 | 400.5 | 529.9 | 113.3 | +1.58% vs off (+0.94% .. +2.56%) | 4.29 Mcycles | +2.78% (+1.04% .. +3.95%) | unresolved (2%) | 0 / 0 / 100810800 | 72 of 72 (207720) | 5.1 MiB |
| jobs | full | 300.8 | 534.0 | 686.3 | 146.9 | +32.65% vs off (+31.24% .. +34.46%) | 5.86 Mcycles | +40.92% (+38.87% .. +41.79%) | reported | 100761600 / 100810800 / 0 | 72 of 72 (3910956) | 8.0 MiB |
| jobs | before-off | 1991.9 | 3402.8 | 4299.8 | 972.6 | reference | 30.36 Mcycles |  |  | 0 / 0 / 0 | 0 of 0 (0) | 4.5 MiB |
| jobs | before-basic | 1901.0 | 3197.6 | 3874.9 | 928.2 | reference | 31.31 Mcycles |  |  | 0 / 0 / 100810800 | 72 of 72 (207720) | 5.1 MiB |
| jobs | before-full | 1827.1 | 3039.1 | 3777.2 | 892.1 | reference | 31.35 Mcycles |  |  | 100761600 / 100810800 / 0 | 72 of 72 (3910968) | 8.0 MiB |
| jobs | off vs before-off |  |  |  |  | -88.61% (-88.90% .. -88.24%) |  | -86.20% (-86.46% .. -85.84%) | reported |  |  |  |
| jobs | basic vs before-basic |  |  |  |  | -87.66% (-88.38% .. -87.28%) |  | -86.20% (-86.37% .. -86.10%) | reported |  |  |  |
| jobs | full vs before-full |  |  |  |  | -83.39% (-83.80% .. -83.06%) |  | -81.32% (-81.42% .. -81.04%) | reported |  |  |  |
| jobs | basic storage | | | | | ring 152 KiB, resident growth over off 0.63 MiB | | | within (32 MiB) | | | |

Linux boards in full:

### linux-gcc-shipping (profiling twin), jobs, 4 threads on 5555

Linux 6.6.87.2-microsoft-standard-WSL2 (x86_64), 32 logical processors; 24 processes per arm, 2000 timed frames after 50 warmup each; affinity 5555.

| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | CPU per frame | CPU cost (95% interval) | Verdict | Payloads / recorded / filtered | Captures (bytes) | Resident growth |
|---|---|---:|---:|---:|---:|---|---:|---|---|---|---|---:|
| jobs | compiled-out | 197.3 | 243.5 | 303.3 | 96.4 | reference | 699.3 µs |  |  | 0 / 0 / 0 | 0 of 0 (0) | 3.2 MiB |
| jobs | off | 196.9 | 240.6 | 301.3 | 96.2 | -0.31% vs compiled-out (-0.94% .. +0.73%) | 694.7 µs | -0.57% (-1.18% .. +0.38%) | within (1%) | 0 / 0 / 0 | 0 of 0 (0) | 3.2 MiB |
| jobs | basic | 196.3 | 242.1 | 298.2 | 95.9 | -0.16% vs off (-1.72% .. +0.63%) | 687.9 µs | -0.80% (-1.60% .. +0.07%) | within (2%) | 0 / 0 / 100810800 | 72 of 72 (83736) | 3.8 MiB |
| jobs | full | 279.3 | 364.5 | 441.6 | 136.4 | +42.16% vs off (+41.23% .. +43.18%) | 1051.9 µs | +51.47% (+50.25% .. +52.63%) | reported | 100761600 / 100810800 / 0 | 72 of 72 (3786984) | 5.0 MiB |
| jobs | before-off | 365.9 | 570.7 | 694.5 | 178.7 | reference | 1611.3 µs |  |  | 0 / 0 / 0 | 0 of 0 (0) | 3.2 MiB |
| jobs | before-basic | 351.8 | 548.4 | 621.9 | 171.8 | reference | 1529.3 µs |  |  | 0 / 0 / 100810800 | 72 of 72 (83736) | 3.8 MiB |
| jobs | before-full | 454.2 | 533.2 | 596.1 | 221.8 | reference | 1894.0 µs |  |  | 100761600 / 100810800 / 0 | 72 of 72 (3786984) | 4.8 MiB |
| jobs | off vs before-off |  |  |  |  | -46.08% (-46.65% .. -45.32%) |  | -57.08% (-57.30% .. -56.14%) | reported |  |  |  |
| jobs | basic vs before-basic |  |  |  |  | -44.21% (-45.05% .. -43.45%) |  | -54.55% (-55.50% .. -53.76%) | reported |  |  |  |
| jobs | full vs before-full |  |  |  |  | -38.43% (-38.78% .. -37.80%) |  | -44.57% (-44.77% .. -44.26%) | reported |  |  |  |
| jobs | basic storage | | | | | ring 152 KiB, resident growth over off 0.50 MiB | | | within (32 MiB) | | | |

### linux-gcc-shipping (profiling twin), jobs, 8 threads on FFFF

Linux 6.6.87.2-microsoft-standard-WSL2 (x86_64), 32 logical processors; 12 processes per arm, 1000 timed frames after 50 warmup each; affinity FFFF.

| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | CPU per frame | CPU cost (95% interval) | Verdict | Payloads / recorded / filtered | Captures (bytes) | Resident growth |
|---|---|---:|---:|---:|---:|---|---:|---|---|---|---|---:|
| jobs | compiled-out | 245.4 | 347.0 | 388.1 | 119.8 | reference | 1416.2 µs |  |  | 0 / 0 / 0 | 0 of 0 (0) | 3.8 MiB |
| jobs | off | 249.7 | 348.0 | 390.9 | 121.9 | +1.78% vs compiled-out (-1.21% .. +4.31%) | 1458.0 µs | +1.59% (-1.15% .. +5.14%) | unresolved (1%) | 0 / 0 / 0 | 0 of 0 (0) | 3.8 MiB |
| jobs | basic | 246.5 | 348.9 | 388.8 | 120.4 | -1.00% vs off (-3.34% .. +1.32%) | 1400.1 µs | -2.41% (-5.53% .. +0.23%) | within (2%) | 0 / 0 / 25817400 | 36 of 36 (41868) | 4.2 MiB |
| jobs | full | 269.6 | 423.6 | 481.4 | 131.6 | +7.76% vs off (+3.97% .. +12.11%) | 1616.2 µs | +11.11% (+7.81% .. +17.97%) | reported | 25804800 / 25817400 / 0 | 36 of 36 (1893489) | 5.5 MiB |
| jobs | before-off | 479.4 | 869.5 | 948.5 | 234.1 | reference | 4596.0 µs |  |  | 0 / 0 / 0 | 0 of 0 (0) | 3.8 MiB |
| jobs | before-basic | 462.7 | 835.7 | 891.3 | 225.9 | reference | 4532.8 µs |  |  | 0 / 0 / 25817400 | 36 of 36 (41868) | 4.2 MiB |
| jobs | before-full | 434.0 | 749.6 | 806.9 | 211.9 | reference | 4188.1 µs |  |  | 25804800 / 25817400 / 0 | 36 of 36 (1893492) | 5.5 MiB |
| jobs | off vs before-off |  |  |  |  | -46.77% (-49.33% .. -44.98%) |  | -68.61% (-69.79% .. -68.11%) | reported |  |  |  |
| jobs | basic vs before-basic |  |  |  |  | -46.79% (-47.52% .. -45.44%) |  | -68.65% (-69.21% .. -67.77%) | reported |  |  |  |
| jobs | full vs before-full |  |  |  |  | -38.01% (-39.31% .. -35.94%) |  | -61.24% (-61.89% .. -60.46%) | reported |  |  |  |
| jobs | basic storage | | | | | ring 152 KiB, resident growth over off 0.50 MiB | | | within (32 MiB) | | | |

### linux-gcc-shipping (profiling twin), jobs, 16 threads on FFFF

Linux 6.6.87.2-microsoft-standard-WSL2 (x86_64), 32 logical processors; 12 processes per arm, 1000 timed frames after 50 warmup each; affinity FFFF.

| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | CPU per frame | CPU cost (95% interval) | Verdict | Payloads / recorded / filtered | Captures (bytes) | Resident growth |
|---|---|---:|---:|---:|---:|---|---:|---|---|---|---|---:|
| jobs | compiled-out | 312.2 | 551.0 | 601.2 | 152.5 | reference | 3358.8 µs |  |  | 0 / 0 / 0 | 0 of 0 (0) | 4.8 MiB |
| jobs | off | 315.1 | 560.0 | 602.8 | 153.9 | +0.14% vs compiled-out (-6.40% .. +7.46%) | 3281.9 µs | -4.12% (-5.64% .. +4.22%) | unresolved (1%) | 0 / 0 / 0 | 0 of 0 (0) | 4.8 MiB |
| jobs | basic | 313.3 | 558.1 | 606.4 | 153.0 | +1.58% vs off (-6.85% .. +4.96%) | 3148.2 µs | -2.87% (-5.60% .. +0.50%) | unresolved (2%) | 0 / 0 / 25817400 | 36 of 36 (41868) | 5.2 MiB |
| jobs | full | 378.2 | 588.2 | 635.4 | 184.6 | +21.53% vs off (+20.05% .. +26.05%) | 3347.5 µs | -2.04% (-3.06% .. +5.67%) | reported | 25804800 / 25817400 / 0 | 36 of 36 (1893486) | 6.8 MiB |
| jobs | before-off | 15813.6 | 17184.6 | 17929.9 | 7721.5 | reference | 26045.6 µs |  |  | 0 / 0 / 0 | 0 of 0 (0) | 4.5 MiB |
| jobs | before-basic | 15807.7 | 17144.9 | 17853.7 | 7718.6 | reference | 26264.4 µs |  |  | 0 / 0 / 25817400 | 36 of 36 (41868) | 5.2 MiB |
| jobs | before-full | 15804.7 | 17087.1 | 17869.2 | 7717.1 | reference | 27691.0 µs |  |  | 25804800 / 25817400 / 0 | 36 of 36 (1893492) | 6.5 MiB |
| jobs | off vs before-off |  |  |  |  | -98.05% (-98.08% .. -97.97%) |  | -87.55% (-87.88% .. -86.73%) | reported |  |  |  |
| jobs | basic vs before-basic |  |  |  |  | -98.03% (-98.12% .. -97.99%) |  | -87.98% (-88.29% .. -87.38%) | reported |  |  |  |
| jobs | full vs before-full |  |  |  |  | -97.63% (-97.66% .. -97.56%) |  | -87.83% (-88.49% .. -87.67%) | reported |  |  |  |
| jobs | basic storage | | | | | ring 152 KiB, resident growth over off 0.50 MiB | | | within (32 MiB) | | | |

## Instruction counts (cachegrind, off mode, cdcc3e79 against c4a56808)

### linux-gcc-shipping, jobs off, 1 jobs thread(s)

cachegrind with a simulated cache; per frame = the difference of runs of 100 and 300 timed frames after 20 warmup, 2 repeats each; 1 jobs thread(s).

| Workload | Arm | instructions per frame | L1 misses per frame | LL misses per frame | Verdict |
|---|---|---:|---:|---:|---|
| jobs | off | 1050694 | 12357 | 0 | |
| jobs | before-off | 1207675 | 12353 | 0 | |
| jobs | off vs before-off | -13.00% | +0.03% | +0.00% | reported |

### linux-gcc-shipping, jobs off, 4 jobs thread(s)

cachegrind with a simulated cache; per frame = the difference of runs of 100 and 300 timed frames after 20 warmup, 2 repeats each; 4 jobs thread(s).

| Workload | Arm | instructions per frame | L1 misses per frame | LL misses per frame | Verdict |
|---|---|---:|---:|---:|---|
| jobs | off | 1248180 .. 1255484 | 12365 .. 12381 | 0 .. 0 | |
| jobs | before-off | 2815213 .. 2842028 | 8336 .. 8337 | 0 .. 0 | |
| jobs | off vs before-off | -56.08% .. -55.40% | +48.31% .. +48.52% | -11.11% .. +36.36% | reported |

### linux-gcc-shipping profiling twin, jobs off, 1 jobs thread

cachegrind with a simulated cache; per frame = the difference of runs of 100 and 300 timed frames after 20 warmup, 2 repeats each; 1 jobs thread(s).

| Workload | Arm | instructions per frame | L1 misses per frame | LL misses per frame | Verdict |
|---|---|---:|---:|---:|---|
| jobs | compiled-out | 1050694 | 12357 | 0 | |
| jobs | off | 1050699 | 12357 | 0 | |
| jobs | before-off | 1207680 | 12352 | 0 | |
| jobs | off vs compiled-out | +0.00% | -0.00% | +0.00% | within (1%) |
| jobs | off vs before-off | -13.00% | +0.04% | +0.00% | reported |

Every count's interval spans every pairing of its repeats; at one thread the totals repeat exactly. The four-thread
first-level misses rise (8,336 → 12,365 a frame) while instructions fall 56%: under Valgrind the threads run one at a
time, so the misses are the simulated cache's view of fewer, denser frames; they are reported, not judged.

## Free-list traffic

`FiberPool::acquisitions(Small)` after 200 frames of 2048 short jobs (409,600 jobs), win-shipping `crd-jobs-tests`,
unpinned, with the thread count swept (a temporary environment switch in the white-box case, not committed):

| Version | 4 threads | 8 threads | 16 threads |
|---|---:|---:|---:|
| before the change | 409,600 (one a job, by construction) | 409,600 | 409,600 |
| 1a12466c: returned on every empty `try_pop` | 753 | 1,581 | 112,354 |
| e0de26cc and later: returned only before blocking | 203, 203 | 207, 207 | 124, 50 |

## The path there

The same boards for each step (24 processes × 2000 frames unless noted; probes 8 × 1000):

| Source | Build | Shape | off: p50 µs, frame, CPU | basic | full |
|---|---|---|---|---|---|
| 1a12466c | win-shipping-profile | 4 threads on 5555, 24 × 2000 | 358 → 237, -33.5%, -32.5% | 361 → 237, -34.2%, -33.6% | 480 → 334, -30.3%, -30.0% |
| 1a12466c | win-shipping-profile | 8 threads on FFFF, 24 × 2000 | 403 → 238, -42.1%, -41.7% | 394 → 239, -41.2%, -35.9% | 386 → 241, -37.4%, -37.2% |
| 1a12466c | win-shipping-profile | 16 threads on FFFF, 24 × 2000 | 1727 → 1580, -8.2%, -10.8% | 1532 → 1435, -6.1%, -12.5% | 1380 → 1345, -4.6%, -11.6% |
| e0de26cc | win-shipping-profile | 4 threads on 5555, 24 × 2000 | 354 → 235, -33.4%, -32.7% | 359 → 236, -34.1%, -33.3% | 480 → 333, -30.8%, -30.5% |
| e0de26cc | win-shipping-profile | 8 threads on FFFF, 24 × 2000 | 400 → 230, -43.6%, -40.0% | 405 → 225, -44.8%, -41.2% | 385 → 240, -37.8%, -37.4% |
| e0de26cc | win-shipping-profile | 16 threads on FFFF, 24 × 2000 | 1810 → 1662, -8.2%, -10.0% | 1626 → 1557, -3.8%, -7.9% | 1483 → 1439, -4.7%, -8.9% |
| e0de26cc | linux-gcc-shipping twin | 4 threads on 5555, 24 × 2000 | 361 → 448, +25.3%, +21.1% | 350 → 416, +18.8%, +18.8% | 453 → 336, -25.8%, -24.8% |
| cdcc3e79 (probe) | linux-gcc-shipping twin | 4 threads on 5555, 8 × 1000 | 360 → 193, -45.9%, -55.2% |  | 448 → 271, -39.0%, -45.5% |
| cdcc3e79 (probe) | linux-gcc-shipping twin | 8 threads on FFFF, 8 × 1000 | 460 → 253, -44.5%, -68.7% |  | 417 → 261, -37.1%, -57.7% |
| cdcc3e79 (probe) | win-shipping-profile | 8 threads on FFFF, 8 × 1000 | 391 → 177, -55.1%, -62.8% |  | 389 → 218, -44.3%, -48.0% |
| cdcc3e79 (probe) | win-shipping-profile | 16 threads on FFFF, 8 × 1000 | 1740 → 213, -88.0%, -86.1% |  | 1417 → 268, -82.1%, -81.5% |

1a12466c already removed the Windows contention at four and eight threads, but at sixteen it still reached the free
list once every 3.6 jobs. e0de26cc made Linux worse at four threads (off +25.3%, basic +18.8%) while full got faster than
off: the scheduler's token per job and wake per job, no longer paced by the pool, became the bottleneck. cdcc3e79's
batch wake removed it (the probes, then the final boards).

## Verdict

The fiber pool's free list is reached about once per thread per frame at every shape measured (203 to 207, and 50 to
124 at sixteen threads, against 409,600 before). Against the build before the change, the jobs workload is faster in
frame time and in CPU per frame in off, basic and full at four, eight and sixteen threads on both platforms, and 13.0%
fewer instructions a frame at one thread (55% to 56% at four). Off over compiled-out stays within the 1% budget in
instructions (+0.00%) and in the four-thread boards (Windows -0.80%, Linux -0.31%).
