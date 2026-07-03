# 2026-07-03 — v14-f einsum execution (TTGT over the own GEMM) vs numpy/torch

- **Machine/config:** i9-14900K, WSL2, `taskset -c 4` (1T, matched), f64, median/best-of-10.
  Cerid: prebuilt `EinsumPlan` + `einsum_execute` (g++ 13.3 -O3 -march=native). Peers: numpy 2.4.6
  `einsum(optimize=True)`, torch 2.12 `einsum` (1 thread). TBLIS/TCL: not installed — N/A stated with
  the check (an oracle build lands with the v14-g close).
- **Harness:** `scripts/run_einsum_bench.sh`. Correctness: 1,139 asserts — exact vs a naive
  full-index-space evaluator across matmul/chains/batch/outer/trace-ring/diagonals/ellipsis/private-
  index pre-summing (both plan modes) + run-twice + the {1,2,4,8,16} moat through `gemm_parallel`
  (ADR-0063); green debug+asan+tidy+gcc.

## The board (µs, lower is better)

| Case | Cerid | numpy | torch | Verdict |
|---|---|---|---|---|
| `ij,jk->ik` @32 (plan reuse ×200) | **2.13** | 7.49 | 6.90 | **3.5× / 3.2× CRUSH** — the build-once story |
| `abc,bad->dc` @96 | 3721 | 3072 | 4470 | 0.83× numpy · **1.20× torch** |
| `ab,bc,cd->ad` @512 (GEMM-bound) | 8393 | 7799 | 6853 | 0.93× / 0.82× — **the raw f64 GEMM gap (OPEN, owner: v0d)** |
| `ea,fb,abcd,gc,hd->efgh` @24 (thin-K TN) | 2905 | 2302 | 1583 | 0.79× / 0.54× — **thin-shape direct kernels (OPEN, the contract's named item)** |

## Levers measured this pass

- **TTGT copy avoidance** (skip materialize when the operand is already `[B,M,K]`/`[B,K,N]`;
  accept the transpose layout via the GEMM trans flags): every row improved (chain 8763→8393,
  TN 3119→2905, small 2.38→2.13).
- Zero-copy stride-sum diagonal views; private-index pre-sum through the Tier-D reducer.
- Found + fixed en route: a store-aliasing bug (permute into the tensor holding its own source —
  resize frees the src buffer); fixed with per-slot double-buffering, caught by the exactness gates.

## Open rows (SANITY #9, named owners)

1. **GEMM-bound parity gap** (0.82–0.93×): pure f64 GEMM throughput (ours ~64 GF/s vs MKL ~78 @512,
   1T) — an engine-wide v0d kernel row, not einsum machinery; tracked for a dedicated GEMM crush pass.
2. **Thin-K contractions** (TN networks): pack overhead dominates K≈24 GEMMs — the v14-f contract's
   "direct kernels for small/odd shapes" item, next increment.

## Increment 2 (2026-07-03, same day): the direct-kernel crush pass

New levers (each measured; one refuted):
1. **Register-blocked small-M direct kernel** (`direct_gemm_smallm`, 8m×2n-vec tile, ATrans variant):
   the tensor-network step shape (M,K≈24, N≈13.8K) ran at ~21 GF/s L1-bound through the packed GEMM;
   the register tile holds 16 accumulators across the full k loop → the TN row went **3021→1496 µs**.
   (A 5m×3n tile variant was measured WORSE — 2095 µs — and reverted.)
2. **Thin-K direct kernel** (`direct_gemm_thin`) for the K,N-small family.
3. **Consumer-aware layouts**: the larger operand keeps any natural (batch|M|K)-run partition (any
   within-run order, trans accepted); the smaller operand matches its sub-orders; materialized free
   groups order ids consumer-kept-first. Kernel-shape exec gates added (5,957 asserts total).

## THE FINAL TABLE (1T matched, best-of-10)

| Case | Cerid | numpy | torch | Verdict |
|---|---|---|---|---|
| `ij,jk->ik` @32 (plan reuse) | **1.86 µs** | 7.32 | 6.73 | **3.9× / 3.6× CRUSH** |
| `ea,fb,abcd,gc,hd->efgh` @24 (TN) | **1496 µs** | 2291 | 1596 | **1.53× / 1.07× WIN** |
| `abc,bad->dc` @96 | 3302 µs | 2931 | 4026 | 0.89× numpy · **1.22× torch** |
| `ab,bc,cd->ad` @512 | 8154 µs | 7792 | 6725 | 0.96× / 0.82× |

## The one remaining cause, pinned precisely (SANITY #9)

The two sub-par cells share a single root: **raw large-K f64 GEMM rate — v0d ~65 GF/s vs
OpenBLAS ~72 / MKL ~78 at 512-class shapes, 1T.** The einsum layer adds nothing to those rows
(copies already eliminated; the `abc,bad` single 7.1 MB re-layout is paid identically by numpy).
The v0d block constants (Mc/Kc/Nc, shared f32/f64) are **bit-locked by ADR-0063** — the fixed
summation order every module's recorded references depend on — so retuning them is an engine-wide
bit-break, not a patch. Sanctioned paths, tracked as the v0d GEMM row: (a) order-preserving
micro-optimization (packing/prefetch/loop overhead at fixed block sizes), (b) an ADR'd opt-in
fast-order kernel tier, or (c) a planned bit-break window with reference regeneration.
