# 2026-06-10 — hesap v7-o CLOSE: the algebraic modeling layer (same session, part 10)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slice:** v7-o — the JuMP/CasADi-pattern ergonomic façade. Continues the v7-f..n, l, m logs.

---

## `model.hpp` — `Model<T>`

- **Declarative:** `add_variable(s)` (+ bounds + starts) · `minimize(λ)` · `subject_to_eq/ge/le(λ)` — every
  function a **scalar-generic lambda** (the v7-b `DiffFunctor` contract: one templated
  `operator()(ConstSpan<S>) const` instantiable on T and Dual<T>). No expression graph, no reverse mode —
  C++ templates give the declarativeness for free; functions are held type-erased behind a tiny
  `IModelFn` vtable (T + Dual<T> entry points) owned via `crd::memory::construct/destroy`.
- **Auto-derivatives:** exact forward-mode AD throughout — the objective through the existing
  `FunctorObjective` driven by an `ErasedFnView` (a DiffFunctor view over the type-erased function),
  constraint Jacobians by per-row n-pass `forward_ad_gradient`, folded bound rows analytic (±e_j).
- **Deterministic dispatch** (overridable `ModelMethod`): general constraints → damped-BFGS SQP ·
  bounds-only → L-BFGS-B · unconstrained → L-BFGS · auglag selectable. With general constraints, finite
  variable bounds fold into c_I rows in a pinned order (user ineqs in add order, then lower-bound rows
  ascending, then upper-bound rows ascending — the `OptResult::multipliers` layout).

## Gates (39 asserts / 6 cases; suite 3467/109)

The sharp one is **WIRING EXACTNESS**: the model's unconstrained and bounds-only dispatches must be
**bit-identical** (same iteration count, bit-equal x) to calling `minimize_lbfgs`/`minimize_lbfgsb`
directly on the same functor — same algorithm + same AD gradients ⇒ same trajectory; any divergence is a
wiring bug by construction. Plus: declarative HS14 at the published f* through SQP and the auglag override ·
the le + folded-bound analytic vertex · an equality projection recovering the **analytic multiplier
λ* = (1−2√2)/2 through the model's AD-built Jacobian** · run-twice bit-identity.

## ⭐⭐ The gate caught a REAL v7-n-1 SQP bug (fixed in-session, no debt)

The le + folded-bound test jumped to the EXACT vertex (0.5, 1.5) in ONE iteration (the subproblem is the
problem itself there), then reported `SmallStep` with kkt_residual = 1.5: the next QP returned p = 0 with
the **exact multipliers**, but `minimize_sqp` fell into its `dphi ≥ 0 ⇒ SmallStep` branch without ever
adopting them or re-certifying. The v7-n battery never tripped this — its multipliers converge gradually
alongside the iterates, so the top-of-loop KKT check fires first. **Fix (`nlp_sqp.hpp`):** adopt the QP
duals and re-run the 4-part KKT certificate immediately after the subproblem, BEFORE the merit machinery —
when p ≈ 0 the QP duals ARE the NLP multipliers and "no merit descent" is convergence, not a stall (the
textbook SQP stopping point). Whole suite re-verified after the fix: zero regressions.

## Honest scope (named)

Hessian-free dispatch members only — the modeling layer does not synthesize second derivatives
(forward-over-forward nesting is a future refinement; exact-Hessian Newton/TR/filter-IPM remain raw-API).
Forward-AD costs n passes per function — the dense-small ergonomic path; large-scale work uses the raw
Objective/Constraints API with analytic or sparse derivatives.

## Verification

3467/109 on win-debug + win-asan + win-shipping; win-tidy build green; clang-format clean; 6 ctest guards
green.

## Next

p/q/r remain slip-candidates (no named consumer — decision at cluster close). **v7-z** is the remaining
row: CLI `hesap.opt.*` + the FULL gold-standard scoreboard (Ceres/liblbfgs/scipy/OSQP/qpOASES/quadprog/
IPOPT/PyTorch/HiGHS/SCS) + system doc + ADR finalization + the 18-config sweep. IPOPT still waits on the
ONE user sudo command.
