# ADR-0095 — the v13 Numerical-Analysis + Motion cluster: the 4-module split, the three certification moat pillars, and the error-tier contract

- **Status:** Accepted (2026-06-29)
- **Phase:** 3.1.6 v13 (Numerical-Analysis + Motion cluster)
- **Tags:** `hesap` `interpolation` `quadrature` `differentiation` `motion` `determinism` `safety-critical` `architecture` `module-edges` `substrate`
- **Plan:** `docs/phases/phase-3.1.6-v13.md` (the v13 DETAILED PLAN — the full algorithm catalog + sub-slice table + the architecture); memory `project_v13_numerical_motion_plan`; system docs at v13-z.

## Context

v13 completes the classical numerical-analysis layer (interpolation · quadrature/integration · numerical differentiation) and adds the mission-critical **motion-primitive** layer (trajectory generation). The driving requirement: Cerid is built to power **satellites, commercial drones, robots, self-driving cars, and AAA games** — so this layer is held to a **certification-grade** bar, not a "fast enough" bar. The original roadmap sketched v13 as a ~1.5-week / ~2300-LOC slice (splines + Gauss-Kronrod + finite differences); against the consumer set + the ~70-algorithm gold-standard surface, that is **re-scoped to a major cluster** (~13 KLOC, ~17 sub-slices, multi-session — the scale of v11-DSP / v12-stats). Four things had to be settled before line one.

1. **The module split** — interpolation, integration, differentiation, and motion are distinct capabilities with distinct edges; one module would tangle them.
2. **What "correct" and "trustworthy" mean** for numerical code that a safety case will depend on — and crucially, the difference between an error *estimate* and a *guaranteed bound*.
3. **The real-time / safety-critical contract** the API must obey (the standards: DO-178C/DO-330, ISO 26262 ASIL-D, IEC 61508, ECSS, MISRA C++:2023 / AUTOSAR C++14).
4. **The reuse boundary** — what already ships (geometry curves, the Gauss nodes, ODE dense output, FD gradients) so v13 extends, never duplicates.

## Decision

1. **Four modules (1 extend + 3 new; module-isolation cornerstone).**
   - **`crd-hesap-interp`** (NEW) — 1-D (piecewise/spline/local-poly/spectral/rational) + scattered & gridded N-D interpolation.
   - **`crd-hesap-quadrature`** (EXTEND — v12-c shipped only the Gauss nodes) — the `integrate()` API + the adaptive QUADPACK family + cubature.
   - **`crd-hesap-diff`** (NEW) — numerical differentiation (Fornberg/Richardson/complex-step/spectral/Savitzky-Golay).
   - **`crd-hesap-motion`** (NEW) — the mission-critical trajectory layer (SQUAD/clothoid/minimum-snap/NURBS/jerk-limited OTG). It owns the motion *primitives*; the *planning* (RRT/A*/MPC) is Phase 3.1.11 control, consuming it (one-way edge).

2. **The three certification moat pillars — every public entry point obeys them** (and Cerid's existing guards — `crd-no-malloc-allocator`, `crd-no-std-transcendental-check`, no-owning-STL, fixed-FP-reduction-order — already enforce the substrate, so v13 starts compliant where GSL/Boost would need a multi-year retrofit):
   - **Determinism by construction** — fixed FP order, `crd::math` not `std::`, no fast-math, pinned/disabled FMA contraction; same source ⇒ bit-identical across compilers/opt-levels/threads. → DO-178C multicore determinism · ISO 26262 ASIL-D deterministic replay.
   - **Allocation-free streaming** — caller-provided workspaces (zero heap per call); adaptive algorithms are **iterative over a fixed-size work-stack, never recursive**; a hard iteration `limit` = the WCET knob; non-convergence returns a *status*, never spins. → MISRA 21.6.x (no dynamic memory) · 8.2.10 (no runtime recursion) · WCET-analyzability.
   - **Error-tier-exposing** — results return `{value, error_estimate, status, tolerance_met, eval_count, …}`, errors flow via **status enum not exceptions/RTTI** (`noexcept`, builds clean `-fno-exceptions`), convergence is a tolerance test (never FP `==`), inputs are defensively validated (NaN/Inf/unordered → status). → IEC 61508 design-diversity cross-check · the V&V rejection contract.

3. **The error-certification tiers (named on every API mode — the honest-scoreboard scar applied to error reporting).** **Tier 1** = a heuristic estimate (Gauss-Kronrod `|K−G|`, embedded-RK) — *foolable* (Lyness-Kaganove hidden peak), labelled `error_estimate`, never `error_bound`. **Tier 2** = an a-priori certified bound, *conditional* on a supplied derivative bound (linear `h²/8·max|f″|`, cubic spline `(1/384)h⁴·‖f⁽⁴⁾‖`) — a distinct "certified mode" query. **Tier 3** = a rigorous interval/Taylor-model enclosure (a `v13-close` stretch). **An estimate is NEVER promoted to a bound** — in a certifiable system an over-claimed bound is a latent hazard.

4. **The two-layer ADR-0078 architecture** — typed whole-array batch upper (the build-once/evaluate-many object model) + allocation-free raw streaming kernels lower (the de-Boor span eval, the Clenshaw recurrence, the Gauss-Kronrod pair — the real-time hot loop).

5. **SANITY rule 8 — reuse, don't reimplement.** interp/quadrature/diff/motion ride the shipped Gauss nodes + `eig_sym`, the ODE cubic-Hermite cousin, the dense tridiagonal/LU/SVD + `interp_decomp`, the FFT/DCT, `special::fresnel`, the `crd-math` quaternion ops, the geometry-curves B-spline/Catmull-Rom + the geometry-delaunay Sibson NNI, and the `crd-hesap-opt` QP. New acyclic edges all one-way; nothing v13 depends on references v13.

## Consequences

- v13-a ships the `crd-hesap-interp` substrate (the `Interpolator` build-once/eval-many object + `InterpStatus` + the caller-workspace contract) and the first kernels (linear · nearest · cubic Hermite · **PCHIP** — the monotone, no-overshoot, certifiable control-LUT default), gated ≤1e-12 vs scipy + the no-overshoot invariant + the Tier-2 linear bound + run-twice bit-identity.
- The scoreboard gains a **certification-readiness axis** beyond speed: builds `-fno-exceptions`, zero heap in the compute path, iterative-not-recursive adaptive, status-not-exception, error-tier labelled — a column GSL (`malloc`s its workspace), Boost (`throw`s), and parallel BLAS (non-reproducible without a ~2× opt-in) structurally lose.
- The downstream domain phases (3.1.10 CFD, 3.1.11 control, 3.1.12 FEA) build on this layer: Gauss-Lobatto + simplex cubature is the FEM/FVM element-integration kernel; `crd-hesap-motion` is what control planning sits on.
- Extends ADR-0065 (hesap substrate); consumes ADR-0078 (two-layer typed/raw); sibling to ADR-0094 (the v12 statistics cluster). Cluster closes at v13-z (CLI + system docs + the conformance audit).
