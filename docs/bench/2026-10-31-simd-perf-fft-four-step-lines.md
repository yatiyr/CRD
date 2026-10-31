# 2026-10-31 — SIMD-PERF: the four-step's twiddled columns written as whole cache lines (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-10-31](../sessions/2026-10-31-simd-perf-fft-four-step-lines.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), 4 cores of 2 threads, L1d 48 KiB and L2 2 MiB a core, L3 260 MiB shared with other tenants, Ubuntu 24.04,
  kernel 7.0.0-1014-gcp, perf 7.0.14 with the guest PMU. **Real AVX-512 hardware.** Every process pinned to CPU 3,
  its sibling idle, nothing else running on the machine; `build-bench.py host` recorded before and after each board
  (in the board directories on the machine, `~/boards/b2`).
- **Binaries:** the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO, `-ffp-contract=off`; kernel units per
  level, ADR-0134 §6) built from the native checkout at **HEAD** (the engine of `3e596ed5` with this session's bench
  tooling) and at the **candidate** (HEAD plus the one engine change, `perf(fft): write the four-step's twiddled
  columns as whole cache lines`), each copied aside (`~/ab/head`, `~/ab/cand`; the f32 inverse boards' pair rebuilt
  on main's later tree, `~/ab/head2`, `~/ab/cand2`, the same fft sources); the pre-row engine `2f2aed72` from
  `scripts/build-prerow.sh` (avx2 only).
- **Levels:** detected (avx512), `CRD_CPU_LEVEL=avx2` and `CRD_CPU_LEVEL=sse2` (the x86-64 floor), the same binary.
- **Peers:** Intel MKL 2020.0.4 (`libmkl_avx512.so` on this CPU), FFTW 3.3.10 (the f64 truth and peer).
- **Method:** `scripts/ab_fft_f32.sh` (HEAD's and the candidate's planned f32 forward and MKL, `bench_fft_f32
  --planned`, HEAD the reference) and `scripts/ab_fft_prerow.sh` (`prof_fft_kernel`, HEAD's saved build the
  reference at each level, or the pre-row engine), through `scripts/ab-board.py`: one warm-up discarded, then
  repetitions until no cell is unresolved (5 to 15); a cell is the median per-repetition speed ratio [percentile
  bootstrap 95%] judged against a 2% band. **Every HEAD-against-candidate board ran with `--same`: both builds printed
  the same FNV-1a digest of their output at every size of every repetition (bit-exact).** Phase split by the
  `CRD_HESAP_FFT_PROFILE` tree's counters, counts by `perf stat`.

## Where the time went (HEAD, before the change)

The four-step's phase counters (`prof_fft_kernel` and `bench_fft_f32 --loop planned`, the profile tree, Mcyc a
transform; P2's sub-phases are folded into its scatter where the fused gather path runs):

| transform | level | P1 gather | P1 sub-FFT | **P1 twiddle + store** | P2 gather | P2 sub-FFT | P2 scatter | ms |
|---|---|---|---|---|---|---|---|---|
| f32 2M forward | avx512 | 3.8 | 5.7 | **21.0 (50%)** | - | - | 11.6 | 19.6 |
| f32 2M forward | avx2 | 3.6 | 7.8 | **6.5 (21%)** | - | - | 13.3 | 14.9 |
| f32 4M forward | avx512 | 8.3 | 11.5 | **42.2 (42%)** | 5.9 | 11.3 | 20.2 | 46.2 |
| f32 4M forward | avx2 | 8.2 | 15.8 | **13.3 (17%)** | 5.8 | 14.3 | 20.3 | 36.7 |
| f64 2M forward | avx512 | 7.1 | 10.9 | **83.1 (69%)** | - | - | 18.7 | 51.3 |
| f64 2M forward | avx2 | 6.8 | 14.1 | **9.3 (18%)** | - | - | 22.9 | 22.3 |
| f64 4M forward | avx512 | 14.7 | 22.5 | **166.0 (66%)** | 14.7 | 22.2 | 9.8 | 105.5 |
| f64 4M forward | avx2 | 14.2 | 29.2 | **21.5 (18%)** | 14.5 | 29.5 | 9.6 | 48.2 |

Phase 1's twiddle step multiplies a tile of `lanes` columns at one k1 and stored each column's one complex into its
own tbuf row: `lanes` rows in flight a step (8 f32 or 4 f64 at avx2, 16 or 8 at avx512's two-tile step), each
a row stride of n1 complex apart (16 KiB f32, 32 KiB f64 at 2M: one L1 set for all of them). `perf stat` on the
same loops (HEAD, 30 transforms):

| transform | level | cycles | `l1d_pend_miss.fb_full` | `resource_stalls.sb` | `l2_rqsts.all_rfo` | `l1d.replacement` |
|---|---|---|---|---|---|---|
| f64 2M forward | avx512 | 5.36G | **3.65G (68%)** | 2.56G | 88M | 227M |
| f64 2M forward | avx2 | 2.85G | 0.65G (23%) | 0.80G | 90M | 240M |
| f32 2M forward | avx512 | 2.75G | 0.57G | **1.11G** | **104M** | 200M |
| f32 2M forward | avx2 | 2.31G | 0.52G | 0.71G | 58M | 163M |

f64 writes tbuf with 16-byte non-temporal stores: at avx512 eight rows' partial lines hold the fill buffers (full
68% of the cycles) and leave as partial writes. f32 writes it with plain 8-byte stores: sixteen rows in one 12-way
L1 set at avx512 evict each line before its next store, so every line is fetched for ownership again (1.8x the RFOs
of avx2) and the store buffer fills.

**The change** (`execute_four_step`, bit-exact by construction): the step keeps `lanes` consecutive k1 of its tile in
registers, transposes the re and im planes (`transpose_rows`) and writes each column's run of k1 as whole 64-byte
lines (f64 non-temporal, f32 plain, the stores each type had); the arithmetic and its order are unchanged. The first
candidate took this path at every level; the boards below found the f32 inverse at sse2 3% slower, so the kept change
takes it where the tile is one register (avx2) or two tiles (avx512) and keeps HEAD's element stores where the tile
spans two narrower registers (sse4.2, sse2, portable, NEON), whose sixteen registers the micro-tile overflows
([the kept change](#the-kept-change-the-line-path-at-avx2-and-avx512)).

The tables up to the kept change's section time **the first candidate** (the line path at every level).

## The f32 forward: HEAD, the candidate and MKL (ms, speed ratio over HEAD)

| n | level | HEAD | candidate | candidate / HEAD | MKL | HEAD x MKL | candidate x MKL |
|---|---|---|---|---|---|---|---|
| 1M (tiled four-step, control) | avx512 | 3.752 | 3.758 | 0.999 [0.993-1.011] level | 5.790 | 1.54 | 1.54 |
| 2M (four-step) | avx512 | 17.74 | 10.91 | **1.627 [1.604-1.629] faster** | 12.72 | 0.72 | **1.17** |
| 4M (four-step) | avx512 | 40.85 | 28.01 | **1.460 [1.438-1.491] faster** | 33.12 | 0.80 | **1.18** |
| 1M (control) | avx2 | 3.437 | 3.441 | 0.996 [0.992-1.006] level | 5.791 | 1.68 | 1.68 |
| 2M | avx2 | 13.41 | 12.73 | **1.061 [1.037-1.073] faster** | 12.38 | 0.92 | 0.97 |
| 4M | avx2 | 33.74 | 32.47 | **1.040 [1.036-1.079] faster** | 31.49 | 0.93 | 0.97 |
| 1M | sse2 | 29.57 | 29.81 | 0.992 [0.991-0.993] level | 5.991 | 0.20 | 0.20 |
| 2M | sse2 | 64.37 | 65.31 | 0.988 [0.981-0.996] level | 13.43 | 0.21 | 0.21 |
| 4M | sse2 | 142.5 | 144.3 | 0.988 [0.984-0.994] level | 34.98 | 0.25 | 0.24 |

Boards: avx512 and avx2 decided after 5 repetitions, sse2 after 12. "x MKL" is MKL's time over Cerid's (above 1
Cerid is faster); MKL runs its avx512 code at every Cerid level. The 1M rows are the control at avx512 and avx2
(the tiled four-step); sse2's own plan runs the four-step at 1M too (`level_plans_gen.hpp`), so its three cells all
take the change, 0.8% to 1.2% slower, inside the band.

## The f64 transforms: HEAD against the candidate (prof_fft_kernel, ms a transform)

| transform | level | HEAD | candidate | candidate / HEAD |
|---|---|---|---|---|
| 1M forward (six-step, control) | avx512 | 7.009 | 7.023 | 1.003 [0.987-1.005] level |
| 1M forward (control) | avx2 | 9.086 | 9.105 | 0.999 [0.987-1.003] level |
| 1M forward (control) | sse2 | 78.00 | 78.08 | 1.001 [0.994-1.004] level |
| 2M forward | avx512 | 50.70 | 18.56 | **2.735 [2.679-2.770] faster** |
| 2M forward | avx2 | 22.48 | 21.75 | **1.032 [1.022-1.045] faster** |
| 2M forward | sse2 | 194.0 | 194.1 | 1.000 [0.998-1.006] level |
| 4M forward | avx512 | 103.6 | 41.08 | **2.514 [2.491-2.564] faster** |
| 4M forward | avx2 | 47.49 | 46.43 | 1.031 [1.004-1.032] unresolved (15 repetitions, at or above 1) |
| 4M forward | sse2 | 411.9 | 412.3 | 0.999 [0.996-1.002] level |
| 128K inverse | avx512 | 3.145 | 1.204 | **2.612 [2.562-2.653] faster** |
| 128K inverse | avx2 | 1.678 | 1.648 | 1.021 [1.012-1.022] unresolved (15 repetitions, at or above 1) |
| 128K inverse | sse2 | 1.761 | 1.790 | 0.987 [0.982-0.991] level |
| 1M inverse | avx512 | 24.84 | 10.48 | **2.378 [2.359-2.379] faster** |
| 1M inverse | avx2 | 14.82 | 14.66 | 1.010 [1.004-1.020] level |
| 1M inverse | sse2 | 15.75 | 15.91 | 0.990 [0.982-1.006] level |
| 2M inverse | avx512 | 49.64 | 20.98 | **2.369 [2.351-2.423] faster** |
| 2M inverse | avx2 | 29.75 | 29.32 | 1.017 [1.004-1.019] level |
| 2M inverse | sse2 | 32.06 | 32.36 | 0.995 [0.987-0.999] level |

The four-step is the planned inverse from 128K and the planned f64 forward from 2M; the 1M forward is the six-step,
whose code this change does not touch (the control). At sse2 the f64 forward is 4.9x (1M, the sse2 plan's
six-step) and 6.0x (2M) slower than the same level's inverse (78.1 against 15.9 ms, 194.1 against 32.4 ms); at 2M the
profile tree's counters put it in the forward's sub-FFTs, P1's batched 2048-point plan 223.6 Mcyc against the
inverse's 25.5 and P2's fused 32x32 path 207.8 against 35.1: the sse2 forward's own gap (Left).

## The f32 inverse: HEAD against the first candidate (prof_fft_kernel f32, ms a transform)

| transform | level | HEAD | candidate | candidate / HEAD |
|---|---|---|---|---|
| 128K inverse | avx512 | 1.048 | 0.602 | **1.734 [1.724-1.745] faster** |
| 128K inverse | avx2 | 0.851 | 0.833 | 1.022 [1.019-1.027] unresolved (15 repetitions, above 1) |
| 128K inverse | sse2 | 0.878 | 0.907 | **0.967 [0.961-0.979] slower** |
| 1M inverse | avx512 | 9.315 | 5.727 | **1.629 [1.621-1.638] faster** |
| 1M inverse | avx2 | 7.865 | 7.670 | 1.024 [1.019-1.027] unresolved (15 repetitions, above 1) |
| 1M inverse | sse2 | 8.338 | 8.587 | **0.973 [0.967-0.978] slower** |
| 2M inverse | avx512 | 18.64 | 11.42 | **1.623 [1.613-1.637] faster** |
| 2M inverse | avx2 | 15.94 | 15.54 | **1.025 [1.021-1.031] faster** |
| 2M inverse | sse2 | 17.00 | 17.42 | **0.972 [0.969-0.978] slower** |

HEAD's f32 inverse at avx512 was slower than at avx2 at every size (the same bound). At sse2 the f32 tile (`Vec8f`) is
two 128-bit registers: a micro-tile of eight k1 steps holds 32 of them in its two planes, twice the register file,
and its spills cost more than the element stores' eight rows did; the f64 sse2 cells above sat at 0.987 to 0.995 for
the same reason. Hence the kept change.

## Against the pre-row engine (f64 forward, prof_fft_kernel, ms)

| n | pre-row | candidate avx512 | ratio | candidate avx2 | ratio |
|---|---|---|---|---|---|
| 2M | 22.52 | 18.55 | 1.213 [1.204-1.225] faster | 21.78 | 1.036 [1.034-1.045] faster |
| 4M | 49.66 | 40.42 | 1.230 [1.202-1.231] faster | 46.43 | 1.058 [1.039-1.077] faster |

HEAD's avx512 four-step had been 0.44x (2M) and 0.48x (4M) of the pre-row engine on this machine (50.7 and 103.6 ms),
sizes the first board's pre-row rows (1K to 1M) did not reach.

## The f64 forward against FFTW, MKL and pocketfft (the candidate, one run, not a board)

`bench_fft_vs_refs` of the candidate once at each level (the program's best-of-repetitions milliseconds; every side
passed its gate). The first board's run of HEAD at avx512 is the "HEAD" column:

| n | level | Cerid | HEAD | FFTW | MKL | pocketfft | FFTW / Cerid | MKL / Cerid |
|---|---|---|---|---|---|---|---|---|
| 1M (six-step) | avx512 | 8.564 | 6.923 | 10.510 | 11.716 | 12.704 | 1.23 | 1.37 |
| 2M | avx512 | 21.539 | 49.659 | 22.054 | 25.191 | 35.072 | **1.02** (HEAD 0.36) | 1.17 |
| 4M | avx512 | 56.212 | 111.534 | 48.543 | 85.407 | 93.909 | **0.86** (HEAD 0.35) | 1.52 |
| 8M | avx512 | 116.600 | 266.988 | 116.177 | 113.179 | 233.796 | **1.00** (HEAD 0.32) | 0.97 |
| 1M (six-step) | avx2 | 9.875 | - | 11.151 | 11.731 | 12.321 | 1.13 | 1.19 |
| 2M | avx2 | 27.138 | - | 21.768 | 25.830 | 35.887 | 0.80 | 0.95 |
| 4M | avx2 | 61.664 | - | 47.170 | 75.557 | 88.286 | 0.76 | 1.23 |
| 8M | avx2 | 134.805 | - | 117.371 | 114.544 | 228.174 | 0.87 | 0.85 |

Single runs move by 10% to 20% at these working sets on this machine (the first board's note), so these rank and do
not measure; the boards above are the measurement. In both runs the program's own arm rows put the six-step ahead of
the four-step at 4M (45.5 against 52.9 ms at avx512, 52.2 against 59.9 at avx2), where the canonical plan and the
avx2 level plan run the four-step.

## After the change: where the time goes now

The candidate's phase counters (the profile tree built from the candidate, Mcyc a transform, same loops as above):

| transform | level | P1 gather | P1 sub-FFT | **P1 twiddle + store** | P2 gather | P2 sub-FFT | P2 scatter | ms |
|---|---|---|---|---|---|---|---|---|
| f32 2M forward | avx512 | 3.7 | 5.9 | **5.0** (was 21.0) | - | - | 12.0 (45%) | 12.9 |
| f32 2M forward | avx2 | 3.6 | 7.9 | **5.5** (was 6.5) | - | - | 13.7 (45%) | 14.7 |
| f32 4M forward | avx512 | 8.4 | 11.4 | **9.3** (was 42.2) | 5.7 | 11.9 | 20.2 (30%) | 32.5 |
| f32 4M forward | avx2 | 8.1 | 16.4 | **11.4** (was 13.3) | 5.6 | 16.5 | 20.2 (26%) | 37.1 |
| f64 2M forward | avx512 | 7.2 | 11.0 | **7.9** (was 83.1) | - | - | 19.1 (42%) | 19.0 |
| f64 2M forward | avx2 | 6.9 | 14.6 | **7.7** (was 9.3) | - | - | 23.4 (45%) | 22.2 |
| f64 4M forward | avx512 | 14.7 | 22.2 | **18.6** (was 166.0) | 14.9 | 21.7 | 10.2 | 41.4 |
| f64 4M forward | avx2 | 15.0 | 32.8 | **19.3** (was 21.5) | 15.2 | 31.6 | 10.0 | 50.9 |

`perf stat` of the same loops: f64 2M avx512 2.34G cycles (was 5.36G), `l1d_pend_miss.fb_full` 0.65G (was 3.65G),
store-buffer stalls 0.71G (was 2.56G); f32 2M avx512 2.12G cycles (was 2.75G), RFOs 59M (was 104M, now avx2's 57M),
store-buffer stalls 0.66G (was 1.11G). The f32 2M forward at avx512 (`prof_fft_f32.sh`, the copy included) is now
65.9% backend bound (49.9% memory, 16.8% core), 23.9% retiring, 4.7% frontend. At 2M the largest phase is P2 (the
fused 32x32 gather, its sub-FFT and the scatter to the output, 42% to 45%), at 4M the two sub-FFT passes.

## The kept change: the line path at avx2 and avx512

The kept change takes the line path where the tile is one register (avx2) or two tiles (avx512,
`CRD_SIMD_HAS_AVX2`) and keeps HEAD's element stores below, inside the first candidate's own loop, so avx2 and avx512
run its code. A second structure, the step's twiddle as a lambda both paths called, ran the f64 128K inverse at avx2
4.4% slower than the first candidate's code rebuilt on the same base (0.956 [0.955-0.965]; avx512 level, 1.003; one
interleaved board of the three builds, all with the same digest): the same cycles (3.22G against 3.23G) at a lower
clock (3.07 to 3.15 against 3.21 GHz, `perf stat` cycles over task-clock, alternating processes). It was not kept.

HEAD and the kept candidate rebuilt on one base (main's tree at the delivery), every board `--same`:

| transform | avx512 | avx2 | sse2 |
|---|---|---|---|
| f64 2M forward | **2.720 [2.688-2.793]** | **1.029 [1.026-1.033]** | 1.000 [0.996-1.002] level |
| f64 128K inverse | **2.593 [2.566-2.634]** | 1.017 [1.015-1.026] (15 reps, above 1) | 0.994 [0.992-1.001] level |
| f64 1M inverse | **2.353 [2.350-2.377]** | 1.014 [1.002-1.018] level | 0.999 [0.990-1.007] level |
| f32 128K inverse | **1.734 [1.724-1.750]** | 1.013 [1.006-1.017] level | 0.991 [0.981-0.996] level |
| f32 2M inverse | **1.630 [1.604-1.643]** | 1.024 [1.019-1.028] (15 reps, above 1) | 0.999 [0.992-1.007] level |
| f32 2M forward | **1.633 [1.585-1.670]** | **1.059 [1.054-1.073]** | 0.996 [0.991-1.004] level |
| f32 4M forward | **1.479 [1.458-1.489]** | **1.054 [1.049-1.064]** | 0.999 [0.995-1.004] level |

The f32 forward against MKL on the same boards (ms; MKL's time over Cerid's): at avx512 HEAD 17.73 and 41.32, the
candidate 10.90 and 27.95, MKL 12.76 and 31.33, so **1.17x and 1.12x MKL** (HEAD 0.72x, 0.76x); at avx2 the candidate
12.58 and 31.40 against MKL 12.54 and 30.89, **1.00x and 0.98x** (HEAD 0.94x, 0.93x).

## Verdict

The avx512 four-step was bound by its phase-1 store pattern, not by its memory passes: the twiddled columns went
to `lanes` tbuf rows a step, one complex each, which at avx512's eight f64 or sixteen f32 rows exhausts the fill
buffers (f64, non-temporal) or one L1 set (f32). Written as whole lines from a register transpose at avx2 and avx512,
with the same bits, the f64 four-step runs 2.35x to 2.74x faster at avx512 (2M and 4M forward, 128K to 2M inverse)
and 1.01x to 1.03x at avx2, the f32 transforms 1.46x to 1.73x at avx512 and 1.01x to 1.06x at avx2; sse2 keeps the
element stores and is level (0.991 to 1.000). The f32 forward now leads MKL at 2M and 4M at avx512 (1.12x to 1.18x
over the boards) and is level with it at avx2 (0.98x to 1.00x); the f64 forward leads the pre-row engine by 1.21x
to 1.23x at avx512 where HEAD had been at 0.44x and 0.48x.
