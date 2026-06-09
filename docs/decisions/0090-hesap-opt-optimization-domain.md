# ADR-0090 — crd-hesap-opt: the optimization domain (substrate + contracts + module edges, Phase 3.1.6 v7)

**Status:** Accepted (2026-06-07)

**Tags:** arch, hesap, opt, optimization, determinism, module-edges, substrate

## Context

Phase 3.1.6 v7 builds `crd-hesap-opt` — the optimization domain (unconstrained + constrained), **the universal
"find the best X" substrate** consumed across Cerid: eylem (constrained dynamics / IK / trajectory / powered-
ragdoll motors), estimation+control (MPC / LQR / Kalman / SLAM-bundle-adjustment), FEA/CFD (topology/shape/design
opt, parameter ID), CAD (sketch constraint solving), ML (training / differentiable sim), rendering (calibration /
BRDF-fit / photogrammetry-BA), games (AI/RL/CSP/balancing), robotics (inverse dynamics / motion planning /
calibration), DAW (filter design). Per user direction (2026-06-07) v7 **absorbs the old v8 constrained cluster** —
optimization is one domain. ADR-0065 reserved `crd-hesap-opt` (D14); this ADR records the domain's design contracts
+ module edges, pinned at the v7-a substrate so the ~18 subslices don't re-litigate them. Plan + subslices:
`docs/phases/phase-3.1.6-hesap.md` (v7 rows). Gold standards: Ceres · liblbfgs/scipy L-BFGS-B · NLopt ·
scipy.optimize · CMA-ES · PyTorch/JAX · OSQP · IPOPT · SCS · HiGHS.

## Decision

1. **Matrix-free over `Objective<T>`** (raw lower-layer f32/f64, below the typed-units boundary — ADR-0078 §5).
   `value` (pure) + `n` (pure) + `gradient`/`hessian_vector` (virtual, default not-provided). **Capability contract:**
   `has_gradient()`/`has_hessian_vector()` are the queried capabilities (set in the protected ctor, LinearOp-style);
   `gradient()` returns true iff it filled `g`; the two MUST agree (optimizer asserts). If no analytic gradient,
   the optimizer finite-differences (v7-b). **Vtable append-at-end** (D135). **RESERVED slot:** a fused
   `value_and_gradient(x, g) -> T` (for objectives sharing work between f and ∇f — L-BFGS/LM/line searches want it)
   is appended at the vtable END when the hot path needs it; reserving it now keeps adding it non-breaking.

2. **The crush axis (gold-standard mandate, honest):** crush the peers where the algorithm allows (the v6/v5
   discipline — fair same-class peer at its best, matched accuracy, no parallel-vs-serial asterisks); honest
   parity + the moat where it doesn't. ⚠ wall-clock vs Python-wrapped peers (scipy/PyTorch/cyipopt) is contaminated
   by per-call/reverse-communication overhead (the v6-z lesson) — report matched-accuracy + the moat as the solid
   claims. Ceres (v7-e) + IPOPT/cyipopt (v7-n) are heavy installs — probe at their slices, not at close.

3. **Determinism moat (the differentiator no mainstream optimizer carries):** the optimizer is SERIAL scalar
   arithmetic; the objective eval may be parallel but bit-exact (`ParallelSparseLinearOp` / a bit-exact reduction)
   ⇒ the optimization TRAJECTORY + result are **bit-identical across {1..16} workers** ⇒ certifiable MPC/optimal-
   control (DO-178C / ISO 26262) + reproducible ML training + replay. Tested `[moat]` per slice (asserting
   convergence too — a maxed-out run makes a bit-identity moat vacuous). ⚠ stochastic optimizers (v7-i): the
   optimizer-UPDATE determinism is in-scope; full shuffled-minibatch reproducibility needs the v12-stats
   Philox/Threefry counter-RNG (not built — name the dep or build a minimal one; do not claim it without it).

4. **Line-search cost contract:** a line search may evaluate ∇f at trial points and return it via `g_out`
   (`grad_at_new_valid=true`, e.g. Wolfe); a value-only search (Armijo) leaves `g_out` untouched and the optimizer
   evaluates ∇f once at the accepted point. Keeps the per-iteration gradient count explicit for L-BFGS/CG.

5. **Workspace allocation:** every method takes `crd::memory::IAllocator*` and allocates its work vectors (x/g/p,
   line-search temps, L-BFGS m-history, trust-region subproblem) from it — the one documented convention (matches
   hesap), so 18 methods don't each invent a workspace pattern.

6. **Module edges (acyclic, one-way), named up front:** `crd-hesap-opt → crd-hesap` (LinearOp + cli) ·
   `crd-hesap-dense` (Vector + blas1 + small dense solves) · `crd-hesap-sparse` (Parallel/SparseLinearOp objective
   eval) · `crd-jobs` (the only parallel step). **LATER (named so methods don't re-litigate):** `→ crd-hesap-direct/
   iterative` (Newton + KKT/Schur solves, v7-g/n), `→ crd-hesap-eig` (exact trust-region / negative-curvature
   detection, v7-h). **Reverse-mode autodiff** (the separate ADR-0065 autodiff sub-module, later) plugs into the
   SAME `Objective` gradient interface when it ships — forward-mode + FD are the v7-b providers now.

## Consequences

- One optimization substrate (Objective/OptResult/OptOptions/convergence/LineSearch) that all 18 subslices inherit;
  the contracts above prevent per-slice re-litigation.
- A determinism moat across the whole optimization domain — the differentiator for safety-critical control/
  estimation and reproducible ML, none of OSQP/IPOPT/Ceres/scipy/PyTorch carries it.
- Newton/KKT/exact-TR reuse the moat-proven hesap direct/iterative/eig solves (a v5↔v6↔v7 bridge, not new kernels).

See `docs/systems/hesap-opt.md` and `docs/phases/phase-3.1.6-hesap.md` (v7 rows).
