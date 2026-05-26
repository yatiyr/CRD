# 2026-05-26 — hesap v4g: IC(0) + ILU(0) + ILUT incomplete-factorization preconditioners

> **Correction 2026-05-26 (v4i-1 close):** the crush numbers below used the Krylov recurrence
> residual vs Eigen's true residual ([[feedback_iterative_bench_matched_true_residual]]). At
> MATCHED true residual the honest **IC(0)-PCG figure is 2.07–2.39× wall / 1.33–1.79×
> per-iteration** (not 2.74–3.10×) on bcsstk13/24/25; and small-nonsym ILUT (sherman3) is
> triangular-solve-bound — Eigen wins wall 0.45× at matched accuracy (Cerid reaches a tighter
> residual). The IC(0) SPD crush and the parallel-setup story hold. context.md + the phase-doc
> row carry the corrected numbers.

Phase 3.1.6 `crd-hesap` v4 (iterative solvers), slice **v4g**. Three incomplete-
factorization preconditioners in `crd-hesap-preconditioners`, all `LinearOp<T>`,
real + complex, determinism-gated.

## What shipped

- **`Ic0Preconditioner`** — Incomplete Cholesky level 0 (SPD/HPD). Left-looking IC(0) on
  A's lower-triangular pattern, with two robustness layers that proved essential on real
  stiffness matrices: (1) **diagonal scaling** D⁻¹ᐟ²·A·D⁻¹ᐟ² to unit diagonal (stiffness
  matrices have ~1e9 diagonal dynamic range; an unscaled shift over-shifts the small
  eigenvalues into a useless M≈huge·I), (2) **Manteuffel diagonal shift** A+α·I with α
  doubling on a non-positive pivot (the level-0 truncation can break SPD-ness). Apply
  z = D⁻¹ᐟ²·L⁻ᴴL⁻¹·D⁻¹ᐟ²·r over L + conj-transpose Lᴴ.
- **`Ilu0Preconditioner`** — Incomplete LU level 0 (general A). IKJ Gaussian elimination on
  A's pattern ∪ inserted diagonals (gemat11 has structurally-absent diagonals), with a
  **pivot floor** (√ε·max|A|). Combined L\U CSR + diagonal-position array; adjoint factors Aᴴ.
- **`IlutPreconditioner`** — ILUT(lfil, droptol), dual-threshold (Saad 1994). Transcribed
  verbatim from SPARSKIT `ilut.f`: dense working row + jw/jr index maps, IKJ with min-column
  elimination, **dual dropping** (multiplier `|fac| ≤ droptol·tnorm` + keep-`lfil`-largest in
  L and U separately via qsplit), dynamic-CSR L+U (fill exceeds A's pattern). **Row-scaled**
  (D_r·A unit ∞-norm) so droptol is scale-invariant + pivot floor on the U diagonal.
- **24 CLI**: `hesap.precond.{ic0,ilu0,ilut}.{f32,f64,c32,c64}` standalone apply + ic0/ilu0/
  ilut added to all 9 solver precond-selectors (pcg/fgmres/bicgstab/minres/symmlq/qmr/gcr/
  gcrot/idrs).
- **+17 tests**: recovery (IC(0)/ILU(0)/ILUT exact on tridiag → ≤2 iters), 2D-Laplacian +
  conv-diff convergence + iteration cuts, ILUT-stronger-than-ILU(0), **ILUT-full-LU-exact**
  (droptol=0, lfil=n), complex, determinism (serial≡parallel spmv bit-exact). Iterative suite
  105 cases / 17328 assertions.

## Honest results (gated bench, real SuiteSparse)

- **IC(0)-PCG CRUSHES Eigen IncompleteCholesky-CG 2.74–3.10×** (bcsstk13/24/25) — fewer
  iterations (443 vs 817, 2002 vs 4038, 4260 vs 6620) AND faster per-iter via the parallel
  SELL spmv. The diagonal-scaling + shift was the enabler (unscaled IC(0) diverged/asserted).
- **ILU(0)**: correct, robust, complete (the cheap level-0 factor).
- **ILUT**: correct (full-LU exact) and matches Eigen IncompleteLUT preconditioner *quality*
  at equal fill (sherman3 17 vs 11 iters). Its **wall-time** on the small available nonsym
  matrices (n~5000) loses (4.5×) because the cost is **triangular-solve-bound** — the
  sequential dense-factor apply dominates and the parallel SELL spmv can't pay at small n.
  The ILUT wall-time crush is gated by a parallel/level-scheduled triangular solve (the
  shared-infra slice the advisor flagged as separate at v4g's start).
- gemat11 (zero-diagonal, pathological) fails for both Cerid and Eigen.

## The debugging arc (the hard part)

The slice grew from IC(0)+ILU(0) to +ILUT, each surfacing a real-matrix robustness gap the
bench exposed (the user authorized each fix; no debt):
1. **IC(0) non-positive pivot on bcsstk13** → diagonal SCALING (the shift alone over-shifts
   stiffness matrices). Diagonal-scale-FIRST-then-small-shift is the standard robust IChol.
2. **ILU(0) missing diagonal on gemat11** → insert explicit diagonals; **zero pivot** → pivot floor.
3. **ILUT "more fill → worse" on sherman3** — looked like a bug (impossible for correct ILUT).
   Localized via the full-LU-exact test (proves the no-drop path is correct) + the
   reordering experiment (AMD didn't help, ruling out ordering) + a droptol sweep. Root
   cause: `droptol=1e-4` over-dropped on sherman3's ill-scaled rows (one large entry inflates
   the average-row-norm `tnorm`). Fix: ROW SCALING (D_r·A unit ∞-norm → droptol scale-
   invariant) → sherman3 295→17 iters. Also fixed an in-place vs front U-compaction (front-
   compaction corrupts U when a row's working U length exceeds its index).

Lesson saved to memory: `feedback_incomplete_factorization_robustness` (IC needs scaling,
ILU needs pivot floor, ILUT needs row scaling — incomplete factorizations are not robust
without the scaling/flooring layer that production libraries always include).

## Next

v4g-tri-solve-parallel — level-scheduled triangular solve (shared `crd-hesap-sparse` infra,
bit-exact): unlocks the ILUT/IC wall-time crush on triangular-solve-bound regimes + speeds
SSOR/block-Jacobi applies. Then v4h (ILU(p) level-of-fill).
