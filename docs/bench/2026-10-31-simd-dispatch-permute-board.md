# 2026-10-31 — SIMD-DISPATCH: the permute board against the pre-row engine and HPTT

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); session:
> [2026-10-31](../sessions/2026-10-31-simd-dispatch-permute-board.md); before: the
> [v14-d board](2026-07-02-v14d-permute-vs-hptt.md).

- **Machine:** Intel i9-14900K, WSL2 Ubuntu 24.04, linux-gcc-release lane tree (GCC 13.3). One thread on CPU 4;
  8 workers on CPUs 0,2,..,14 (one thread a P-core); 16 workers on CPUs 0-15. Busy host by the user's order of
  2026-10-31: `scripts/build-bench.py host` before each part read other Windows processes at 0.6 to 1.1 cores
  (dota2) and 3.7 to 16.2 of 63.7 GiB available (flagged below the 25% mark); Linux 30.2 of 31.2 GiB available.
- **Pre-row:** `2f2aed72`, its own tree. **HEAD:** `8daab825`, then the two folds below.
- **Harnesses:** a probe (one source, `permute_probe.cpp`, compiled against each tree with that tree's own
  `smoke_hesap_tensor` commands) runs `detail::permute_impl`, each engine's production permute, over
  `bench_permute`'s three cases and four types, every output gated against a reference walk; the two binaries run
  in alternating order (pre-row first in even rounds), medians of 10 timed calls each, ratios taken within a round.
  Then `scripts/run_permute_bench.sh`'s `bench_permute` calls (its arms and HPTT `942538649b51` from the pinned
  archive, at the host's AVX, interleaved round by round in one process, each gated bit for bit).
- Ratios: pre-row ms over HEAD ms (above 1 HEAD faster), median [min-max] over the rounds.

## Before: two regressions

One thread, six rounds: HEAD trailed pre-row in every round at f32 2D 0.766 [0.733-0.946], c64 2D 0.789
[0.750-0.817], c32 2D 0.847 [0.762-0.900], f32 4D 0.737 [0.487-0.924] and c64 3D 0.734 [0.695-0.925].

`bench_permute`'s arms named the cause: the rules' next-tile prefetch (distance 1). Without it (`distance 0`) every
case ran faster, x rules:

| arm, level | 2D f32 / f64 / c32 / c64 | 4D | 3D |
|---|---|---|---|
| distance 0, avx2, one thread | 1.454 / 1.917 / 1.919 / 1.653 | 1.377 / 1.312 / 1.315 / 1.360 | 1.714 / 1.783 / 1.813 / 1.552 |
| distance 0, sse4.2, one thread | 1.222 / 1.732 / 1.749 / 1.273 | 1.307 / 1.333 / 1.354 / 1.287 | 1.343 / 1.568 / 1.551 / 1.205 |
| distance 0, sse2, one thread | 1.205 / 1.803 / 1.723 / 1.271 | 1.282 / 1.353 / 1.323 / 1.252 | 1.404 / 1.560 / 1.583 / 1.244 |
| distance 0, avx2, 8 workers | 0.955 / 1.103 / 1.148 / 1.057 | 1.092 / 1.116 / 1.097 / 1.063 | 1.058 / 1.138 / 1.117 / 1.057 |
| distance 0, avx2, 16 workers | 1.100 / 1.094 / 1.104 / 1.180 | 1.072 / 1.054 / 1.088 / 1.103 | 1.086 / 1.135 / 1.129 / 1.137 |

47 of 48 cells; the 8-worker f32 2D cell (0.955) is 1.100 at 16 workers. With no prefetch as the rule, the
multithreaded f32 cases still trailed pre-row in every round (4 rounds each): 2D 0.698 and 0.717 (8, 16 workers),
4D 0.697 and 0.836, 3D 0.865 and 0.856. The arms' staged strips beat the unstaged tiles in all 24 multithreaded
cells, x rules, f32 / f64 / c32 / c64:

| workers | 2D | 4D | 3D |
|---|---|---|---|
| 8 | 1.618 / 1.272 / 1.275 / 1.175 | 1.759 / 1.207 / 1.262 / 1.071 | 1.536 / 1.345 / 1.331 / 1.204 |
| 16 | 1.827 / 1.308 / 1.297 / 1.254 | 1.852 / 1.230 / 1.336 / 1.154 | 1.678 / 1.332 / 1.340 / 1.260 |

## The folds

- `PermutePick::distance` 0: no next-tile prefetch (the hint, distance and destination-prefetch arms stay, each
  reaching the next tile).
- Each multithreaded task stages its tiles of a kernel element through its thread's frame arena
  (`PermuteArm::unstaged` is the arm).

## After

Pre-row over HEAD, HEAD with both folds (one thread: 6 rounds; 8 and 16 workers: 4 rounds each):

| case | f32 | f64 | c32 | c64 |
|---|---|---|---|---|
| 1t 2D | 1.048 [0.930-1.139] | 1.395 [1.256-1.645] | 1.572 [1.451-1.635] | 1.195 [1.115-1.396] |
| 1t 4D | 1.133 [0.972-1.232] | 2.188 [2.089-2.530] | 2.449 [2.160-2.574] | 1.511 [1.333-1.635] |
| 1t 3D | 1.003 [0.981-1.091] | 1.117 [0.585-2.428] | 1.054 [0.613-2.207] | 1.058 [0.610-1.706] |
| 8 workers 2D | 1.146 [0.995-1.207] | 1.839 [1.760-1.878] | 1.851 [1.810-1.938] | 1.523 [1.500-1.649] |
| 8 workers 4D | 1.247 [1.201-1.401] | 1.512 [1.487-1.564] | 1.628 [1.442-1.741] | 1.218 [1.040-1.388] |
| 8 workers 3D | 1.333 [1.095-1.656] | 1.796 [1.531-2.368] | 1.533 [1.446-2.049] | 1.647 [1.402-1.890] |
| 16 workers 2D | 1.214 [1.140-1.356] | 1.607 [1.533-2.097] | 1.668 [1.593-1.933] | 1.558 [1.360-1.879] |
| 16 workers 4D | 1.192 [0.988-1.296] | 1.477 [1.247-1.580] | 1.518 [1.404-1.582] | 1.385 [1.270-1.506] |
| 16 workers 3D | 1.229 [1.182-1.519] | 1.665 [1.424-1.917] | 1.389 [1.293-1.997] | 1.399 [1.286-1.769] |

No cell trails pre-row in every round. The one-thread 3D cells of the 1 and 2 GiB tensors carry the board's widest
spread (0.59 to 2.43 between rounds) on a host with 3.7 to 16 GiB of Windows memory free.

HPTT against the rules (HPTT ms over Cerid ms, above 1 Cerid faster), f32 / f64 / c32 / c64:

| level, threads | 2D | 4D | 3D |
|---|---|---|---|
| avx2, 1 | 1.07 / 1.32 / 2.51 / 1.77 | 0.97 / 1.13 / 1.95 / 1.32 | 1.26 / 0.99 / 1.96 / 1.59 |
| avx2, 8 | 1.06 / 1.29 / 1.87 / 2.11 | 1.77 / 1.29 / 1.54 / 1.57 | 1.37 / 1.41 / 2.09 / 2.09 |
| avx2, 16 | 1.02 / 1.06 / 1.75 / 1.73 | 1.14 / 1.03 / 1.41 / 1.43 | 1.34 / 1.30 / 2.01 / 2.13 |
| sse4.2, 1 | 0.84 / 1.33 / 2.23 / 1.19 | 0.75 / 1.06 / 1.93 / 1.20 | 0.90 / 0.84 / 1.70 / 1.25 |
| sse2, 1 | 0.87 / 1.28 / 2.24 / 1.18 | 0.73 / 1.12 / 2.00 / 1.15 | 0.93 / 0.81 / 1.71 / 1.24 |
| sse4.2, 8 | 1.36 / 1.20 / 1.91 / 1.69 | 1.61 / 1.11 / 1.62 / 1.42 | 1.37 / 1.26 / 2.11 / 2.05 |
| sse2, 8 | 3.16 / 1.75 / 1.70 / 1.58 | 1.33 / 1.12 / 2.86 / 1.72 | 1.18 / 1.32 / 1.62 / 2.18 |
| portable, 1 | 0.71 / 0.99 / 1.83 / 1.31 | 0.48 / 0.45 / 0.92 / 0.86 | 0.60 / 0.68 / 1.51 / 1.40 |
| portable, 8 | 1.92 / 1.49 / 1.15 / 1.31 | 0.97 / 0.77 / 1.00 / 1.13 | 0.52 / 0.74 / 1.16 / 1.43 |

HPTT runs its AVX kernels at every Cerid level. At avx2 Cerid leads HPTT in every cell but one-thread f32 4D
(0.97) and f64 3D (0.99); at sse4.2 and sse2 on one thread the 16-byte transposes trail HPTT's 32-byte kernels on
f32 (0.73 to 0.93) and f64 3D (0.81, 0.84), and lead on 8 workers but portable's. The portable level's scalar tiles
trail on one-thread f32 and f64 (0.45 to 0.99) and lead on complex 2D and 3D; tile 32 beats the rule's 64 there by
1.56x to 2.21x on 4D, nowhere else.

After the folds no arm beats the rules in every cell of a level: at avx2 non-temporal stores win one-thread 4D
f64/c32/c64 (1.14 to 1.39) and lose 2D and 3D (0.52 to 0.97), tile 32 wins one-thread c64 2D and 4D (1.16, 1.10)
and loses f32; the 8- and 16-worker arms sit within 0.83 to 1.19 of the rules, none ahead in every cell.

Logs: `~/permute-board/ab` in WSL (tags a: before; b: the lower levels before; c: no prefetch; d and e: both folds).
