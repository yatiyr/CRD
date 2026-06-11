# 2026-06-10 — hesap v7-l + v7-m CLOSE: LP (revised simplex + Mehrotra) and conic (SCS-class) (same session, part 9)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slices:** v7-l LP · v7-m conic — taken after v7-n per user direction ("let's do v7-l and v7-m then"); no
reorganisation needed, both landed clean on the established v7-k machinery. Continues the v7-f..n logs.

---

## v7-l — `lp.hpp`: both LP members on the v7-k canonical form

`LpProblem`: min cᵀx s.t. l ≤ Ax ≤ u with optional variable bounds xlo ≤ x ≤ xup; OSQP-sign row duals.

- **`solve_lp_simplex` — the bounded-variable REVISED SIMPLEX** (the only new algorithm of the slice):
  slack working form [A −I]·v = 0 with the row bounds moved onto the slacks; TWO-PHASE with a sign-matched
  artificial basis (Phase-I optimum > 0 = the infeasibility certificate); **Dantzig pricing with the Bland
  fallback** after a 50-degenerate-pivot streak; bounded ratio test including **BOUND FLIPS**; free variables
  enter in either direction; equality-row slacks are fixed columns that never price in; explicit dense B⁻¹
  (eta update per pivot, Gauss-Jordan refactorization every 64 pivots); unbounded ray ⇒ DualInfeasible.
  Duals: y_report = −c_BᵀB⁻¹ lands the OSQP sign (q + Aᵀy = 0 on rows-only LPs — gate-pinned).
- **`solve_lp_mehrotra`** = the v7-k predictor-corrector at **P = 0** (finite variable bounds folded in as
  identity rows). Zero new algorithm — the QP IPM honestly subsumes LP.

Gates (893 asserts / 8 cases): analytic vertex LP (both members) · EXACT dual recovery checked directly ·
equality + free variable · the pure bound-flip path · **Beale's classic cycling example** (terminates at
obj = −1/20) · certified infeasibility/unboundedness · a **60-instance Philox cross-adjudication** (simplex
vs IPM agree ≤1e-5 on boxed-feasible-by-construction instances) · bit-identical determinism (both members).

## v7-m — `conic.hpp`: SCS-class operator splitting

`ConicProblem` in the SCS data form (min cᵀx s.t. Ax + s = b, s ∈ K); cone blocks Zero / Nonneg / SOC
(closed form) / **PSD via the v3 `dense::eig_sym` eigenvalue clamp** (full-matrix vectorization — a NAMED
divergence from SCS's scaled-lower-tri packing). `solve_conic_admm` is the v7-k OSQP iteration with the box
projection swapped for Π_C, C = {b} − K: same quasi-definite factor seam (Zero-block 1e3 ρ-boost), α = 1.6,
deterministic adaptive-ρ, **conic infeasibility certificates** (δy ∈ K* ∧ Aᵀδy ≈ 0 ∧ bᵀδy < 0 ⇒ primal;
−Aδx ∈ K ∧ cᵀδx < 0 ⇒ dual), termination = residuals **+ the duality gap** (the SCS criterion). The duals
come out in the SCS convention, which coincides with the v7-k/l OSQP sign — one certificate convention
across QP/LP/conic.

Gates (111 asserts / 7 cases): LP-as-conic reproduces the v7-l LP **including the exact duals** + the conic
KKT directly · analytic SOCP (norm ball: x* = p − r·c/‖c‖, y* = (‖c‖, c), s* on the boundary) · analytic
2×2 SDP (x* = 1, dual Y* = [[½,−½],[−½,½]], trace-complementarity) · mixed Zero+Nonneg vs the simplex ·
certified infeasibility both ways · a **25-instance Philox cross-adjudication vs the v7-l simplex** (the
THIRD independent algorithm family on the same instances) · bit-identical determinism (x, y, s, iterations).

Honest scope (named): no polish (SCS has none), no homogeneous self-dual embedding (certificates via
iterate differences), DENSE like v7-k/l; SCS/ECOS/HiGHS wall-clock rows live at v7-z.

## Verification

Opt suite **3432 asserts / 103 cases** on win-debug + win-asan + win-shipping; win-tidy build green;
clang-format clean; all 6 ctest guards green. Both slices' new-file tidy errors were two lowercase literal
suffixes (fixed).

## En route — the `#deps 0` landmine's THIRD head (root-caused + fixed, no debt)

win-shipping was found poisoned AGAIN (English `msvc_deps_prefix`, Turkish include-notes leaking, `#deps 0`)
despite the v7-j durable fix. The new mechanism: **a regenerate ever executed by the VS-fork CMake rewrites
`CMAKE_COMMAND:INTERNAL` in CMakeCache.txt to point at itself** — after that, even correctly-invoked
standalone `cmake --build` runs its regen rule through the fork. Audit = check BOTH the cache's
CMAKE_COMMAND and rules.ninja's prefix. Fixed: wiped + standalone-reconfigured win-shipping (Turkish prefix
+ `#deps 72 VALID` verified, suite green); added `scripts/{build-target,configure-preset,check-deps,
run-ctest}.bat` so every build/configure/ctest call has the policy baked in. CLAUDE.md Troubleshooting
extended. win-debug/asan/tidy/tidy-local audited healthy.

## Next

v7-o modeling layer (planned) · p/q/r slip-candidates · then **v7-z CLOSE** (CLI `hesap.opt.*` + the
gold-standard scoreboard — Ceres/liblbfgs/scipy/OSQP/qpOASES/quadprog/IPOPT/PyTorch/HiGHS/SCS — + system doc
+ ADR finalization + the 18-config sweep). IPOPT still waits on the ONE user sudo command.
