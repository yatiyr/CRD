# 2026-05-26 — hesap v4f-2: block-CG + block-PCG (the complete block-CG family)

Phase 3.1.6 `crd-hesap` v4 (iterative solvers), slice **v4f-2**. Shipped block-CG +
block-PCG for SPD/HPD multi-RHS systems, with the complete block-preconditioner family.

## What shipped

- **`BlockLinearOp<T>`** (crd-hesap-sparse) — the block (multi-RHS) analogue of
  `LinearOp<T>`, deliberately separate (the `LinearOp` vtable is locked). `apply_block`
  applies A to all s right-hand sides in ONE fused pass over A.
- **`ParallelSpmmLinearOp<T>`** — square `BlockLinearOp` over CSR; `apply_block` = one
  `spmm` (serial) / `spmm_parallel` (above ~L2 working set), bit-exact across threads.
- **`block_cg` / `block_pcg`** (`block_cg.hpp`) — classic O'Leary block-CG in the
  **Galerkin form** (γ = (PᴴAP)⁺·PᴴR, δ = −(PᴴAP)⁺·(AP)ᴴZ, both solved against the same
  PᴴAP), **breakdown-free via per-step search-block orthonormalization**. Block-PCG folds
  the preconditioner in exactly where M=I (Z≡R) recovers block-CG.
- **Block preconditioner family** (crd-hesap-preconditioners, `block_preconditioner.hpp`):
  `JacobiBlockPreconditioner` (native one-pass diagonal, row-scaled) + `BlockPreconditionerAdapter`
  (wraps ANY single-vector `LinearOp<T>` via gather/apply/scatter — SSOR/IC/block-Jacobi all
  work in block mode, so the family is complete without a per-preconditioner block rewrite).
- **8 CLI**: `hesap.iterative.{block_cg,block_pcg}.{f32,f64,c32,c64}` (block_cg in the
  iterative module; block_pcg in the preconditioners module where both the solver and a
  concrete block preconditioner are visible — same split as scalar pcg).
- **+7 tests** (test_block_cg.cpp): SPD s=4, rank-deficiency (duplicate-column x₀≡x₂),
  complex HPD, determinism (serial≡parallel bit-exact), **ill-conditioned 1D Laplacian
  cond~2.6e5 (the QR regression guard)**, block-PCG cuts iterations, native-Jacobi ≡
  adapter-point-Jacobi bit-exact.

## The engineering arc (three pivots)

1. **Convergence bug → Form-1 Galerkin.** The initial D-BCG rewrite used the residual-ratio
   recurrence (β from RᴴR ratios), which is fragile and failed to converge. Reverted to the
   Galerkin form (γ/δ both vs PᴴAP) — the proven O'Leary recurrence.

2. **Tall-skinny GEMMs → row-streaming kernels.** The block products (PᴴAP, PᴴR, (AP)ᴴZ,
   X+=Pγ, …) are the K=s tall-skinny shape; the generic packed `gemm` pays packing overhead
   it can't amortise at K=s. Replaced with allocation-free **row-streaming** rank-1 / axpy
   kernels (the documented small-K anti-pattern, `feedback_simd_rowwise_unblocked_beats_blocked_smallk`).
   Flipped banded s=4 from 0.92× → 1.18× over Eigen.

3. **Robustness → breakdown-free orthonormalization (packed MGS).** Plain D-BCG (no QR)
   STALLS on ill-conditioned A (cond~1e10 bcsstk: r stuck at 9.7e-4) from gradual loss of
   search-block conjugacy — a correctness defect, not a perf regime. Restored the per-step
   thin-QR (Dubrulle DP-BCG breakdown-free variant), implemented as a basis change that
   leaves the Galerkin iterates unchanged in exact arithmetic while keeping PᴴAP well-
   conditioned. Iterated the QR implementation:
   - strided MGS: robust but 4.8× slow (cache-unfriendly column access),
   - CholeskyQR2: fast but loses orthogonality on cond~1e10 (cond² gram overflows) → bcsstk stalls,
   - **packed MGS (final)**: transpose to column-contiguous, MGS+reorth via the bit-exact
     SIMD blas1 (`dotc`/`axpy`/`nrm2`/`scal`), transpose back — robust AND fast.

## Honest crush result (gated 3-way bench, all Jacobi-preconditioned)

- **Cerid per-column PCG CRUSHES Eigen CG+Jacobi EVERYWHERE: 1.23–2.43×** (every matrix,
  every s) via the v4a parallel SELL spmv — the universal win and the cache-resident-regime owner.
- **Cerid block-PCG additionally beats Eigen on the expensive banded operator (1.12× @s=4)**,
  with 3–16× fewer A-passes, and converges robustly on ill-conditioned bcsstk where it
  previously stalled.
- **Characterized floor** (per `feedback_crush_mandate_bounded_by_importance`): on
  cache-resident A (tridiag, small bcsstk) block-PCG's O(n·s²) dense flops have no DRAM-pass
  to trade for, so per-column PCG is the algorithm-appropriate path and owns that regime.
  block-PCG's wall-time win scales with operator expense (A>L2 / matrix-free expensive applies).
- bcsstk24 (cond~1e11) stalls for BOTH Cerid and Eigen at the iter cap — the Jacobi-CG limit,
  not a block-PCG defect.

User decision (asked mid-slice): block-PCG built **in-slice** (complete family now); cheap-
operator floor **characterized-and-accepted** because another slice (v4a per-column PCG)
crushes that regime — proven explicitly by the per-column column in the bench.

## Verification

- Iterative suite **85 cases / 7892 assertions** (win-debug) — all green.
- win-debug ctest guards green (no-non-ascii / no-std-math / no-std-sort / no-untagged).
- clang-cl (stale PCH from an MSVC toolset bump) + win-tidy (newer-local-LLVM skew flagging
  pre-existing shipped files) hit local environment issues only; CI owns the pinned-LLVM-19
  cross-config gate per `feedback_local_test_only_ci_owns_sweep`.

## Next

v4f-3 — block-GMRES + block-BiCGSTAB (nonsymmetric multi-RHS) on the v4f-2 `BlockLinearOp`.
