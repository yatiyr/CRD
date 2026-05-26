# 2026-05-26 — hesap v4h: ILU(p) level-of-fill (the ILU family is complete)

> **Correction 2026-05-26 (v4i-1 close):** the crush numbers below were measured on the
> Krylov *recurrence* residual vs Eigen's *true* residual — an unfair stopping mismatch
> ([[feedback_iterative_bench_matched_true_residual]]). At MATCHED true residual the honest
> figure is **ILU(2) cd2d-100 = 2.58×** (not 3.2×; Cerid 30 it / 5.40 ms vs Eigen 47 it /
> 13.92 ms). The level-of-fill value-add and the sherman3 robustness win (Cerid converges,
> Eigen diverges) both hold. context.md + the phase-doc row carry the corrected number.

Phase 3.1.6 `crd-hesap` v4, slice **v4h**. Level-of-fill incomplete LU, completing the
ILU preconditioner family (IC(0) + ILU(0) + ILU(p) + ILUT).

## What shipped

- **`IlupPreconditioner<T>(a, alloc, p)`** (Saad Alg. 10.5) — fused symbolic+numeric IKJ.
  Each entry carries a fill **level**: A's nonzeros are level 0; a fill (i,j) produced by
  eliminating k has level `lev(i,k) + lev(k,j) + 1` (the **minimum** over all fill paths).
  A **new** fill is created only when its level ≤ p; an **existing** pattern entry is
  **always** updated numerically (the level gates fill *creation*, never *updates*). U
  entries store their level (they are fill sources for later rows). Pivot floor; no
  row-scaling / threshold / qsplit (purely structural). Reuses the v4g level-scheduled
  parallel triangular solve + the dynamic-CSR L/U + the adjoint(Aᴴ) path.
- **4 CLI** (`hesap.precond.ilup.{f32,f64,c32,c64}` standalone apply + `ilup`/`level` added
  to all 9 solver precond-selectors).
- **+4 tests**: recovery (exact at any p on tridiag; a DENSE matrix ⇒ ILU(0) = full LU,
  exact); **monotone fill + convergence in p** (factor_nnz grows, iterations drop — a wrong
  max-instead-of-min level would break this); complex; determinism. Iterative suite
  **110 cases / 108024 assertions**.

## The bug (caught by the dense-matrix test)

First implementation put `if (newlev > p) continue;` *before* the numeric update — so at
p=0 (every newlev ≥ 1 > 0) it skipped **every** fill update, producing a factor that wasn't
even ILU(0). The dense-matrix test (ILU(0) on a dense matrix must equal full LU → 1 iter)
exposed it (6 iters). Fix: the level threshold gates only **new fill creation** (`jpos==-1
&& newlev>p`); existing pattern entries are always updated and their level refined to the
min. Recovery then exact.

## Honest crush result (gated bench, vs Eigen IncompleteLUT, matched fill, FGMRES)

- **cd2d-100 (n=10000)**: ILU(0) 48 it / 8.10 ms (fill 1.0×) → ILU(1) 29 it / 4.78 ms
  (1.40×) → **ILU(2) 25 it / 4.49 ms (1.79×)** — monotone iteration cuts with controlled
  fill (the level-of-fill value-add). **Eigen IncompleteLUT 47 it / 14.33 ms at the same
  1.79× fill ⇒ Cerid ILU(2) is 3.2× faster.**
- **sherman3**: Cerid ILU(2) converges (98 it, r=6.6e-7) where Eigen ILUT **diverges**
  (r=1.2) at matched fill — robustness win.
- Eigen ships no level-of-fill ILU(p) → breadth + the crush.

## Status

🎉 The ILU preconditioner family is complete: **IC(0)** (crushes Eigen IncompleteCholesky
2.74–3.10×), **ILU(0)** (level-0), **ILU(p)** (level-of-fill; ILU(2) crushes Eigen
IncompleteLUT 3.2×), **ILUT** (dual-threshold; matches/beats Eigen ILUT). All share the
level-scheduled parallel triangular solve + the determinism moat.

win-debug ctest + guards + engine win-tidy clean.

## Next

The v4 plan continues (AMG and beyond per the phase-doc v4 ledger).
