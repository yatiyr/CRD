# 2026-10-31 — SIMD-DISPATCH row-close board: SELL spmv against the pre-row engine and Eigen, every level

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); session:
> [2026-10-31](../sessions/2026-10-31-simd-dispatch-sell-board.md); acceptance: the user's orders of
> [2026-10-30](../sessions/2026-10-30-simd-dispatch-peer-pins-board-acceptance.md#user-decisions-recorded) and
> 2026-10-31 (judge ratios measured side by side; a regression is real when it holds across the repetitions).

- **Machine:** Intel i9-14900K, WSL2 (32 CPUs). One-thread runs pinned to CPU 4, job runs on every CPU, Eigen on as
  many threads. The host was not quiet, by the user's order: `scripts/build-bench.py host` before and after each pass
  read other Windows processes at 0.7 to 2.5 cores (Studio One 0.4 to 0.8, dota2 0.1 to 0.4) and 10.9 to 16.6 of
  63.7 GiB available (flagged below the 25% mark); Linux 30.2 of 31.2 GiB available, nothing else running.
- **HEAD:** `3db4573f`, the linux-gcc-release lane tree with `CRD_BUILD_HESAP_VS_REFERENCE=ON` (GCC 13.3);
  `bench_hesap_spmv_vs_reference` as `scripts/run_bench_spmv.sh` runs it at detected (avx2), sse4.2, sse2 and portable
  (the host has no AVX-512).
- **Pre-row:** `2f2aed72` (its SELL code is AVX2-only), the tree of the GEMM board.
- **Probe:** one source on both engines (the bench's generator, seeds, bands and inner counts through the production
  `to_sell`, `spmv_sell` on CPU 4 and `spmv_sell_parallel` at 1 to 32 jobs; best of 15 rounds; exit 1 when a run
  leaves the first run's bits), built with each tree's commands at `5afef104` (no sparse or bench source changed
  since). It lives in the board's scratch directory, as the GEMM probe does.
- **Board:** five passes. Each runs the probe on both engines, the engine order alternating by pass, then the bench's
  one-thread phase (production, its arms and Eigen interleaved round by round in one process) and its threads phase
  at every level, each part one supervised command under an hour. Pass 1 ran as one command and reached the 3,600 s
  deadline in its threads phase; its one-thread log was cut at the threads phase's first level header and its threads
  phase rerun on its own.
- **Cells:** median of the passes [lowest-highest]. A ratio is taken within each pass (the same host state), and its
  noise is the larger engine's spread across passes, (max - min) / median.

## One thread against the pre-row engine (the probe, avx2)

Ratio pre-row/HEAD, above 1 HEAD faster. f64:

| shape | band | pre-row ms | HEAD ms | ratio | noise | every pass |
|---|---|---|---|---|---|---|
| uniform-16 | half L2 | 0.0192 [0.0188-0.0203] | 0.0169 [0.0167-0.0178] | 1.136 [1.093-1.149] | 0.075 | faster |
| uniform-16 | L2-L3 | 0.2201 [0.2130-0.2309] | 0.2261 [0.2184-0.2400] | 0.962 [0.940-1.008] | 0.096 | mixed |
| uniform-16 | half L3 | 0.5698 [0.5333-0.7749] | 0.5461 [0.5318-0.5520] | 1.070 [0.987-1.412] | 0.424 | mixed |
| uniform-16 | 1M | 22.44 [21.13-23.12] | 22.49 [20.79-23.82] | 0.960 [0.942-1.056] | 0.135 | mixed |
| uniform-16 | 2M | 74.29 [70.82-79.86] | 75.92 [68.87-87.31] | 0.999 [0.915-1.057] | 0.243 | mixed |
| uniform-4 | half L2 | 0.0220 [0.0214-0.0233] | 0.0254 [0.0247-0.0271] | 0.866 [0.831-0.925] | 0.093 | slower |
| uniform-4 | L2-L3 | 0.2566 [0.2465-0.2906] | 0.3168 [0.3144-0.3307] | 0.814 [0.784-0.879] | 0.172 | slower |
| uniform-4 | half L3 | 0.9853 [0.7497-1.1297] | 0.9496 [0.8759-1.1093] | 1.018 [0.789-1.197] | 0.386 | mixed |
| uniform-4 | 1M | 7.088 [6.628-7.227] | 6.973 [6.524-7.474] | 0.951 [0.948-1.108] | 0.136 | mixed |
| uniform-4 | 2M | 20.73 [18.50-21.94] | 20.42 [19.22-26.62] | 0.980 [0.824-1.079] | 0.362 | mixed |
| banded-5 | half L2 | 0.0195 [0.0189-0.0205] | 0.0206 [0.0199-0.0211] | 0.969 [0.902-1.002] | 0.081 | mixed |
| banded-5 | L2-L3 | 0.1684 [0.1660-0.1771] | 0.1723 [0.1671-0.1783] | 0.977 [0.963-1.041] | 0.066 | mixed |
| banded-5 | half L3 | 0.4187 [0.3821-0.5089] | 0.3940 [0.3811-0.4203] | 1.091 [0.970-1.211] | 0.303 | mixed |
| banded-5 | 1M | 2.820 [2.725-3.103] | 2.653 [2.578-2.867] | 1.056 [1.013-1.170] | 0.134 | faster |
| banded-5 | 2M | 5.983 [5.490-7.003] | 5.326 [5.160-6.488] | 1.084 [1.033-1.179] | 0.253 | faster |
| power-law | half L2 | 0.0197 [0.0194-0.0207] | 0.0214 [0.0211-0.0220] | 0.918 [0.916-0.977] | 0.063 | slower |
| power-law | L2-L3 | 0.2542 [0.2467-0.3043] | 0.2387 [0.2324-0.2990] | 1.046 [1.018-1.094] | 0.279 | faster |
| power-law | half L3 | 0.7722 [0.6416-0.8694] | 0.6614 [0.6465-0.6700] | 1.182 [0.974-1.314] | 0.295 | mixed |
| power-law | 1M | 12.63 [12.37-14.12] | 12.74 [11.98-15.97] | 1.003 [0.813-1.055] | 0.313 | mixed |
| power-law | 2M | 39.87 [38.49-48.69] | 41.59 [37.82-44.01] | 0.990 [0.940-1.106] | 0.256 | mixed |

f32: faster in every pass at 10 of 20 cells, mixed at 7; slower in every pass at uniform-4 half L2 (0.782
[0.726-0.861], noise 0.200), half L3 (0.910 [0.773-0.990]) and 1M (0.941 [0.826-0.999]). c64 (1.04 to 1.64) and c32
(1.15 to 3.92): faster in every pass at 39 of 40 cells, c64 uniform-4 half L3 mixed.

**Verdicts.** By the 2026-10-30 rule no cell regressed beyond its noise (the contended host widened it to 6% to 48%).
By the 2026-10-31 rule six cells are slower in all five passes: f64 uniform-4 half L2 and L2-L3, f64 power-law half
L2, f32 uniform-4 half L2, half L3 and 1M. The probe is two binaries, so a layout floor
([two-binary boards](../../MEMORY.md)) can carry such a gap; the A/B below separates it.

## Same matrix, same process: HEAD's production against the pre-row loop

`sell_ab2` (scratch, built with the bench's commands in HEAD's tree) runs HEAD's production `spmv_sell` and
2f2aed72's AVX2 loop (f64: gather, slice pairs, the columns prefetched 16 ahead; f32: the eight-lane scalar gather into
a stack row) on HEAD's U32 matrix, the same x and y, alternating round by round, medians of 21 rounds. The whole set is
rebuilt behind pads of 0 B to 64 KiB, eight memory layouts both arms share. Both loops give HEAD's bits.

| type | matrix | HEAD ms | pre-row loop ms | pre-row/HEAD over the 8 layouts |
|---|---|---|---|---|
| f64 | uniform-4, 15,420 rows | 0.0274 | 0.0229 | 0.837 [0.809-0.865] |
| f64 | banded-5, 13,107 rows | 0.0225 | 0.0193 | 0.854 [0.764-0.897] |
| f64 | uniform-16, 4,946 rows | 0.0182 | 0.0198 | 1.095 [1.049-1.120] |
| f64 | uniform-4, 160,000 rows | 0.3855 | 0.3490 | 0.903 [0.888-0.911] |
| f32 | uniform-4, 23,831 rows | 0.0302 | 0.0294 | 0.979 [0.968-1.059] |
| f32 | banded-5, 20,000 rows | 0.0251 | 0.0239 | 0.959 [0.938-1.094] |
| f32 | uniform-16, 4,946 rows | 0.0112 | 0.0141 | 1.248 [1.218-1.452] |
| f32 | uniform-4, 160,000 rows | 0.2774 | 0.2651 | 0.957 [0.939-0.978] |

f64's narrow rows are a real regression of 10% to 16% at every layout; f32's narrow rows 2% to 4% (the probe's 0.78
at uniform-4 half L2 was mostly layout); wide rows are faster. The earlier A/B (`sell_ab`, 2f2aed72's loop in its
variants) puts HEAD's own pick for these rows (scalar loads, one slice at a time, no prefetch) in a flat loop at
0.0204 to 0.0210 ms against production's 0.0255 to 0.0261: the cost is HEAD's slice driver, not its pick.

## Against Eigen (the bench, one process)

Eigen ms over production ms per pass, above 1 Cerid faster; geometric mean over the 20 cells of each type
[lowest cell]:

| level | f64 | f32 | c64 | c32 |
|---|---|---|---|---|
| avx2 (detected) | 1.162 [0.897] | 1.392 [1.050] | 1.391 [1.174] | 1.569 [1.249] |
| sse4.2 | 1.101 [0.798] | 1.214 [0.988] | 1.134 [1.030] | 1.399 [1.089] |
| sse2 | 1.098 [0.808] | 1.219 [1.022] | 1.124 [1.030] | 1.396 [1.114] |
| portable | 1.001 [0.716] | 0.946 [0.688] | 1.053 [0.947] | 1.068 [0.956] |

Eigen dispatches on its own CPUID and stays at AVX2 under a forced Cerid level (ADR-0134 §8). At avx2 only f64
uniform-4 trails, at half L2 (0.963 [0.931-0.973]) and between the L2 and the L3 (0.897 [0.858-0.920]), the cells of
the driver regression; sse2 and sse4.2 trail only at f64 uniform-4 (half L2 to 1M, 0.80 to 0.999) against an AVX2
Eigen; portable is level overall against it.

**The arms.** At half the L2 the geometric means of production/arm put production within 4% of the best arm on every
type and level, so the picks stay. Beyond the L2 the column is biased: production runs first after Eigen in each
round and pays for its evicted matrix at half the L3, so every arm sharing its matrix reads 1.2x to 1.3x there,
`policy resident` (production's own pick) included. `index u32` at f64 uniform-4 between the L2 and the L3 (1.217x
over the Auto Delta16 cut) is re-read after the driver fix.

## Jobs

`spmv_sell_parallel` costs 0.33 to 0.35 ms at 4 to 16 jobs on every matrix up to half the L3, on both engines (the
probe: power-law between the L2 and the L3 at 4 jobs, pre-row 0.354 against HEAD 0.358), where Eigen's threads take
0.006 to 0.2 ms; at 32 jobs the same cells take 0.05 to 0.07 ms, and runs land in either mode (the probe's two jobs
cells it calls regressed, c32 power-law at 16 and 32 jobs, read 0.05 to 0.12 ms on pre-row and 0.35 on HEAD). From
1M rows Cerid's best cut is 0.87x to 1.20x Eigen at 4 to 32 jobs. That floor is crd-jobs' wake and join, not SELL;
the retune's thread sweep takes it first.
