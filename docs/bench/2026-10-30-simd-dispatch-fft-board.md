# 2026-10-30 — SIMD-DISPATCH row-close board: the fft forward against the pre-row engine, every level

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); session:
> [2026-10-30](../sessions/2026-10-30-simd-dispatch-fft-board-whole-codelets.md); acceptance: the user's order of
> [2026-10-30](../sessions/2026-10-30-simd-dispatch-peer-pins-board-acceptance.md#user-decisions-recorded).
> Board 2 ([session](../sessions/2026-10-30-simd-dispatch-fft-board-method.md), [below](#board-2-one-program-on-both-engines))
> supersedes this board's f32 verdict and its two method claims.

- **Machine:** Intel i9-14900K, WSL2 (kernel 6.6.87.2, 32 logical CPUs, 31 GiB, 30 GiB available), Windows 11 host.
  `scripts/build-bench.py host` before and after each board: other processes 0.5 to 1.1 cores (the busiest Studio
  One, 0.2 to 0.6), Windows memory 9.6 to 14.3 of 63.7 GiB available, below the 25% mark the check flags, held by
  the user's own applications, which this session did not close. Every run pinned to CPU 4 (`taskset -c 4`), the
  peers held to one thread (`MKL_NUM_THREADS=1 OMP_NUM_THREADS=1`).
- **HEAD:** the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO; kernel units per level without LTO,
  ADR-0134 §6), `bench_fft_vs_refs` (f64) and `bench_fft_f32` as `scripts/run_bench_fft.sh` builds them;
  `CRD_CPU_LEVEL` selects the level, `cpu_level_record --expect` printed each level line (detected avx2, kernel cap
  avx512). Before: `aa304119`; after: `d8475439`.
- **Pre-row:** `2f2aed72`, the parent of `809e720c` (the row's first engine commit), exported with `git archive`
  into `~/prerow-2f2aed72`; its header-only fft, its two programs and its core, memory, vm, log, containers and math
  sources compiled by hand with that commit's linux-gcc-release flags (`-O3 -DNDEBUG -flto=auto -mavx2 -mfma
  -msse4.2 -mf16c -fno-fast-math -ffp-contract=off -mfpmath=sse -DCRD_SIMD_TARGET=2 -DCRD_DETERMINISTIC_FP=1`,
  crd-core with `CRD_FAULT_SEAMS=1`, `build_config.hpp` from the same preset). It built the avx2 level only.
- **Peers:** Intel MKL 2020.0.4 (`DFTI_SINGLE`/`DOUBLE`, in place), FFTW 3 (`FFTW_MEASURE`), pocketfft
  `2e88a5b0addf`.
- **Board:** five passes; each runs pre-row, then HEAD at detected, sse4.2, sse2 and portable, for both programs
  (best of each program's repetitions per size). A cell is the median of the five passes [the lowest - the highest];
  the range is the run-to-run noise of that binary.

## The avx2 level against the pre-row engine (Cerid ms)

f64:

| n | pre-row (board 1) | HEAD before | pre-row (board 2) | after | after / pre-row | MKL (after) |
|---|---|---|---|---|---|---|
| 65536 | 0.176 [0.172-0.180] | 0.174 [0.171-0.179] | 0.177 [0.166-0.183] | 0.177 [0.176-0.179] | 1.00 | 0.157 |
| 131072 | 0.483 [0.478-0.518] | 0.581 [0.546-0.584] | 0.509 [0.456-0.532] | 0.501 [0.474-0.511] | 0.98 | 0.433 |
| 262144 | 1.061 [1.027-1.154] | 1.296 [1.222-1.349] | 1.054 [1.028-1.174] | 1.162 [1.040-1.228] | 1.10 | 0.928 |
| 524288 | 2.576 [2.498-2.718] | 3.078 [3.036-3.194] | 2.516 [2.424-2.570] | 2.613 [2.546-2.823] | 1.04 | 2.036 |
| 1048576 | 6.293 [6.154-6.517] | 6.108 [6.047-6.508] | 6.380 [6.194-6.987] | 6.125 [5.719-6.577] | 0.96 | 5.434 |
| 2097152 | 14.518 [13.325-15.227] | 16.108 [15.818-16.194] | 13.552 [13.351-13.899] | 13.465 [13.240-14.581] | 0.99 | 12.611 |
| 4194304 | 28.253 [26.495-28.934] | 32.488 [32.196-32.867] | 28.710 [27.135-29.633] | 30.279 [29.621-30.582] | 1.05 | 27.599 |
| 8388608 | 63.462 [62.398-63.806] | 70.168 [67.270-71.407] | 63.583 [59.462-65.561] | 64.650 [62.110-67.062] | 1.02 | 62.084 |

f32 (the planned forward; pre-row's one route):

| n | pre-row (board 1) | HEAD before | pre-row (board 2) | after | after / pre-row | MKL (after) |
|---|---|---|---|---|---|---|
| 65536 | 0.068 [0.068-0.072] | 0.077 [0.076-0.079] | 0.068 [0.064-0.070] | 0.077 [0.073-0.080] | 1.13 | 0.064 |
| 131072 | 0.189 [0.183-0.193] | 0.230 [0.227-0.237] | 0.192 [0.179-0.195] | 0.206 [0.178-0.211] | 1.07 | 0.214 |
| 262144 | 0.419 [0.407-0.446] | 0.543 [0.510-0.625] | 0.434 [0.409-0.444] | 0.493 [0.442-0.564] | 1.14 | 0.473 |
| 524288 | 0.926 [0.885-0.963] | 1.375 [1.355-1.493] | 0.952 [0.887-0.974] | 1.170 [1.053-1.188] | 1.23 | 0.988 |
| 1048576 | 2.370 [2.296-2.599] | 3.007 [2.868-3.191] | 2.475 [2.226-2.616] | 2.749 [2.485-2.905] | 1.11 | 2.118 |
| 2097152 | 6.358 [6.329-6.596] | 7.865 [7.495-8.193] | 6.371 [5.787-6.532] | 6.936 [6.453-7.066] | 1.09 | 5.271 |
| 4194304 | 14.020 [13.513-14.167] | 16.208 [15.708-16.668] | 13.880 [13.257-14.051] | 14.122 [13.791-15.042] | 1.02 | 13.056 |

Below 64K both types are within the 3-decimal resolution of the programs' millisecond columns (their in-process
MKL ratios, f32 1K to 32K: 0.59x to 0.81x pre-row, 0.82x to 1.75x after).

**Before the fix** HEAD was slower than pre-row beyond noise on every f64 size from 128K to 8M but 1M (the lowest
HEAD pass above the highest pre-row pass; +11% to +22% on the medians) and on f32 from 64K to 4M (+13% to +48%).

## Cause: the codelet split

Commit `20c9f2a1` cut every generated tile body into non-inlined parts of at most 150 lines passing their live values
through a slot array, on every level, for MSVC's compile cost at sse2 and portable. Interleaved A/B builds (three
passes each, the lowest - the highest; the tree's own avx2 compile commands, the codelet objects linked ahead of
`libcrd-hesap-fft.a`). Run 1, the batched and twiddle units regenerated whole:

| f64 ms | pre-row | HEAD | batched and twiddle whole |
|---|---|---|---|
| 262144 | 1.165-1.235 | 1.483-1.550 | 1.178-1.370 |
| 4194304 | 28.10-28.94 | 31.07-32.52 | 31.39-32.09 |

Run 2, every codelet unit whole (the hierarchical ones re-split from `20c9f2a1^` with no budget) and HEAD's parts
force-inlined:

| ms | pre-row | HEAD | every unit whole | parts force-inlined |
|---|---|---|---|---|
| f64 262144 | 1.038-1.113 | 1.170-1.482 | 1.224-1.290 | 1.168-1.279 |
| f64 4194304 | 27.41-29.50 | 32.26-33.34 | 28.53-29.66 | 28.49-30.02 |
| f32 524288 | 0.994-1.072 | 1.432-1.464 | 1.185-1.279 | 1.469-1.537 |
| f32 1048576 | 2.640-2.898 | 2.978-3.156 | 2.807-2.927 | 3.314-3.521 |

The four-step sizes need the hierarchical units whole too; force-inlined parts keep the slot arrays on f32.
`d8475439` compiles each codelet whole where a lane is one register (optimized avx2 and avx512). MSVC 19.51 `/O2`
(win-release's commands): a whole batched unit 136 s and 2.15 GB at avx2, 106 s and 4.19 GB at avx512 (split: 2.6 s,
0.15 GB); a hierarchical unit 5.8 to 6.7 s, a twiddle unit 1.7 s.

## The other levels (after; median ms, MKL at its own ISA)

| n | f64 sse4.2 | f64 sse2 | f64 portable | f64 sse2 vs MKL | f32 sse4.2 | f32 sse2 | f32 portable | f32 sse2 vs MKL |
|---|---|---|---|---|---|---|---|---|
| 1024 | 0.016 | 0.016 | 0.010 | 0.06x | 0.006 | 0.006 | 0.009 | 0.17x |
| 16384 | 0.474 | 0.469 | 0.266 | 0.06x | 0.206 | 0.219 | 0.238 | 0.06x |
| 131072 | 5.457 | 5.491 | 2.746 | 0.08x | 1.660 | 1.755 | 2.664 | 0.13x |
| 1048576 | 46.310 | 45.056 | 26.709 | 0.11x | 17.442 | 17.550 | 28.149 | 0.14x |
| 4194304 | 191.577 | 191.505 | 124.225 | 0.15x | 64.705 | 63.953 | 109.627 | 0.21x |

Pre-row had no such level, so these have no pre-row reference. This board called two method defects that board 2
corrects: MKL runs AVX2 on this host (`MKL_VERBOSE`), and a peer at the host's ISA at every forced level is ADR-0134
§8's method (amended 2026-10-29); glibc's FMA ifunc is right for portable (its targets have the FPU's fma) and is now
hidden on sse4.2 and sse2. At sse2 and sse4.2 the planner's algorithms lose to others in
`bench_fft_f32`'s arms: Stockham 1.2x to 1.7x faster at 1K to 64K and the four-step 1.2x to 1.26x at 128K to 1M
(pass 1, sse2).

## Verdict

- **f64, avx2: accepted.** After / pre-row 0.96 to 1.10, every after range overlapping its pre-row range.
- **f32, avx2: not accepted.** 64K +13% (0.073-0.080 against 0.064-0.070) and 512K +23% (1.053-1.188 against
  0.887-0.974) stay outside the noise; 256K, 1M and 2M +9% to +14% with overlapping ranges.
- **sse4.2, sse2, portable: provisional** until the method defects are fixed; the plan per level is a decision (one
  plan on every level keeps the family's cross-level bits, ADR-0134 §7).

## Board 2: one program on both engines

The f32 rows above compare two programs: HEAD's `bench_fft_f32` timed the input copy with each transform (and
MKL's), pre-row's did not; both f64 programs timed it. Board 2 builds pre-row's two programs against HEAD's lane
tree (each its compile and link command with the source swapped) and runs them on both engines; HEAD's two programs
now copy the input before each timed call. Same host (other processes 0.5 to 0.8 cores, Windows 10.3 to 11.4 of
63.7 GiB available, the user's applications), CPU 4, five passes, each: pre-row's f32 and f64 programs on pre-row's
engine, then on HEAD's (`62d9aedd` with the untimed copy), then HEAD's programs at detected, sse4.2, sse2 (glibc's
features above the level hidden, `run-lane-bench.sh`) and portable.

The avx2 level, pre-row's program, median [lowest - highest] ms (64K and below at the programs' 3-decimal
resolution):

| n | f32 pre-row | f32 HEAD | HEAD / pre-row | f64 pre-row | f64 HEAD | HEAD / pre-row |
|---|---|---|---|---|---|---|
| 65536 | 0.069 [0.067-0.072] | 0.069 [0.068-0.073] | 1.00 | 0.171 [0.166-0.179] | 0.176 [0.170-0.185] | 1.03 |
| 131072 | 0.202 [0.185-0.224] | 0.176 [0.170-0.203] | 0.87 | 0.520 [0.453-0.547] | 0.513 [0.493-0.528] | 0.99 |
| 262144 | 0.456 [0.429-0.543] | 0.437 [0.422-0.502] | 0.96 | 1.163 [1.039-1.258] | 1.180 [1.116-1.222] | 1.01 |
| 524288 | 1.090 [0.960-1.240] | 1.018 [0.991-1.159] | 0.93 | 2.675 [2.486-2.880] | 2.984 [2.825-3.156] | 1.12 |
| 1048576 | 2.912 [2.519-3.374] | 3.018 [2.403-3.257] | 1.04 | 6.165 [5.990-6.251] | 6.070 [5.953-6.216] | 0.98 |
| 2097152 | 6.247 [5.980-6.400] | 6.387 [6.071-6.738] | 1.02 | 14.319 [13.521-14.616] | 14.292 [13.821-14.573] | 1.00 |
| 4194304 | 13.480 [12.865-13.679] | 13.397 [12.742-13.677] | 0.99 | 27.929 [26.860-29.690] | 28.242 [28.107-29.753] | 1.01 |
| 8388608 | | | | 64.735 [61.544-65.531] | 65.619 [62.197-67.671] | 1.01 |

Every range overlaps its pre-row range. The widest median gap, f64 512K (+12%), overlaps; HEAD's own f64 program
reads 2.440 [2.289-2.511] there with the copy untimed.

HEAD's programs, every level (median ms; peers from the detected arm, at the host's ISA):

| n | f64 avx2 | f64 sse4.2 | f64 sse2 | f64 portable | FFTW | pocketfft | MKL | f32 avx2 | f32 sse4.2 | f32 sse2 | f32 portable | f32 MKL |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1024 | 0.001 | 0.016 | 0.016 | 0.009 | 0.001 | 0.003 | 0.001 | 0.001 | 0.006 | 0.006 | 0.009 | 0.000 |
| 16384 | 0.032 | 0.455 | 0.452 | 0.258 | 0.027 | 0.071 | 0.025 | 0.015 | 0.216 | 0.214 | 0.230 | 0.012 |
| 131072 | 0.443 | 5.244 | 5.233 | 2.818 | 0.302 | 0.751 | 0.352 | 0.175 | 1.694 | 1.707 | 2.635 | 0.151 |
| 1048576 | 5.240 | 44.679 | 45.820 | 27.000 | 6.142 | 9.410 | 4.082 | 2.554 | 16.836 | 16.890 | 28.359 | 1.933 |
| 4194304 | 26.052 | 185.390 | 185.680 | 118.003 | 32.590 | 61.555 | 26.169 | 13.116 | 61.494 | 61.518 | 105.134 | 11.825 |

Hiding glibc's FMA left sse4.2 and sse2 where board 1 had them: their codelets run Vec bodies, not libm's `fma`. On
portable it made the f32 4M forward 3,357 ms against 109 (glibc's software `fma`), a machine no build targets, so
portable keeps the host's glibc. At 1M the f32 tiled four-step arm ran 2.105 [2.098-2.128] against the planned
deep-split's 2.554 [2.528-2.613] at avx2, 17.04 against 16.89 at sse2, 17.20 against 16.84 at sse4.2 and 28.43
against 28.36 at portable; it is now the f32 1M plan on every level (one run after: 1.955 ms, MKL 1.857, 0.95x).

## Verdict (board 2)

- **f32 and f64, avx2: accepted.** No size regressed beyond the pass noise against pre-row on one program; board
  1's f32 gap was the timed copy.
- **sse4.2, sse2, portable:** no pre-row level; their standing against the peers is recorded above. At sse2 and
  sse4.2 the planner's algorithms trail Stockham (1.36x to 1.69x, 1K to 64K) and the four-step (1.18x to 1.23x, 128K
  to 1M); one plan on every level keeps the family's cross-level bits (ADR-0134 §7), a plan per level would give them
  up, which is the user's decision.
