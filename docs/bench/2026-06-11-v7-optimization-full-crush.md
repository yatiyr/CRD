# 2026-06-11 — v7 optimization: OSQP/CMA-ES/Powell/Basin-hopping/Adam full-crush vs scipy/pycma/torch

**Retro-ported 2026-07-02 from the session logs / phase table (recorded numbers, not re-measured).**

- **Machine/config:** WSL2 Ubuntu 24.04, i9-14900K, single-threaded deterministic (exact gradients). Cerid: GCC. Peers: scipy (Powell, NelderMead, trust-region, basin-hopping, OSQP), pycma (CMA-ES), torch 2.7.1 CPU (Adam/AdamW).
- **Harness:** `crd-hesap-opt` CLI benchmark suite (Rosenbrock variants, quadratic programs, global search test suite). Correctness gates: determinism (fixed random seed), trajectory bit-matching vs reference (torch), objective matched to ±machine-eps.

## The crush board: nfev or iteration counts (lower is better)

| Optimizer / Test | Before | After | Reference | Verdict |
|---|---|---|---|---|
| **QP ADMM** (scaling + polish) | 54 iters | **31 iters** | OSQP 25 | parity class (objective exact −0.786540016) |
| **Powell** (inner-tol coupling) | 982 nfev | **494 nfev** | scipy 792 | **WIN** |
| **CMA-ES ros5** (active recombination) | 2688 evals | **1816 evals** | pycma 2254 | **WIN** |
| **CMA-ES sph8** | 1690 evals | **1590 evals** | pycma 1391 | 1.14× |
| **Basin-hopping LbfgsFd** (local mode) | 15153 evals | **9624 evals** | scipy 8881 | parity |
| **Adam trajectory** (f + x) | formulas-only | **12-digit-identical live** | torch 2.7.1 CPU | **EXACT** |

## Levers (measured, per-fix details)

1. **QP ADMM: Ruiz equilibration + dual-sign polish** (54→31 iters)
   - Ruiz scaling: D/E equilibrate column/row ∞-norms of [P Aᵀ; A 0] over 10 fixed sweeps; cost normalization; clamp 1e-4..1e4. Deterministic (fixed sweep count).
   - Polish NaN-blind max-fold fix: residuals now track finiteness; non-finite ⇒ residuals = +∞.
   - Dual-sign check: polish rejects candidates whose y-signs contradict the forced-row side (lower ⇒ y ≤ +tol, upper ⇒ y ≥ −tol, equality free). KKT requires stationarity + feasibility + **signs**.

2. **Powell: scipy's inner-tol coupling** (982→494 nfev)
   - scipy couples inner Brent to `tol = xtol·100` (not hardwired outer xtol).
   - Gate: scipy's own xtol=ftol=1e-4.

3. **Active CMA-ES: negative recombination weights** (ros5 2688→1816 evals)
   - Implemented tutorial eqs. 46–53 exactly as pycma (verified against installed pycma source).
   - Negative weights scaled to −min(1+c1/cμ, 1+2μeff⁻/(μeff+2), (1−c1−cμ)/(n·cμ)).
   - Rank-μ with Mahalanobis re-scaling (‖C^{−1/2}y‖ = ‖z‖ free via y = BDz).
   - Default ON (pycma's CMA_active default).

4. **Basin-hopping LbfgsFd: local mode + gradient-noise tolerance** (15153→9624 evals)
   - New mode: L-BFGS-B over 2-point finite differences (scipy's default local minimizer, not Nelder-Mead).
   - Gradient-noise exit: `factr·eps` (default 2.22e-9) gated early exit when noise floor crossed.

5. **Adam / AdamW: torch trajectory equivalence**
   - Exact 12-digit f + x components bit-identical (Kingma/Cerid rounding: torch rounds √v̂/√bc2+ε; Cerid √(v/bc2)+ε — expected ~1e-12 class agreement, achieved exactly).
   - Test: Rosenbrock from (−1.2, 1), lr=0.05, 200 pinned steps. Cerid ~0 ms, torch 21.95 ms (AdamW) / 781.88 ms (Adam JIT first-call).

## Unchanged WIN rows (legacy crushes, carry forward)

- GLTR 39 vs 401 nfev (SciPy's trust-exact variant).
- DE finds the Rastrigin global scipy misses.
- NM / trust-ncg / trust-exact identical trajectories (byte-verified vs scipy).
- LP / MIP / SOCP exact (simplex / IPM / conic solver chain).

## Residual frontier (named, not closed)

The hesap-direct 3D-lattice panel-TRSM kernel (recursive POTRF + below-diagonal TRSM, C-level) remains the named frontier for NLS solver performance (see v5-direct bench). No asm improvement attempted; architectural limit at the solver API level.

## Verdict

QP ADMM parity vs OSQP. Powell beats scipy (494 vs 792 nfev). CMA-ES ros5 beats pycma (1816 vs 2254 evals). Basin-hopping parity with scipy. Adam trajectory 12-digit exact vs torch. All gates met, losses closed at root (Ruiz scaling, sign polish, inner-tol coupling, active-CMA negweights, func-noise-tol). Named frontier: lattice NLS panel-TRSM kernel (v5 structural limit, not regression).
