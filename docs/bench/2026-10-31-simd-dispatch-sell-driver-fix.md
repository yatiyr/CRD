# 2026-10-31 — SIMD-DISPATCH: the SELL driver fix against the pre-row engine and Eigen

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); session:
> [2026-10-31](../sessions/2026-10-31-simd-dispatch-sell-driver-fix.md); before: the
> [five-pass SELL board](2026-10-31-simd-dispatch-sell-board.md), same host, probes and parser.

- **Machine:** Intel i9-14900K, WSL2, one-thread runs on CPU 4. Host by the user's order of 2026-10-31 (ratios side by
  side, not a quiet machine): `scripts/build-bench.py host` read other Windows processes at 0.6 to 0.8 cores (dota2)
  and 12.9 to 14.7 of 63.7 GiB available (flagged below the 25% mark); Linux 30.2 of 31.2 GiB available.
- **HEAD:** `85c5c1bd` (the slice cursor and the pair-free loop) and `05dbbc1c` (a Delta16 build's rows grouped by
  form), the linux-gcc-release lane tree (GCC 13.3). Pre-row: `2f2aed72`.

## Same matrix, same process (the primary evidence)

`sell_ab2` as in the five-pass record: HEAD's production and 2f2aed72's AVX2 loop on HEAD's U32 matrix, rounds
alternating, medians of 21 rounds at each of eight layouts. Pre-row/HEAD, above 1 HEAD faster, median [min-max]:

| type | matrix | before the fix | after |
|---|---|---|---|
| f64 | uniform-4, 15,420 rows | 0.782 [0.776-0.820] | 1.017 [0.987-1.049] |
| f64 | banded-5, 13,107 rows | 0.802 [0.781-0.826] | 1.087 [1.054-1.141] |
| f64 | uniform-16, 4,946 rows | 1.010 [0.950-1.023] | 1.099 [1.056-1.130] |
| f64 | uniform-4, 160,000 rows | 0.930 [0.917-0.942] | 0.984 [0.969-1.025] |
| f32 | uniform-4, 23,831 rows | 0.976 [0.963-1.000] | 1.158 [1.103-1.193] |
| f32 | banded-5, 20,000 rows | 0.954 [0.942-0.961] | 1.189 [1.146-1.205] |
| f32 | uniform-16, 4,946 rows | 1.162 [1.123-1.198] | 1.253 [1.223-1.272] |
| f32 | uniform-4, 160,000 rows | 0.958 [0.945-0.981] | 1.115 [1.093-1.134] |

`sell_bisect` (scratch) copied HEAD's lone-slice path (H) and changed one thing at a time, f64 scalar loads, three
layouts, time over production's: H 1.01 to 1.05, its rows written from the register 1.00 to 1.05, a view holding only
`vals` 1.02 to 1.05, the slices' offsets carried on with the view in locals (O) 0.80 to 0.85, O without the pair test
0.74 to 0.81, the flat loop 0.66 to 0.70. After the fix the same arms read: production 1.0, O 0.99 to 1.03, O without
the pair test 0.91 to 0.95.

`sell_auto` (scratch): HEAD's Auto build (Delta16 where the slices fit) against its U32 build and the pre-row loop,
one process, 15 rounds of best-of-15:

| matrix | Delta16 slices | U32/Auto before | after | pre-row/Auto after |
|---|---|---|---|---|
| uniform-4, 130,000 rows (between the L2 and the L3) | 14,606 then 26,606 of 32,500 | 0.740 | 0.895 | 0.995 |
| banded-5, 110,000 rows | all 27,500 | 1.218 | 1.241 | 0.980 |
| uniform-4, 280,000 rows (half the L3) | 114 then 14,037 of 70,000 | 0.950 | 0.938 | 1.023 |

## The board, one pass after both fixes (pass 7)

The probe (two binaries, as before) against the five passes' median [range] and noise. f64 uniform-4 half L2 1.096
(0.866 [0.831-0.925], noise 0.093), between the L2 and the L3 0.912 (0.814 [0.784-0.879], 0.172), banded-5 half L2
1.261 (0.969), power-law half L2 1.019 (0.918, slower in every pass); f32 uniform-4 half L3 1.047 (0.910) and 1M 1.127
(0.941). One cell reads lower: f32 uniform-4 half L2 0.705 (0.782 [0.726-0.861], noise 0.200), where the probe's HEAD
binary takes 0.0382 ms while the bench's production takes 0.0254 ms on the same matrix in the same pass and the A/B
above puts HEAD 1.10x to 1.19x ahead: the probe binary's layout, not the engine. A single pass carries no spread of
its own; the A/B is the repetition.

Against Eigen (the bench, one process), Eigen ms over production ms, geometric mean over the 20 cells [lowest cell];
in brackets the five passes' means:

| level | f64 | f32 | c64 | c32 |
|---|---|---|---|---|
| avx2 (detected) | 1.211 [0.959] (1.162) | 1.454 [1.029] (1.392) | 1.438 [1.285] (1.391) | 1.636 [1.270] (1.569) |
| sse4.2 | 1.134 [0.965] (1.101) | 1.267 [1.048] (1.214) | 1.136 [1.028] (1.134) | 1.480 [1.160] (1.399) |
| sse2 | 1.138 [0.963] (1.098) | 1.256 [1.023] (1.219) | 1.150 [1.027] (1.124) | 1.480 [1.147] (1.396) |
| portable | 1.079 [0.919] (1.001) | 0.971 [0.731] (0.946) | 1.074 [0.935] (1.053) | 1.063 [0.895] (1.068) |

Behind Eigen at avx2 only f64 uniform-4 between the L2 and the L3 (0.959, was 0.897), where the Auto build's grouped
Delta16 slices still run 10% behind its U32 build: Auto's L2 cut, at parity with pre-row. At portable f32 the U32 body
trails its own Delta16 arm at half the L2 (1.32x to 1.47x on uniform-16, uniform-4 and banded-5; 1.41x to 1.51x in the
five passes), the cell behind Eigen there.
