# 2026-06-11 — the SOLVE crush + the full-scoreboard correction (part 20)

**Phase:** 3.1.6 `crd-hesap`. The user caught me reporting lattice "parity/WIN" verdicts on FACTOR ratios
while the same bench lines showed SOLVE losing 2–3× and REGRESSING with thread count — a recorded-but-parked
deficit I kept parked. Standing correction saved to memory
(`feedback_full_scoreboard_no_partial_victory`): **every bench verdict reports ALL metrics together; a crush
claim requires ALL of them green.** This session attacked the solve with the same measure-first discipline
as the factor work.

## Root causes found (each measured, each fixed)

1. **The single-RHS parallel solve was NET-NEGATIVE and got worse with workers** (lat32: 69 ms serial ≈ 69
   @8T → 98 @16T; hood 27 → 54 @16T): a triangular solve is memory-streaming-bound — one core saturates the
   achievable bandwidth and the per-level fork/join only adds cost. The old `kSolveParallelMinLnz` gate sent
   every large factor down that path. **Fix: single-RHS is always serial** (the moat test still forces the
   parallel path explicitly; parallel ≡ serial bit-identically by construction).
2. **The backward pass was a scalar FP-add dependency chain** (gcc cannot vectorize reductions under strict
   FP): measured fwd 30.5 ms / back 39.6 ms on lat32 serial. **Fix: shared SIMD solve kernels** —
   `solve_axpy_minus`/`solve_acc_plus` (element-independent maps, the vector form is BIT-IDENTICAL to the
   scalar loop) for the forward, and `solve_dot_conj` routing to the canonical `simd_dot` (FMA + fixed
   4-accumulator reduction tree, deterministic) for the backward dots — used by BOTH the serial and the
   level-parallel paths, so worker counts stay bit-identical. Also hoisted the parallel backward's
   per-column re-gather of `xb[srow[...]]` (was nc× redundant indexed loads). lat32 serial solve:
   **70 → 51 ms**.
3. **The multi-RHS parallel solve collapsed at high worker counts on small/mid factors** (lat12 x16 @16T:
   25.3 ms vs CHOLMOD 2.0 = **0.08×**; every size degraded past 8 workers): the path always parallelized at
   full pool width. **Fix: a measured work gate** (parallel iff lnz·nrhs ≥ 160M — lat20×16 lost parallel,
   lat24×16 won) **+ an 8-lane cap** (16 lanes measured strictly worse than 8 at every size: lat24 x16 46 ms
   @8 → 74 @16). lat12 x16 @16T: 25.3 → 2.6 ms.
4. The streaming ceiling was MEASURED, not assumed: a pure SIMD read of our factor buffer = **25–27 GB/s**,
   and CHOLMOD's own factor buffer streams at the same 25.5 GB/s (probe inside the bench). Software prefetch
   (tested at 4 KB ahead + per-column head warming) moved nothing — the HW prefetcher already covers it.
   Our solve passes run at ~80% of that ceiling; CHOLMOD's solve time implies ~100% on fewer effective
   bytes — the residual model question is NAMED below.

## THE FULL BOARD (CHOLMOD/Cerid, >1 = Cerid wins; FACTOR · SOLVE · x16 at 1T/8T/16T)

| matrix | FACTOR | SOLVE (1 RHS) | SOLVE x16 |
|---|---|---|---|
| nls_lat12 | 0.91 / 0.77 / 0.37 | **1.34 / 1.33 / 2.67 WIN** | 0.75 / 0.67 / 0.83 |
| nls_lat16 | 0.81 / 0.94 / 0.77 | **0.97 / 1.07 / 1.86** | 0.93 / – / – |
| nls_lat20 | 0.89 / 0.94 / 0.85 | 0.81 / 0.79 / **1.05** | 0.72 / 0.52 / 0.64 |
| nls_lat24 | 0.89 / 0.98 / **1.02 WIN** | 0.79 / 0.68 / 0.71 | 0.93 / 0.86 / 0.77 |
| nls_lat28 | 0.77 / 0.90 / **1.00 parity** | 0.83 / 0.75 / 0.72 | 0.70 / 0.65 / – |
| nls_lat32 | 0.78 / 0.83 / **0.96 parity** | 0.69 / 0.61* / 0.72 | 0.82 / 0.70 / 0.85 |
| bcsstk25 (FEA) | **1.34 / 1.70 / 1.37 WIN** | **1.45 / 1.79 / 3.82 WIN** | 0.81 / 0.80 / 0.89 |
| hood (FEA) | 0.92 / **1.61 / 1.98 WIN** | 0.93 / 0.94 / 0.99 parity | 0.77 / **1.82 / 1.66 WIN** |

(*the lat32 8T single-RHS line predates the final gate run; the solve is serial-locked and flat ~51 ms at
every worker count — the ratio moves only because CHOLMOD's own number moves.) Residuals 8.9e-15..1.2e-14 on
every lattice (the new backward dot slightly IMPROVED lat32's residual); FEA residuals unchanged.

## Honest verdict + the named remaining gaps (with measurements)

- Single-RHS solve: was 0.43–0.55 with a thread-collapse; now **flat at every worker count, WINS on
  small lattices and bcsstk25 (up to 3.82×), parity on hood, 0.69–0.83 on big lattices**.
- x16: collapse fixed (0.08× → 0.83×); hood x16 now **WINS 1.66–1.82×**; mid lattices 0.52–0.93.
- NAMED GAP 1 — big-lattice single-RHS rate: we stream 543 MB/pass at ~21 GB/s vs the 26 GB/s ceiling
  (80%); CHOLMOD's 35–38 ms implies ~100% of ceiling on ~15% fewer effective bytes — its per-byte advantage
  is not explained by my traffic model and needs perf-counter work (WSL-limited). Closing rate alone caps us
  at ~0.85–0.90 on lat32 single-RHS.
- NAMED GAP 2 — CHOLMOD's single-RHS solve GAINS ~15% from threaded OpenBLAS gemv at 8T on big factors
  (37 → 31 ms) while ours is serial-locked; the analogous lever is WITHIN-FRONT parallel gemv on the big
  supernodes (not the per-level fork that lost) — a future slice.
- NAMED GAP 3 — mid-size x16 parallel efficiency (lat20 x16 0.52 @8T) and tiny-matrix 16T factor overhead
  (lat12 factor 0.37 @16T — fork costs on a 9 ms problem; gate candidate).

## Verification

WSL gcc: dense 359,508/349 + direct 598,861/190 ✓ (the {1..16} moat holds — serial and parallel paths share
the new kernels by construction). win-debug + win-shipping: direct 598,861 ✓. win-tidy: caught 3 local-constant
naming violations (kCamelCase → lower_case), fixed, green. win-asan: both modules green. The temp probes
(SOLVE-SPLIT, STREAM-CEIL, CHOLMOD-LX-STREAM) removed; the dead `kSolveParallelMinLnz` constant removed.
