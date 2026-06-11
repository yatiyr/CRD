# 2026-06-10 — hesap v7-h CLOSE: trust-region (same session as v7-f/g, part 3)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slice:** v7-h — the trust-region framework + the full subproblem ladder. Continues
`2026-06-10-v7f-first-order.md` + `2026-06-10-v7g-newton.md`.

---

## What was built — `trust_region.hpp` (umbrella)

**Framework** (N&W Algorithm 4.1): ρ-test acceptance (η = 0.15), radius shrink ¼ / grow 2× on boundary steps,
**reject-keeps-H** (a rejected step re-solves the same model at smaller Δ without re-evaluating the Hessian),
and a Δ-collapse stall guard (radius below rounding scale ⇒ `SmallStep`, the honest stall report).

**Six subproblem solvers** for min gᵀp + ½pᵀHp s.t. ‖p‖ ≤ Δ:

| Solver | Needs | Notes / gold peer |
|---|---|---|
| Cauchy | H·v | model minimizer along −g (the convergence-theory floor) |
| Dogleg | dense H | pU→pB path; Cauchy fallback on a failed (non-PD) factor [scipy 'dogleg'] |
| Subspace2D | dense H | span{g, (H+τI)⁻¹g} (τ by the v7-g N&W-3.4 ladder), orthonormalized → the exact 2×2 |
| Steihaug-CG | H·v | N&W Alg 7.2: boundary + negative-curvature exits, Eisenstat-Walker forcing [scipy 'trust-ncg'] |
| TrustKrylov (GLTR) | H·v | Lanczos w/ FULL reorthogonalization; the k×k tridiagonal subproblem solved EXACTLY per step; GLTR residual β_k·\|y_k\| [scipy 'trust-krylov' = Gould's GLTR] |
| Exact (Moré-Sorensen) | dense H | via the v3 `dense::eig_sym`: interior test, safeguarded secular Newton on 1/Δ − 1/‖p(λ)‖, HARD CASE closed-form in the eigenbasis [scipy 'trust-exact', GALAHAD] |

⭐ **`solve_trust_region_subproblem_exact` is PUBLIC** — a certified TR subproblem solve returning the
Moré-Sorensen KKT triple (λ, pred, boundary). Reused internally by Subspace2D (2×2) and GLTR (k×k tridiagonal);
named consumers ahead: v7-k QP, eylem.

## Tests — 109 asserts / 6 cases (file-captured); full hesap-opt suite 576 / 55

1. **The KKT certificate verified DIRECTLY** on the exact solver: interior (λ=0, Hp=−g) · boundary (λ>0,
   ‖p‖=Δ) · indefinite regular (λ ≥ −λ₁) · **hard case** (g ⊥ the λ₁-eigenspace: λ = −λ₁ exactly + the τ pad
   along q₁ checked against the closed form) · pure saddle (g=0: p = ±Δ·q₁, pred = ½|λ₁|Δ²) — all on diagonal
   H with known spectra, so every condition is checkable to 1e-9.
2. All SIX subproblems drive the framework to the minimizer of an SPD quadratic (incl. Cauchy at ~κ·ln
   iterations; the Newton-class five in < 60).
3. Rosenbrock-2 with the five Newton-class subproblems (TR globalization; n=2 per the v7-g local-min lesson).
4. Saddle escape from the indefinite double-well start: Steihaug (negative-curvature exit), GLTR, exact.
5. **Determinism moat {1,2,4,8,16}:** Steihaug over `ParallelSparseLinearOp` (gradient + H·v parallel-but-
   bit-exact) — trajectory bit-identical.
6. n = 0 boundary.

## Verification (module-local; CI owns the sweep)

win-debug suite 576/55 + 5 source guards · win-asan exit 0 · win-shipping exit 0 · win-tidy clean (it caught
mismatched `bugprone-argument-comment` names in the test fixtures — fixed).

## The f64-resolution-floor lesson re-applied

The all-six quadratic gate initially failed for **Cauchy only** with `SmallStep`: a slow first-order method
crawls THROUGH the stall window (model reduction < eps·|f*| ≈ 3e-15 at |f*| ≈ 10) where Newton-class methods
leap over it. Fix: the test quadratic is now **recentered** — f = ½(x−c)ᵀA(x−c), f* = 0 exactly — so the
resolution floor scales away near the minimum and even Cauchy certifies grad_tol = 1e-8. (Driver behavior was
correct: ρ saturates at the f-resolution and Δ collapses ⇒ the honest `SmallStep` report.)

## Honest scope notes

- GLTR here is the faithful algorithm (per-step exact tridiagonal solve + full reorthogonalization) without
  the production engineering of Gould's GALAHAD implementation (no preconditioning, no restarts; full-memory
  Lanczos with k ≤ 64 default). Named, not hidden.
- Eval-parity benches vs scipy `trust-*` live at v7-z with the rest of the gold-standard scoreboard.

## Next

v7-i stochastic/ML (note the v12 counter-RNG dependency split pinned in the plan), or the constrained spine
v7-j → v7-k QP (OSQP-class; the exact TR subproblem solver is already in place) → v7-n NLP.
