# 2026-06-10 — hesap v7-n-1 ⭐ CLOSE: SQP + augmented Lagrangian (same session, part 7)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slice:** v7-n-1 — the first two of the NLP trio. The third (IPOPT-class filter IPM) is the explicitly-scoped
**v7-n-2** next sub-slice (the v7-d-3/v7-e-2 decomposition pattern; the session had run very long and the
filter IPM is the riskiest remaining piece — quality over marathon, per the pinned v6 lesson). Continues the
v7-f..k logs.

---

## `nlp_sqp.hpp` — inequality-capable line-search SQP (N&W Alg 18.3)

- The QP subproblem min ½pᵀBp + ∇fᵀp s.t. J_E p = −c_E, J_I p ≥ −c_I is solved by the **v7-k
  Goldfarb-Idnani** dual active-set — the designed consumer edge: finite termination, exact duals (which
  become the new multiplier estimates), no feasible start needed, and B ≻ 0 guaranteed by…
- **Damped BFGS** (N&W Procedure 18.2), the device the v7-j prover deliberately lacked: y_L = ∇L(x⁺) − ∇L(x)
  at the new multipliers, θ-damped to keep B ≻ 0. **HS6 — the stress gate deferred from v7-j (λ* = 0 ⇒
  curvature-free exact W ⇒ measured ~×0.995/iter ℓ1 creep) — now converges in < 60 iterations** from the
  classic (−1.2, 1) start. The deferral is closed.
- ℓ1 merit with the ν ≥ ‖duals‖∞ rule + the **second-order correction** generalized to inequalities (the
  restoration rows = equalities + the inequalities violated at the trial point).
- Stops on the 4-part KKT certificate; multipliers/kkt_residual reported in the v7-j semantics.

## `nlp_auglag.hpp` — PHR augmented Lagrangian (LANCELOT-class)

`AuglagObjective` adapts L_A(x; λ, μ, ρ) (Powell-Hestenes-Rockafellar inequalities) as an `Objective<T>`, so
each outer iteration is one **matrix-free v7-d L-BFGS** inner solve — no QP, no factorization, the robust
large-n member of the trio. First-order multiplier updates (λ ← λ − ρc_E, μ ← [μ − ρc_I]₊) under the classical
Bertsekas/LANCELOT (η, ω) schedule; penalty ×10 on stalled violation; the same KKT stopping.

## Tests — 60 asserts / 7 cases (both methods over one battery)

HS6 (BOTH methods — the λ*=0 regime is now covered twice) · **HS14** (equality + active nonlinear inequality;
the published f* = 9 − 2.875√7 matched to 1e-9 by SQP, 1e-6 by auglag) · **Rosenbrock in the unit disk** (the
scipy reference instance: x* ≈ (0.78641515, 0.61769832), boundary-active with μ > 0) · the circle projection
(analytic x*, λ* — now reproduced by a THIRD and FOURTH algorithm after v7-j Newton-SQP) · bit-identical
run-twice + worker counts · m = 0 (SQP ≡ BFGS) + n = 0. Verified debug + asan + shipping + tidy + guards.

## Honest scope

- No elastic mode in the SQP: an infeasible QP linearization reports `LineSearchFailed` (IPOPT/SNOPT
  elasticity is named future work; none of the gates need it).
- **v7-n-2 (next):** the Wächter-Biegler filter interior point + the IPOPT/cyipopt WSL install probe for the
  v7-z scoreboard (it can reuse our MUMPS as its linear solver).

## Next

v7-n-2 (filter IPM + IPOPT probe), then v7-l LP / v7-m conic / v7-o modeling, then v7-z (CLI + the
gold-standard scoreboard + the cluster close).
