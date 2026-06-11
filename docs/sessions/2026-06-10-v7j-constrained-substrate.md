# 2026-06-10 — hesap v7-j CLOSE: the constrained substrate (same session as v7-f/g/h, part 4)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slice:** v7-j — constraints + Jacobians + the KKT system + Lagrangian/ℓ1-merit + multipliers + KKT-residual
stopping; the foundation v7-k (QP) and v7-n (NLP) consume. Continues the v7-f/g/h logs.

---

## What was built

- **`constraints.hpp`** — `Constraints<T>`: c_E(x) = 0, c_I(x) ≥ 0 with the PINNED sign conventions
  (L = f − λᵀc_E − μᵀc_I; N&W Ch.12) every v7-j..n consumer shares. Dense row-major Jacobians +
  `add_lagrangian_hessian` (the constraint-curvature hook, IPOPT-`eval_h`-style) + the capability contract +
  a locked vtable with reserved sparse-Jacobian slots.
- **`kkt.hpp`** —
  - `KktResidual<T>`: the **4-part certificate** (stationarity ‖∇L‖∞ · primal · dual · complementarity) whose
    `max()` is the constrained stopping quantity (now `OptResult::kkt_residual`; `OptResult::multipliers` added —
    the fields the v7-a comment reserved).
  - `solve_kkt_dense`: the saddle [W J_Eᵀ; J_E 0]·[p; z] = [−g; −c] (λ⁺ = −z) factored by the v0e dense
    **Bunch-Kaufman LDLᵀ** with the **inertia test read directly off D's 1×1/2×2 blocks** — (n+, m−, 0) required
    for an EQP minimizer — and the IPOPT-style correction ladders (δ·I on W for wrong inertia ×100/retry; γ·I on
    the constraint block when singular).
  - `estimate_eq_multipliers`: λ̂ = argmin ‖∇f − J_Eᵀλ‖₂ via the v3 `dense::lstsq`.
- **`merit.hpp`** — ℓ1 exact-penalty value + the one-sided DIRECTIONAL derivative (N&W 18.29 kink rules).
- **`sqp_equality.hpp`** — the substrate PROVER (the v7-a gradient-descent analog): line-search Newton-SQP with
  the exact-penalty ν-rule (ν ≥ ‖λ⁺‖∞ + margin) **plus the second-order correction** (N&W §15.6): on a rejected
  full step, p̂ = Jᵀ(JJᵀ)⁻¹(−c(x+p)) restores feasibility to third order — added after MEASURING the ℓ1 creep.
  Constrained reporting semantics: `grad_norm` = the stationarity part; `kkt_residual` = the certificate max.

## Tests — 68 asserts / 9 cases; full hesap-opt suite **644 / 64**

Analytic KKT-point residual checks (equality λ=1 point; inequality μ=2 point + dual/complementarity violations
isolated) · the KKT-solve certificate including the SHARP inertia pair — **indefinite W with a PD reduced Hessian
must NOT be regularized** (δ=0, by Sylvester via the nullspace) while an indefinite reduced Hessian must fire the
δ-ladder and still certify · multiplier-LS at the circle's analytic KKT point · merit directional derivative vs
finite differences · **one-step convergence on an equality QP** (Newton-KKT exactness, the v7-g one-step analog)
· the circle problem and circle projection (nonlinear constraint curvature through the hook; analytic x*, λ*) ·
the `{1,2,4,8,16}` moat with the **primal AND dual trajectory** bit-identical (quartic objective over the
parallel spmv + a linear constraint) · m=0 (SQP ≡ Newton) + n=0 boundaries.

## Verification

win-debug 644/64 + 5 guards (the non-ASCII guard caught a `é` in a v7-h test name — fixed) · win-asan exit 0 ·
win-shipping exit 0 on a CLEAN standalone-CMake build · win-tidy clean.

## Honest deferrals (named, not hidden)

- **HS6 deferred to v7-n as a globalization stress gate:** with λ* = 0 the exact ∇²L carries no constraint
  curvature and plain ℓ1-Armijo creeps globally (~×0.995/iter — measured, 500 iterations insufficient even with
  SOC). N&W Alg 18.3 itself prescribes **damped BFGS**, not exact ∇²L, for line-search SQP; the production
  globalization (damped BFGS / filter / watchdog) lands with the full v7-n SQP. The v7-j tests use problems in
  the healthy exact-Hessian regime (λ* ≠ 0 or linear constraints) — which is what the substrate must prove.
- Finite-difference constraint Jacobians not built (the analytic capability contract is asserted).

## En-route: the `#deps 0` landmine root-caused DEEPER (the v7-f fix was incomplete)

The win-shipping suite SIGSEGV'd on the new v7-j test — a stale-obj artifact, not a code bug: `OptResult` grew
fields this slice and the incremental shipping build compiled ONLY the new TU (deps dead again ⇒ mixed struct
layouts across objs). The second dig found the REAL mechanism: **the VS-bundled CMake fork (`4.2.3-msvc3`,
first on PATH under vcvars) detects/stores the ENGLISH `showIncludes` prefix on this Turkish-locale host while
generating rules a Turkish-emitting cl can't satisfy** — and any in-build REGENERATE (CMakeLists.txt change)
run via bare `cmake --build` under vcvars re-writes `rules.ninja` from that stored detection, silently
re-breaking a freshly-fixed dir. Durable fix applied: **always invoke the standalone CMake by explicit path**;
purged `CMakeFiles/*-msvc*` stored detections from all dirs; wiped + rebuilt win-shipping (verified Turkish
prefix + `#deps 108` + suite green + incremental header rebuilds now work). CLAUDE.md Troubleshooting rewritten
with the real mechanism. No debts.

## Next

v7-k ⭐ QP (Goldfarb-Idnani active-set · Mehrotra IPM · OSQP-class ADMM) — the MPC/contact/SVM workhorse, now
with `solve_kkt_dense`, the inertia machinery, and the exact TR subproblem solver already in place.
