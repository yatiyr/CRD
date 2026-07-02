# Phase 3.1.6 — v13: the Numerical-Analysis + Motion cluster (interpolation · quadrature · differentiation · trajectory generation)

> **Status: v13-a…q SHIPPED (2026-07-01) + linux-gcc-green + crushing; only v13-z close remains.** The
> certification-grade classical numerical-analysis layer + the mission-critical motion-primitive layer. This is the
> spec; the master phase doc (`phase-3.1.6-hesap.md`) carries the one-line roadmap rows + the per-slice crush verdicts.
> ADR-0095 to write at v13-z close.
>
> **▶ PROGRESS (2026-07-01).** `crd-hesap-interp` (a-f) ✅ · `crd-hesap-quadrature` (g-k) ✅ · `crd-hesap-diff` (l-m) ✅
> · `crd-hesap-motion` (n-q) ✅. Every method that has a peer crushes or matches it (scipy 5-835× · GSL match-or-beat ·
> JAX 57× · **the full arbitrary-state Ruckig-class OTG — single-DoF 1.94× and multi-DoF sync 1.26× — crushes Ruckig's
> OWN C++ library on speed while matching it bit-for-bit**). Sessions: `docs/sessions/2026-06-30-v13-quadrature-engine.md`
> · `2026-07-01-v13-jk-oscillatory-cubature.md` · `2026-07-01-v13-motion-ruckig-otg.md` · `2026-07-02-v13z-windows-close.md`.
>
> **▶ v13-z CLOSE (2026-07-02, Windows-verify session) — DONE:** Windows baseline (all 4 modules compile clean on MSVC
> for the first time; win-debug v13 suite green; caught+fixed a `[a,inf)` TEST_CASE name that broke catch_discover) ·
> **win-tidy pass CLOSED** (92 violations fixed across headers + the never-tidy'd test files; full check set = 0 on all 4
> targets) · **CLI `hesap.{interp,quad,diff,motion}.*`** shipped (8 commands, 11 CLI tests green) · **4 system docs**
> (`hesap-{interp,quadrature,diff,motion}.md`) + README index rows · **ADR-0095** (already Accepted) · **conformance
> guard** `crd-hesap-v13-no-exceptions` (status-not-exception pillar, ctest-registered, non-vacuous) · the crush
> scoreboard consolidated into the system docs; the determinism moat evidenced by the 56 run-twice/bit-identity
> assertions. **REMAINING:** the win-asan + win-shipping DoD configs (win-shipping cache found poisoned — wipe+reconfigure)
> + the user commit + the 18-config CI. The v12+v13 tree is uncommitted (agents never commit).
>
> **The quality bar (set by the consumers).** Cerid v13 is designed to power **satellites, commercial drones, robots, self-driving cars, and AAA games**. Those domains don't just want fast interpolation — they want a numerical layer that is **deterministic by construction, allocation-free in the hot path, exception-free, and error-estimate-exposing**, because that is exactly what DO-178C (avionics/space), ISO 26262 ASIL-D (automotive), IEC 61508, and ECSS demand. v13 is built to clear that bar from line one — which is also Cerid's competitive moat (the incumbents can't: GSL/QUADPACK `malloc`s its workspace, Boost.Math `throw`s, parallel BLAS isn't bit-reproducible without a ~2× opt-in).

---

## 0. Re-scoping note (important)

The original master-phase-doc row sketched v13 as a ~1.5-week / ~2300 LOC slice (splines + Gauss-Kronrod + finite differences). **That sketch is superseded.** To hit "gold-standard insanely complete" against the satellite/drone/robot/car/game consumer set, v13 is a **major cluster on the scale of v11-DSP / v12-stats** — ~**12–16 KLOC**, ~**450–550 tests**, **4 modules** (one extension + three new), ~**6–10 weeks**, **multi-session — do not marathon.** The catalog below is the ~70-algorithm gold-standard surface, each mapped to its peer, its error/robustness property, the reuse boundary, the gate, and the consumer domain.

---

## 1. The mission-critical quality bar — three moat pillars

Every public v13 entry point honors three contracts. They are not aspirational; each maps to a safety standard, and Cerid already enforces the substrate (`crd-no-malloc-allocator`, `crd-no-std-transcendental-check`, no-owning-STL, fixed-FP-reduction-order) — so v13 starts compliant where GSL/Boost would need a multi-year retrofit.

**Pillar 1 — Determinism by construction.** Fixed FP reduction order, no `-ffast-math`, pinned/disabled FMA contraction, no arbitrary parallel reductions, `crd::math::*` deterministic transcendentals (never `std::`). Same source ⇒ bit-identical output across compilers, opt levels, and thread counts. → DO-178C multicore determinism · ISO 26262 ASIL-D deterministic replay · the bit-reproducibility that *is* the verification evidence. The `{1,4,16}` moat audit + run-twice bit-identity gate apply to every deterministic kernel.

**Pillar 2 — Allocation-free streaming.** Every routine takes a **caller-provided workspace** (a pre-sized `Array`/`Span`); node/weight tables are `static constexpr`; **zero heap per call**. Adaptive algorithms are **iterative over a fixed-size subinterval array, never recursive** (a bounded-depth work-stack loop — the QUADPACK pattern, not textbook recursive adaptive Simpson). Every iterative refinement carries a hard `limit`/`max_iters` cap = the WCET knob, and returns a *not-converged status* rather than spinning. → MISRA C++:2023 21.6.x (no dynamic memory) · 8.2.10 (no runtime recursion) · AUTOSAR A18-5-x · WCET-analyzability · freedom-from-interference.

**Pillar 3 — Error-estimate-exposing, never a bare number.** Every result returns `{value, error_estimate, status, tolerance_met, eval_count, subdiv_count}` with the **error tier labelled** (see §3). Errors flow via **status enum, not exceptions/RTTI** (`noexcept` hot kernels; builds clean with `-fno-exceptions -fno-rtti`). Convergence tests are **tolerance comparisons, never FP `==`**, ANDed with the iteration cap. Defensive input validation rejects NaN/Inf/`a>=b`/unordered-knots with a `BadInput` status, never a trap or garbage. → IEC 61508 design-diversity cross-check · the V&V rejection contract · ISO 26262 / AUTOSAR A15-0-1 (no exception escape).

> **The honesty constraint that protects the moat:** an error *estimate* is **not** a guaranteed bound. v13 labels the tier on every API mode and never promotes a hedged estimate to a hard claim — in a certifiable system an over-claimed bound is a latent hazard. (This is the v10/v11/v12 honest-scoreboard scar, applied to numerical error reporting.)

---

## 2. Architecture — the two-layer contract (ADR-0078) + the universal result

**Upper layer (typed):** whole-array batch APIs carrying `crd::units` Quantity where meaningful (`Length`/`Time`/`Angle`), the build-once/evaluate-many object model (`PchipSpline`/`ChebyshevInterpolant`/…), the adaptive drivers with workspaces + status.

**Lower layer (raw):** allocation-free reentrant kernels in raw `f32`/`f64`/SIMD — the de-Boor span eval, the Clenshaw recurrence, the Gauss-Kronrod pair, the complex-step step — the real-time hot loop (satellite ephemeris eval, drone trajectory regen, robot IK Jacobian at kHz). Fixed per-element FP order (ADR-0078 §5).

```cpp
namespace crd::hesap::quadrature
{
enum class QuadStatus : crd::u8 { Converged, MaxSubdivisions, RoundoffLimited, Singularity, Diverged, BadInput };

template <typename T> struct QuadResult
{
    T value;             // best approximation (returned even on failure)
    T error_estimate;    // TIER-1 estimate — NOT a guaranteed bound (label the tier)
    QuadStatus status;
    crd::u32 eval_count, subdiv_count; // V&V cost bound + the fooled-estimator defense
    bool tolerance_met;
};

template <typename T> struct AdaptiveWorkspace        // allocated ONCE at init, reused every call
{ crd::containers::Array<T> a, b, result, error; };   // capacity = limit; never grows in integrate()

template <typename T, class Fn>
[[nodiscard]] QuadResult<T> integrate_adaptive(Fn&& f, T a, T b, T epsabs, T epsrel,
                                               crd::u32 limit, GaussKronrodKey key,
                                               AdaptiveWorkspace<T>& ws) noexcept; // bounded loop, no heap, no recursion
}
```

Interpolants are **build-once / O(1)-amortized-evaluate**: PCHIP/spline/Chebyshev precompute tangents/coefficients/weights at build; per-query cost is one segment's polynomial (local support) with a cached last-segment index (O(1) uniform, O(log n) non-uniform documented). Real-time variants mandate a precomputed segment map for a true O(1) WCET bound.

---

## 3. The error-certification tiers (the V&V-facing contract)

| Tier | What it is | Where v13 uses it | The contract |
|---|---|---|---|
| **Tier 1 — heuristic estimate** | Gauss-Kronrod `\|K−G\|` (+ QUADPACK rescale + roundoff floor); embedded-RK local error; the cheap, *foolable* (Lyness-Kaganove hidden-peak), default. | every `integrate_adaptive` result | field is `error_estimate`, never `error_bound`; also expose `eval_count`/`subdiv_count` so an outer harness can flag suspiciously-few subdivisions; surface the roundoff floor as `RoundoffLimited`. |
| **Tier 2 — a-priori certified bound** | a *real* upper bound, conditional on a supplied/computed derivative bound: cubic spline `‖f−s‖∞ ≤ (1/384)h⁴‖f⁽⁴⁾‖∞` (constant proven optimal); poly `(1/(n+1)!)‖f⁽ⁿ⁺¹⁾‖∞‖∏(x−xⱼ)‖`. | a distinct `worst_case_error(h, deriv_bound)` query on the spline/poly interpolants | strictly stronger than Tier 1; labelled "certified mode"; only valid when the derivative bound is supplied/rigorous. |
| **Tier 3 — rigorous enclosure** | interval arithmetic (IEEE 1788-2015) / Taylor-Chebyshev models / self-validating quadrature — a proven `[lo,hi]` containing the true result, rounding included. | a `v13-close` stretch: an interval-valued result type for the highest-assurance path (tight + guaranteed + cheap — pick two). | the interval *is* the guarantee; opt-in, at cost. |

---

## 4. Module decomposition + edges (acyclic) + the SANITY-8 reuse map

| Module | New/extend | Owns | Edges (consumes) |
|---|---|---|---|
| **`crd-hesap-interp`** | NEW | 1-D + scattered/gridded N-D interpolation | core/containers/memory · math (deterministic) · **dense** (tridiagonal/LU/Cholesky for spline+RBF systems; `interp_decomp` for RBF low-rank) · **fft** (DCT for Chebyshev) · **special** (where needed) · **geometry-delaunay** (Sibson NNI — reuse) |
| **`crd-hesap-quadrature`** | **EXTEND** (v12-c shipped the Gauss nodes) | the `integrate()` API + adaptive drivers + cubature | adds the adaptive workspace/status; reuses its own Golub-Welsch nodes + `eig_sym` + **fft** (Clenshaw-Curtis DCT) |
| **`crd-hesap-diff`** | NEW | numerical differentiation | core/containers/memory · math · **fft** (spectral diff) · **dsp** (Savitzky-Golay infra) |
| **`crd-hesap-motion`** | NEW | mission-critical trajectory generation | **interp** · **quadrature** · **special** (Fresnel→clothoid) · **math** (quaternion slerp/log/exp) · **opt** (the min-snap QP) · **geometry-curves** (B-spline/Catmull-Rom — reconcile, not duplicate) |

**Reuse boundary (SANITY 8 — search before you build, confirmed by the codebase agent):**

| Capability | Already ships in | v13 action |
|---|---|---|
| Gauss-Legendre/Hermite/Laguerre/Jacobi/Gegenbauer/Chebyshev nodes+weights | `crd-hesap-quadrature` (Golub-Welsch over `eig_sym`) | REUSE; add Lobatto/Radau + the integrate API |
| Cubic Hermite eval (allocation-free, generic) | `crd-hesap-ode` `hermite_eval` | REUSE as the PCHIP/Hermite base case |
| Tridiagonal / SPD / LU / QR / SVD solve | `crd-hesap-dense` | REUSE for spline + RBF + poly-LS systems |
| Interpolative decomposition (low-rank) | `crd-hesap-dense` `interp_decomp` | REUSE for RBF conditioning |
| FFT / DCT | `crd-hesap-fft` | REUSE for Chebyshev fit, Clenshaw-Curtis, Fourier diff, trig interp |
| Fresnel S/C | `crd-hesap-special` | REUSE for clothoid eval |
| Quaternion slerp/nlerp + (log/exp) | `crd-math` `quat.hpp` | REUSE for SQUAD + quaternion-B-spline |
| Catmull-Rom / B-spline / Bézier / Hermite parametric curves + Sibson NNI | `crd-geometry-curves`, `crd-geometry-delaunay` | RECONCILE (parametric eval exists; v13 adds DATA-fitting + NURBS) |
| QP solver | `crd-hesap-opt` | REUSE for minimum-snap |
| FD gradient | `crd-hesap-opt` `finite_difference` | EXTEND / migrate general FD to `crd-hesap-diff` |
| deterministic sin/cos/exp/log | `crd::math` | REUSE everywhere (the moat substrate) |

---

## 5. The complete algorithm catalog → sub-slices (a→z)

Each row's `★` = the modern best-in-class pick for its job. Status: ✅ ships · ◐ partial · ∅ gap. Gate column abbreviates the per-slice gold-standard gate.

### `crd-hesap-interp`

**v13-a — 1-D piecewise + the interface + the safety contract.** linear, nearest/previous/next, **cubic Hermite** (reuse ODE `hermite_eval`), **★PCHIP** (Fritsch-Carlson, monotone, *no-overshoot* — the certifiable control-LUT default). Interface: `Interpolator<T>` build-once/eval-many, the workspace/status contract, `worst_case_error` (Tier 2). Gate: scipy `interp1d`/`PchipInterpolator` bit-close + the **no-overshoot invariant on monotone data** + the certified spline-error bound.

**v13-b — cubic splines (all boundary conditions).** natural / clamped / **★not-a-knot (default)** / periodic — the tridiagonal (cyclic for periodic) system via `dense`. Gate: scipy `CubicSpline(bc_type=…)` bit-close + C² continuity + the `(1/384)h⁴` Tier-2 bound.

**v13-c — local + stable-polynomial 1-D.** Akima + **★modified Akima (makima)** (no flat-region overshoot) · **★barycentric Lagrange** (2nd form, backward-stable, O(n) eval) · Newton divided differences (incremental) · **★Floater-Hormann** (barycentric rational, *pole-free on all real nodes* — the equispaced-data answer). Gate: scipy/MATLAB `makima`/`BarycentricInterpolator` + conditioning checks (barycentric on Chebyshev nodes vs Runge).

**v13-d — spectral + rational 1-D.** **★Chebyshev interpolation** (DCT-fit via `fft` + Clenshaw recurrence — near-minimax, exponential convergence) · trigonometric/Fourier (via `fft`) · rational/Padé (with spurious-pole guard). Gate: Chebfun/`numpy.polynomial.chebyshev` + exponential-convergence check on an analytic f.

**v13-e — scattered N-D (RBF + Shepard).** RBF: multiquadric · inverse-multiquadric · Gaussian · **★thin-plate spline** (min-curvature 2-D warp) · polyharmonic · **★Wendland compact-support** (sparse, O(n)-solvable — large real-time scattered sets) — over `dense` + `interp_decomp` conditioning · Shepard/IDW · Sibson NNI (reuse geometry-delaunay). Gate: scipy `RBFInterpolator(kernel=…)` + the SPD/conditionally-PD poly-tail correctness + reproduction-of-low-order-polynomials.

**v13-f — gridded N-D + uncertainty.** bilinear/trilinear (bounded, no-overshoot) · bicubic/tricubic · tensor-product B-splines · Clough-Tocher C¹ (over Delaunay) · **★kriging / Gaussian-process** (returns mean **+ predictive variance** — the safety-critical sensor-fusion interpolant). Gate: scipy `RegularGridInterpolator`/`RectBivariateSpline`/`CloughTocher2DInterpolator` + sklearn `GaussianProcessRegressor` (mean+var).

### `crd-hesap-quadrature` (extend)

**v13-g — the integrate API + nodes + the result contract.** Gauss-**Lobatto** (endpoints — FEM/SEM) + Gauss-**Radau** (one endpoint — stiff collocation) · the `integrate()` / `QuadResult` / `AdaptiveWorkspace` / `QuadStatus` surface · composite trapezoid/Simpson on samples (telemetry/odometry) · Newton-Cotes (low order only; assert positive weights). Gate: exactness degree (Gauss-n exact ≤2n−1, Lobatto ≤2n−3) + the positive-weight invariant + scipy `simpson`/`trapz`.

**v13-h — the adaptive engine (the real-time-safe core).** **★Gauss-Kronrod** (G7-K15 … embedded error) + the **iterative bounded-depth driver**: QNG (non-adaptive cascade) · QAG (bisect-worst) · **★QAGS** (+ Wynn-ε extrapolation, endpoint singularities — the default robust integrator) · QAGP (user break-points) · QAGI (infinite range). Gate: QUADPACK/`scipy.quad` value + `\|G−K\|` estimate within bounds + **the no-recursion / fixed-workspace structural guard** + the Lyness-Kaganove hidden-peak honesty test (must report low `subdiv_count` ⇒ caller can reject).

**v13-i — non-Gauss + double-exponential.** **★Clenshaw-Curtis** + Fejér 1st/2nd (Chebyshev points, FFT weights, nested, positive weights) · **★tanh-sinh / exp-sinh / sinh-sinh** (double-exponential — endpoint singularities & infinite ranges & arbitrary precision; the Boost.Math peer) · Romberg (smooth fixed grids). Gate: Boost.Math `tanh_sinh`/`exp_sinh` + Clenshaw-Curtis vs Gauss accuracy-per-eval + singular-endpoint integrands.

**v13-j — oscillatory + singular weights.** **★QAWO** (Filon-type modified Clenshaw-Curtis — ω-independent cost) · QAWF (Fourier tail + ε-accel) · QAWS (algebraico-log endpoints) · QAWC (Cauchy principal value) · **★Levin collocation** (general/Bessel oscillators where Filon moments don't exist). Gate: QUADPACK `qawo`/`qawf`/`qaws`/`qawc` + accuracy-grows-with-ω check.

**v13-k — multi-D cubature.** tensor-product Gauss (d≲4) · **★Genz-Malik** adaptive (degree-7 + embedded-5, d≈2–7) · **★Smolyak sparse grids** (breaks the curse for smooth f, dimension-adaptive) · **★Lebedev** (sphere — exact for Yₗₘ, minimal points: attitude/BRDF/sky) · simplex cubature (Dunavant — FEM/CFD on unstructured meshes). (QMC already in `crd-hesap-stats` — cross-reference.) Gate: `scipy`/Cubature/Tasmanian + spherical-harmonic exactness (Lebedev) + simplex exactness degree.

### `crd-hesap-diff`

**v13-l — finite differences (done right).** **★Fornberg arbitrary-stencil weights** (any nodes, any derivative order, stable generator) · central/forward/backward with **per-scheme + per-magnitude optimal step** (never hard-coded h) · **★Richardson / Ridders** (error-killing extrapolation + estimate) · matrix FD Jacobian + Hessian-vector. Gate: `numpy.gradient`/`findiff`/NR `dfridr` + the documented accuracy floors (forward √ε, central ε^⅓).

**v13-m — exact + spectral + noisy.** **★★complex-step** `Im[f(x+ih)]/h` (machine-exact, *zero subtractive cancellation*, no h-tuning — the modern best gradient when f takes complex args: aero/MDO/satellite sensitivity) · **★Chebyshev / Fourier differentiation matrices** (spectral, via `fft`/barycentric) · **★Savitzky-Golay** differentiation (noisy IMU/telemetry rates). Gate: complex-step = analytic-exact (≤ machine ε) · spectral-diff exponential convergence · scipy `savgol_filter(deriv=k)`.

### `crd-hesap-motion` — ✅ SHIPPED (2026-07-01), motion suite 37289 asrt / 11 cases; session `docs/sessions/2026-07-01-v13-motion-ruckig-otg.md`

**v13-n — attitude trajectories. ✅** **★SQUAD** (C² spherical cubic via nested SLERP) · **★quaternion cubic B-spline** (Kim-Kim-Shin cumulative-basis, C² manifold-correct via log/exp) — added `quat_log`/`quat_exp` to **crd-math** (SANITY-8; quat suite still 64-green). Gate met: unit-norm 1e-16 + C⁰/C¹ at knots + endpoint interpolation.

**v13-o — continuous-curvature paths. ✅** **★clothoid / Euler spiral** (curvature linear in arc length, closed-form via `special::fresnel`) · **★NURBS** (Cox-de Boor rational B-splines — exact unit circle to 1e-12). Gate met.

**v13-p — optimal polynomial trajectories. ✅** **★minimum-snap MULTI-SEGMENT** (`min_snap_trajectory` — the Mellinger `pᵀQp` QP: minimize ∫snap² s.t. waypoints + C³ continuity + BCs, via the equality-constrained KKT linear system over **hesap-dense** LU; new acyclic motion→hesap-dense edge) + min-jerk quintic + min-snap septic BVP. Gate met: waypoints + C³ continuity + zero-boundary-derivatives.

**v13-q — jerk-limited real-time motion. ✅ (the headline)** S-curve / trapezoidal profiles · **Kochanek-Bartels TCB** · **★★rest-to-rest multi-DoF time-sync** (`plan_synchronized`, matches ruckig.duration exactly) · **★★★the FULL arbitrary-state Ruckig-class OTG** — a faithful reimplementation of Ruckig's third-order position solver (Berscheid-Lien 2021, MIT), both step1 (`otg.hpp` `plan_otg`, min-time from any state + brake) and step2 (`otg_sync.hpp` `plan_otg_timed`/`plan_synchronized_otg`, reach-in-exactly-tsync + multi-axis sync). Deterministic (crd::math, fixed order) · allocation-free (stack) · WCET-bounded (finite candidate set + fixed-iteration Newton). **⭐⭐⭐ CRUSHES RUCKIG'S OWN C++ (`libruckig.a` Release): single-DoF 0/2000 duration-mismatch (bit-EXACT) + 1.94× faster; multi-DoF sync 0/2000 tsync-mismatch (bit-EXACT) + 0 reach-fail + 1.26× faster.** Reconstruct-verified in python 1934/1934 (step1) + 2474/2474 (step2) vs the ruckig package. The bounded-WCET real-time guarantee (Pillar 2) is preserved *while beating Ruckig's throughput.* ⚠ noted extension only: 2nd-order (velocity-interface) OTG + Ruckig's UDUD sextic time_vel branch (not needed — the 9 cases hit 2474/2474).

### Close

**v13-z — CLI + docs + ADR + conformance audit.** CLI `hesap.interp.*` / `hesap.quad.*` / `hesap.diff.*` / `hesap.motion.*` (curated agent subset). System docs: `hesap-interp.md`, `hesap-diff.md`, `hesap-motion.md` + rewrite `hesap-quadrature.md`. ADR-0095. The **all-peers crush scoreboard** + the **`{1,4,16}` determinism-moat audit** + **the safety-critical conformance audit** (new guards: builds `-fno-exceptions`; the adaptive-driver-uses-a-workspace-not-recursion structural check; no-heap-in-hot-path; status-not-exception).

---

## 6. Gate strategy — four axes, all honest

1. **Correctness (tiered):** bit-for-bit / ≤1e-12 vs scipy.interpolate, scipy.integrate, MATLAB, Boost.Math, GSL/QUADPACK where deterministic; **certified-bound checks** (Tier 2: the analytic spline/poly error bound; the Gauss exactness degree); **robustness invariants** (PCHIP no-overshoot on monotone data; positive-weight-sum for every quadrature rule; complex-step machine-exactness; barycentric-on-Chebyshev beats Runge).
2. **Determinism moat:** `{1,4,16}` bit-identical + run-twice bit-identity on every deterministic kernel; the new structural guards.
3. **Crush (all peers, named):** per-call throughput vs scipy/MATLAB/Boost/GSL — the native-C++ + precomputed-nodes + reused-factorization lever (same engine that gave Ridge 47× / KDE 5872×). Honest where a peer wins (e.g. Boost's hand-tuned tanh-sinh).
4. **Certification-readiness (the moat axis the incumbents fail):** builds clean `-fno-exceptions -fno-rtti`; zero heap in the compute path (workspace pattern); adaptive = iterative bounded-depth, not recursive; status-not-exception; error-tier labelled. This is a scoreboard column GSL (mallocs), Boost (throws), and parallel BLAS (non-reproducible) structurally lose.

---

## 7. Where v13 is used — the consumer map

**Satellites:** Chebyshev ephemeris eval + **exact derivative-of-Chebyshev** velocity (SPK Type 2/3 pattern); sliding-window Lagrange/Hermite (SP3 9th-degree, Runge-suppressed); **SLERP/SQUAD** attitude (unit-norm preserved); ground-station-pass **quadrature** over time; orbit root-finding for elevation crossings. *Theme: deterministic, long-duration-stable, exactly-differentiable.*

**Drones:** **minimum-snap** trajectories (differential flatness → bounded motor commands), regenerated at high rate within a **bounded onboard compute budget** (Pillar 2).

**Robots:** **jerk-limited S-curve / Ruckig** motion (vibration/wear/tracking); real-time **IK Jacobians** via complex-step/FD at kHz; PCHIP **lookup tables** (no-overshoot actuator maps); **Savitzky-Golay** velocity from noisy encoders.

**Self-driving:** **clothoid** continuous-curvature steering (bounded lateral jerk); **spline pose interpolation** keyed by asynchronous LiDAR/camera/IMU timestamps (continuous-time estimation); **ASIL-D deterministic replay** of the planning/interpolation layer for incident analysis.

**Games:** **Catmull-Rom / Kochanek-Bartels** camera & keyframe animation; fixed-timestep ↔ render **state interpolation** (`alpha`-lerp); **RBF / noise** procedural terrain; LOD. *Tolerance: plausible, not exact.*

**Downstream hesap + the engine:** v14 tensors (Chebyshev/quadrature) · v15-16 autodiff (complex-step/FD as the cross-check & fallback) · **Phase 3.1.10 CFD + 3.1.12 FEA** (Gauss-Lobatto element integration, simplex cubature — the single most-reused v13 capability for the engineering-calc thesis) · **Phase 3.1.11 estimation/control** (consumes `crd-hesap-motion`: planning/MPC sits on the motion primitives) · **eylem** (energy integrals, spline motion) · **Phase 3.1.16 sciviz** + the v18 notebook (resampling/contouring).

---

## 8. Risks + honest scope

- **`crd-hesap-motion` straddles the hesap/control boundary.** v13 ships the motion *primitives* (SQUAD, clothoid, min-snap, jerk-limited profiles, NURBS, Ruckig-class OTG). The *planning* (RRT/A*/MPC/path-search) is **Phase 3.1.11 control**, consuming v13. Keep the edge one-way.
- **Kriging/GP** is a statistical model (O(n³) train) — reuse `crd-hesap-stats` covariance + `crd-hesap-dense`; cap exact GP to moderate n, note sparse/inducing as a follow-on.
- **Tier-3 interval/Taylor-model quadrature** is a `v13-close` stretch (tight+guaranteed+cheap — pick two); ship Tier 1+2 as the baseline, Tier 3 opt-in.
- **No-clean-gold-peer cases** (per the v12-z doctrine — state, don't invent): some domain trajectory pieces (Ruckig time-optimality, clothoid G² fitting) gate against their reference implementations + analytic invariants, not a scipy bit-match.
- **Reconcile, don't duplicate:** geometry-curves already ships parametric Catmull-Rom/B-spline/Bézier/Hermite eval; v13 adds *data-fitting* + NURBS + the manifold quaternion splines — one-way edge `motion → geometry-curves`.
- **Size honesty:** ~12–16 KLOC over ~17 sub-slices is a multi-session cluster. Do not marathon; close per-slice on the 4-config DoD; cluster-close on the 18-config sweep.

---

## 9. Sequencing

Interp (a–f) → quadrature-extend (g–k) → diff (l–m) → motion (n–q) → close (z). Motion depends on interp + quadrature + opt + special, so it lands last. Recommended: **commit v12 (q/r/z) first**, then open v13-a with ADR-0095 (the cluster decision: the 4-module split + the safety-critical numerical contract + the three moat pillars).
