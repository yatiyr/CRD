# 2026-06-10 — hesap v7-n-2 ⭐ CLOSE: the filter interior point + the IPOPT probe (same session, part 8)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slice:** v7-n-2 — the third member of the NLP trio (Wächter-Biegler 2006) + the IPOPT install probe.
Completes v7-n. Continues the v7-f..k, n-1 logs.

---

## `nlp_interior_point.hpp` — `minimize_interior_point`

- **Slack form** c_I(x) − s = 0, s > 0, barrier −μΣln s; primal-dual variables (x, s, y_E, z).
- **The reduced Newton system lands exactly on the v7-j machinery:** eliminating (Δs, z⁺) gives
  [W + J_IᵀΣJ_I, J_Eᵀ; J_E, 0] with Σ = z/s — solved by `solve_kkt_dense`, whose (n+, m−, 0) inertia test +
  δ·I ladder IS IPOPT's inertia-correction mechanism. W = the exact Lagrangian Hessian through the v7-j
  `add_lagrangian_hessian` hook, evaluated at (y_E, z).
- **Filter line search** (the W-B globalization): fraction-to-boundary on s and z (τ = max(0.99, 1−μ)), then
  acceptance vs the current point AND every filter entry by the (γ_θ, γ_φ) margins; the switching condition
  (δ, s_θ, s_φ from the paper) routes f-type steps to Armijo-on-φ_μ; θ-type acceptances augment the filter;
  the filter resets per barrier problem (initialized with the θ_max cap row).
- **Monotone Fiacco-McCormick μ:** each barrier problem solved to E_μ ≤ κ_ε·μ, then
  μ ← max(tol/10, min(κ_μ·μ, μ^{θ_μ})) with the IPOPT defaults (κ_ε=10, κ_μ=0.2, θ_μ=1.5); outer stop on the
  NLP error E_0 ≤ tol. The κ_Σ clip keeps z in [μ/(κ_Σ s), κ_Σ μ/s].

## Tests — the full battery through the third method (v7-n total: 80 asserts / 8 cases)

HS6 (the mi=0 degenerate-barrier path) · HS14 (published f* to 1e-7; z ≥ 0 by construction) ·
Rosenbrock-in-the-unit-disk (the scipy reference x*; boundary multiplier > 0) · the circle projection — **the
analytic λ* = 1 − √5 has now been reproduced by FIVE independent algorithms** (v7-j Newton-SQP, v7-n SQP,
auglag, and the IPM, plus the v7-j multiplier-LS estimate) · bit-identical run-twice determinism.
Suite **2428 asserts / 88 cases**; debug + asan + shipping + tidy + guards green.

## The IPOPT/cyipopt install probe (the row's de-risk task)

Probed on WSL: python 3.12.3 + pip 24.0 present; **`coinor-libipopt-dev` (Ipopt 3.11.9) is available in apt
but not installed, and sudo needs a password** — the install is ONE user command:
`sudo apt-get install -y coinor-libipopt-dev pkg-config`, after which `scripts/setup-ipopt-ref.sh` (written
this slice, the setup-lbfgsb-ref pattern: apt gate → `pip3 install --user cyipopt` → import check) finishes
the job. **Honest note in the script:** apt's 3.11.9 is ~2014-era — a fine correctness/iteration-count peer,
but the v7-z WALL-CLOCK rows should coinbrew a modern Ipopt 3.14 with MUMPS (which we already build for the
v5 benches; IPOPT can reuse it) — the recipe pointer is in the script.

## Honest scope (named)

No restoration phase (a floored backtracking reports `LineSearchFailed` instead of feasibility restoration);
no second-order correction inside the IPM (the SQP carries SOC); unscaled optimality error (s_d = s_c = 1 —
scaling is v7-z polish). Exact-Hessian only by design — SQP (damped BFGS) and auglag (L-BFGS) are the
Hessian-free members of the trio.

## v7-n is COMPLETE

SQP · augmented Lagrangian · filter interior point — all three NLP methods shipped, sharing the v7-j
interfaces/certificate and the v7-k QP subproblem, validated on one battery by cross-reproduction.

## Next

v7-l LP (revised simplex + Mehrotra) · v7-m conic (SCS-class) · v7-o modeling layer · then **v7-z** (CLI
`hesap.opt.*` + the gold-standard scoreboard — Ceres/liblbfgs/scipy/OSQP/IPOPT/PyTorch — + system doc + the
18-config sweep + cluster close). The p/q/r slip-candidates per the plan.
