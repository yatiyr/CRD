# 2026-11-01 — SIMD-PERF: the f32 forward's avx512 plans from the benchmark machine's level board (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-avx512-level-plans.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3. **Real AVX-512 hardware.** Every process pinned
  to CPU 3, the peers one thread; `build-bench.py host` before each board: 28.6 of 29.4 GiB available, others 0.0
  cores, idle. One start.
- **Binaries:** `bench_fft_f32` from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO): HEAD
  (`1228a0b1`, `~/ab/b20-head`; avx512 ran the canonical planner, no avx512 row) and the candidate (`f6230d8f`,
  `~/ab/b20-cand`; the avx512 rows below). `CRD_CPU_LEVEL=avx512` selects the fft unit
  (`cpu detected=avx512 selected=avx512 override=applied kernel_cap=avx512`).
- **Peer:** Intel MKL 2020.0.4 (one thread), timed in the same process; every arm gated against the FFTW f64 truth.

## Board 1: the level board (the plan table's data)

`scripts/board_fft_level_plans.sh ~/ab/b20-level --type f32 --passes 5` with `CRD_BENCH_LEVELS=avx512`: five passes
of `bench_fft_f32` timing every forward the planner offers at 1K to 4M (each transform warm, best of the program's
repetitions, a 64-byte-aligned buffer), one process a pass, about 50 s a pass. `gen_fft_level_plans.py --import`
filed the passes as the f32 data file's avx512 entry
([2026-10-30-simd-dispatch-fft-f32-arms.json](2026-10-30-simd-dispatch-fft-f32-arms.json), every pass's time per
arm, with this machine, commit and board) beside the i9's portable, sse2 and avx2 entries, which it kept. A level's
plan of a size is the fastest median whose every pass beat every pass of the planned (canonical) arm.

| n | canonical (HEAD) | median ms [min-max] | fastest | median ms [min-max] | row |
|---|---|---|---|---|---|
| 1K | standalone-hier | 0.00061 [0.00061-0.00062] | standalone-hier | | canonical |
| 2K to 16K | in-place-3 | 0.00131 to 0.01698 | in-place-3 | | canonical |
| 32K | ip4 | 0.05027 [0.05013-0.05074] | deep-split-4 | 0.03951 [0.03893-0.03996] | **deep-split-4** |
| 64K | ip4 | 0.12018 [0.11968-0.12200] | deep-split-4 | 0.08764 [0.08623-0.09155] | **deep-split-4** |
| 128K | deep-split-4 | 0.2652 [0.25985-0.27164] | deep-split-3 | 0.25919 [0.2537-0.26599] | canonical (ranges overlap) |
| 256K | deep-split-4 | 0.65375 [0.64706-0.67529] | deep-split-3 | 0.62441 [0.60837-0.64467] | **deep-split-3** |
| 512K | deep-split-4 | 1.33018 [1.3101-1.3394] | deep-split-4 | | canonical |
| 1M | tiled-four-step | 2.66472 [2.63496-2.76596] | tiled-four-step | | canonical |
| 2M | four-step | 9.91399 [9.70117-10.04331] | deep-split-4 | 6.11777 [6.06773-6.1909] | **deep-split-4** |
| 4M | four-step | 21.60451 [21.47273-21.8824] | deep-split-4 | 15.01006 [14.69901-15.80216] | **deep-split-4** |

## Board 2: each new row against the canonical arm, at both buffer offsets

`scripts/ab_fft_f32.sh` with `CRD_AB_HEAD=~/ab/b20-head` and `CRD_AB_ARMS` (the two arms and MKL from one process a
repetition), the canonical arm the reference, at `CRD_AB_OFFSET` 0 (a 64-byte boundary) and 16 (a malloc'd buffer):
median paired speed ratio [percentile-bootstrap 95%], 2% band, at most 9 repetitions (7 at 2M and 4M).

| n | row against canonical | offset 0 | offset 16 |
|---|---|---|---|
| 32K | deep-split-4 / ip4 | 1.258 [1.247-1.306] faster | 1.261 [1.247-1.273] faster |
| 64K | deep-split-4 / ip4 | 1.348 [1.312-1.358] faster | 1.351 [1.310-1.354] faster |
| 256K | deep-split-3 / deep-split-4 | 1.069 [1.064-1.070] faster | 1.063 [1.058-1.064] faster |
| 2M | deep-split-4 / four-step | 1.651 [1.578-1.665] faster | 1.530 [1.507-1.548] faster |
| 4M | deep-split-4 / four-step | 1.566 [1.523-1.597] faster | 1.675 [1.652-1.704] faster |

Every row wins at both alignments, so the README's rule (a plan the two alignments decide differently takes the arm
whose worse case is better) does not arise.

## Board 3: the candidate's planned forward against MKL, and against HEAD where the plan did not change

The candidate's planned forward against MKL (`CRD_AB_HEAD=~/ab/b20-cand`, `--planned`), at most 7 repetitions; the
ratio is Cerid's speed over MKL's:

| n | offset 0 | offset 16 | HEAD's plan against MKL (board 2, offset 0 / 16) |
|---|---|---|---|
| 32K | 1.020 [0.990-1.044] unresolved | 1.056 [1.018-1.131] unresolved | 0.79x / 0.83x |
| 64K | 0.997 [0.994-1.032] unresolved | 1.073 [1.035-1.085] faster | 0.75x / 0.79x |
| 128K | 1.579 [1.568-1.620] faster | 1.584 [1.570-1.588] faster | (unchanged) |
| 256K | 1.882 [1.868-1.889] faster | 1.892 [1.878-1.907] faster | 1.77x / 1.75x |
| 512K | 1.842 [1.819-1.851] faster | 1.745 [1.730-1.752] faster | (unchanged) |
| 1M | 2.081 [2.066-2.093] faster | 1.819 [1.813-1.832] faster | (unchanged) |
| 2M | 2.056 [1.991-2.057] faster | 1.743 [1.740-1.768] faster | 1.22x / 1.11x |
| 4M | 1.832 [1.797-1.863] faster | 1.541 [1.426-1.553] faster | 1.23x / 0.96x |

HEAD's column is MKL's time over the canonical arm's in board 2's processes (the reciprocal of its "mkl against"
cell). Where the plan did not change, the candidate against HEAD (`CRD_AB_CANDIDATE`, `--planned`, `--same`
digests: the two builds printed the same bits at every size of every repetition), offset 0:

| n | candidate against HEAD |
|---|---|
| 1K | 1.000 [1.000-1.018] level |
| 2K | 1.000 [1.000-1.000] level |
| 4K | 1.003 [1.003-1.017] level |
| 8K | 0.995 [0.982-1.000] level |
| 16K | 1.013 [1.000-1.035] unresolved (not slower) |
| 512K | 0.999 [0.996-1.022] unresolved (not slower) |
| 1M | 1.002 [0.998-1.030] unresolved (not slower) |

## Verdict

- **Kept:** avx512 runs its own f32 rows under `DeterminismMode::Default`: 32K and 64K 1.26x and 1.35x faster than
  HEAD's plan, from 0.75x to 0.83x MKL to parity or better (0.997x to 1.073x, every interval reaching 0.99x); 256K
  1.06x to 1.07x; 2M and 4M 1.53x to 1.68x, from 0.96x to 1.23x MKL to 1.54x to 2.06x. Every other size is the
  canonical plan, level with HEAD on the same bits.
- **Results:** each row's forward meets the ADR-0092 §5 class against the radix-2 reference and twice it against
  portable's own plan (the cross-level case, now walking to 4M; the board's errors against the FFTW f64 truth at 2M
  and 4M: 2.2e-7, the four-step's 2.8e-7 and 2.4e-7). The deterministic modes keep the canonical plan, so their bits
  are unchanged on every level.
- **Scope:** one Intel AVX-512 microarchitecture; AMD's (Zen 4 and 5) stays QUAL.1-HW's. f64's avx512 rows need
  `bench_fft_vs_refs` to print below 1 us first (this board's [predecessor](2026-11-01-simd-perf-fft-whole-block-stores.md)
  could not resolve 4K to 64K).
