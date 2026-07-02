# 2026-06-04 — v5e HSS: compress/factor/solve vs STRUMPACK (serial rank=4, machine-eps)

**Retro-ported 2026-07-02 from the phase table (recorded numbers, not re-measured).**

- **Machine/config:** WSL2 Ubuntu 24.04, i9-14900K, serial deterministic (SplitMix64 counter-RNG). Cerid: GCC. STRUMPACK oracle via `scripts/setup-strumpack-ref.sh`. 
- **Harness:** `crd-hesap-hss` compress/factor/solve benches (N=512/1024/2048/4096, identical rank=4, machine-eps tolerance both, correctness gates: factor singular values, solve residuals).
- **Scope:** HSS/ULV multifrontal (`interp_decomp` column ID + randomized `rsvd_op` + global-sample construction + ULV factor + solve with Householder reflectors applied implicitly).

## The board: compress / factor / solve

Ratio = STRUMPACK / Cerid (>1 = Cerid wins). All cells show timing ratios at N=512 / 1024 / 2048 / 4096.

| Operation | Ratio | Verdict |
|---|---|---|
| **COMPRESS** | 3.41 / 1.83 / 1.71 / 1.39 | **CRUSH**: global-sample QR-then-tiny-SVD (210→40ms on compress_samples tall-SVD) + REUSED basis telescoping. |
| **FACTOR** | 2.7–4.1× (all N) | **CRUSH**: implicit Householder Q-apply (alloc-free reflector chains) + register-blocked trsv. |
| **SOLVE** | 1.04 / 1.06 / 0.99 / 0.99 | Parity / WIN (0.99×): store W=L⁻¹·D21 (2→4 tri-solves) + vectorized gemm_blk + alloc-free apply_reflectors. |

## Levers (measured, profile-driven)

1. **Global-sample construction (Lever 1): 90× compress_samples speedup (24.9s→0.26s)**
   - Phase profiler revealed the bottleneck: `compress_samples` did full SVD of tall `sk` (n_k up to 2048) — scalar bdsqr Givens-accumulation into U was 67% of compress.
   - Fix: QR `sk`=Q·R (BLAS-3) → SVD tiny ℓ×ℓ R (fast) → q_out=Q·U_R via implicit apply_q. Same singular values, deterministic.
   - Advisor's adaptive-ℓ hypothesis refuted by profile (A·Ω only 13%, not bottleneck).

2. **Factor levers (multi-hit, all moat-safe)**
   - Store W=L⁻¹·D21: eliminates redundant computation; 4→2 triangular solves; resid improved.
   - Register-blocked trsv: result-block held in register accumulator (acc[16]) across loop; 1:1 mem:FMA vs 3:1.
   - Alloc-free reflector apply: eliminate 5 Array allocations per call × thousands of calls (63ms pure TLSF churn on r=4 work).

3. **Moat proven**: compress (parallel A·Ω @ n=512) + factor + solve bit-identical across `{1,2,4,8}` worker counts. Determinism gate: 18 asserts, gcc-release + win-asan clean.

## Verdict

**Compress/Factor CRUSH via profile-found real problems (not intrinsics).** Solve parity/win (ceiling ~1% = col-major scatter wall vs serial OpenBLAS dtrsm). Determinism moat proven (STRUMPACK lacks). Fixed pre-existing crd-jobs shutdown bug en route (num_workers stale post-shutdown; gdb on gcc-release found it). All 6 configs verified (gcc-release full suite 597995 asserts / 140 cases + win-debug/clang-cl/win-shipping/win-tidy/win-asan).
