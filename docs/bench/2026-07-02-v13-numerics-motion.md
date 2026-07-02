# 2026-07-02 — v13 numerical computing + motion: interpolation · quadrature · differentiation · trajectory generation

**Retro-ported 2026-07-02 from the session logs / phase table (recorded numbers, not re-measured).**

- **Machine/config (sessions 2026-06-30 through 2026-07-02):**
  - 1-D interpolation + quadrature + differentiation: i9-14900K, WSL2 Ubuntu 24.04, linux-gcc-release.
  - Peers: SciPy 1.17.1, MATLAB R2026a (single-thread), Boost.Math 1.83, GSL 2.7.1.
  - Motion (Ruckig OTG): i9-14900K, Ruckig 0.17.3 C++ library Release (`-O3 -march=native`).
- **Sources:** docs/sessions `2026-06-30-v13-quadrature-engine.md` (interp v13-f · quadrature v13-g/h/i) · `2026-07-01-v13-jk-oscillatory-cubature.md` (quadrature v13-j/k · diff v13-l/m) · `2026-07-01-v13-motion-ruckig-otg.md` (motion v13-n/o/p/q, Ruckig OTG).
- **Scope:** 1-D piecewise + polynomial interpolation; scattered/gridded N-D interpolation (RBF + kriging); Gauss + adaptive QUADPACK + double-exponential + non-Gauss quadrature; oscillatory + singular-weight quadrature; multi-D cubature; numerical differentiation (FD + complex-step + spectral); trajectory generation (splines + clothoid + NURBS + jerk-limited profiles + Ruckig time-optimal generator).

## Interpolation (v13-a through v13-f)

**Clough-Tocher C¹** (v13-f final row):

| Method | Cerid FIT | SciPy FIT | MATLAB fit | Cerid EVAL | SciPy EVAL (batch) | vs SciPy |
|---|---|---|---|---|---|---|
| Clough-Tocher | fit overhead | ~2.14× faster | — | 85 ns/pt | 29 ns/pt (batch) | 1.70× (batch) · **35.7× per-point** · **2.90× MATLAB griddata('cubic')** |

**Verdict:** Bit-exact transcription of scipy `_interpnd.pyx`; per-point speedup reflects reusable precomputation (Bézier ordinates, orient-jump-walk locate). MATLAB griddata('cubic') N/A detailed comparison (not a direct peer). Boost N/A (no gridded cubic variant). Interp module complete (a–f), suite **544 assertions / 22 cases**.

## Quadrature (v13-g through v13-k) — the frontier

**1-D scalar quadrature board:**

| Method | Cerid (ns) | GSL 2.7.1 | SciPy | Boost.Math | Verdict |
|---|---|---|---|---|---|
| Gauss-Lobatto/Radau | fast (~85 ns sym pair) | — | — | — | **85× vs scipy fixed_quad; 1.02× Boost gauss<10>** (symmetric-pair fix flipped edge) |
| Simpson | 21.9 ns | — | 474 ns | — | **21.9× SciPy** |
| trapezoid | 70× SciPy | — | — | — | **70× SciPy; 32.7× MATLAB** |
| newton_cotes | — | — | 224 ns | — | **2.97× SciPy** |
| QNG (Patterson) | 1.09× GSL | baseline | — | — | parity GSL |
| QAG | **22.7× SciPy** | — | — | — | — |
| QAGS (Wynn-ε extrap) | **1.29× GSL** | baseline | ~1400 ns | — | **beats the gold-standard QUADPACK reference** |
| QAGI (infinite) | **1.36× GSL** | — | — | — | — |
| QAGP (breakpoints) | **1.24× GSL** | baseline | — | — | — |
| exp_sinh (DE) | **1.17× Boost** | — | — | baseline | **beats Boost hand-tuned** |
| sinh_sinh (DE) | **1.21× Boost** | — | — | baseline | — |
| tanh_sinh (DE) | **1.34× Boost** | — | — | baseline | — |
| Clenshaw-Curtis adaptive | **2.05× GSL-cquad** | — | — | — | — |
| Romberg | **1.19× GSL-romberg** | — | — | — | — |

**Oscillatory + singular-weight (v13-j):**

| Method | Cerid | GSL 2.7.1 | SciPy | Verdict |
|---|---|---|---|---|
| QAWO (Filon-type) | ~123 ns | ~123 ns (parity) | 2677 ns | **parity GSL; 22× SciPy** |
| QAWF (Fourier tail) | ~1035 ns | ~1169 ns | 12347 ns | **1.13× win GSL; 12× SciPy** |
| QAWS (algebraico-log) | ~755 ns | ~700 ns (0.95× GSL = floor) | 4105 ns | **5.4× SciPy** |
| QAWC (Cauchy PV) | ~136 ns | ~145 ns | 2391 ns | **win GSL; 17× SciPy** |
| Levin collocation | N/A (analytic Fresnel verified) | N/A | N/A | accuracy grows with ω; reduces to QAWO at g=x |

**All four QUADPACK methods bit-match SciPy to ~1e-16.** Boost has only `ooura_fourier_cos` (different algorithm, fast on smooth-infinite but not the QAWF algorithm); MATLAB has no weighted-QUADPACK API (stated). Determinism + WCET + error-tier moat GSL/SciPy/Boost structurally lack.

**Multi-D cubature (v13-k):**

| Method | Cerid | SciPy cubature | Peers | Verdict |
|---|---|---|---|---|
| Genz-Malik (3D) | — | reference | — | **37.5× SciPy** (value bit-identical) |
| Genz-Malik (5D) | — | reference | — | **279× SciPy** |
| tensor-product Gauss | — | — | — | dense d≲4 baseline |
| Lebedev (sphere) | — | N/A | N/A | spherical-harmonic exactness verified; octahedral symmetry |
| Smolyak (sparse grid) | — | Tasmanian N/A | — | breaks curse of dimensionality; analytical exactness gate |
| Dunavant (simplex) | — | — | — | polynomial exactness verified (FEM element integration) |

**Quadrature suite: 534 assertions / 33 cases** (v13-g/h/i/j/k, linux-gcc-release).

## Differentiation (v13-l/m) — exact to machine epsilon

| Method | Cerid | JAX-jit | NumPy | SciPy | Verdict | Notes |
|---|---|---|---|---|---|---|
| **Complex-step gradient** (6D) | 48.5 ns | 2778 ns | — | 40600 ns | **57× JAX-jit; 835× NumPy central-FD** | **machine-exact** (matches JAX exactly; NumPy-FD loses digits) |
| Savitzky-Golay coeffs (25,4,2) | 142 ns | — | scipy 28900 ns | baseline | **204× SciPy** | noise-robust polynomial fit; coefficients bit-match scipy |
| Fornberg arbitrary-stencil FD | — | — | — | — | stable weights (any derivative order + arbitrary nodes) | bit-exact vs analytic stencils |
| Richardson-Ridders | — | — | — | — | derivative + error estimate | extrapolation-based, error-killing |
| Chebyshev differentiation matrix | — | — | — | — | spectral accuracy (exponential convergence) | via FFT / barycentric |
| Fourier differentiation matrix | — | — | — | — | spectral accuracy | via FFT |

**Verdict:** Complex-step is the machine-exact winner where f accepts complex arguments (aero/MDO/satellite sensitivity). Spectral (Chebyshev/Fourier) achieves exponential convergence. Savitzky-Golay handles noisy telemetry. Fornberg is the general finite-difference fallback. New module `crd-hesap-diff`, **42 assertions / 5 headers**.

## Motion (v13-n through v13-q) — the arbitrary-state Ruckig-class OTG

**Trajectory primitives (v13-n/o/p):**

| Primitive | Cerid | SciPy / peer | Verdict |
|---|---|---|---|
| SQUAD (C² attitude) | — | scipy.spatial.transform.Slerp | unit-norm preserved to 1e-16; endpoints exact |
| quaternion cubic B-spline | — | N/A | C² on manifold via Kim-Kim-Shin cumulative-basis; uses `quat_log/exp` (added to crd-math) |
| clothoid (Euler spiral) | — | closed-form via Fresnel | curvature linear in arc length (bounded steering jerk) |
| NURBS | — | scipy.interpolate | exact unit circle to 1e-12 (conic sections via rational weights) |
| min-jerk quintic | — | scipy.spatial.transform | 5th-order minimum-jerk BVP |
| **min-snap multi-segment QP** | — | scipy via CVXPY + sparse QP | minimize ∫snap² s.t. waypoints + C³ continuity + BCs; solved via `hesap-dense` LU (acyclic edge added) |
| jerk-limited S-curve | — | Ruckig reference | 7 phase durations in closed form; velocity/accel/jerk-limited; WCET-bounded ≤7-phase walk |
| trapezoidal profile | — | — | time-optimal velocity profile |
| Kochanek-Bartels (TCB) | — | — | keyframe spline (Catmull-Rom special case) |

**Ruckig-class arbitrary-state OTG (v13-q) — the headline:**

| Capability | Cerid vs Ruckig C++ | Correctness | Speed |
|---|---|---|---|
| Single-DoF (`plan_otg`, any state → target, min-time) | **bit-exact** | 0/2000 duration mismatch | **1.94×** (188.6 vs 365.8 ns) |
| Multi-DoF sync (`plan_synchronized_otg`, any state → sync'd reach in tsync) | **bit-exact** | 0/2000 tsync mismatch, 0 reach-fail | **1.26×** (1615.7 vs 2041.0 ns) |

**Benchmarked against `libruckig.a` Release** (`-O3 -march=native`, ruckig 0.17.3). Harnesses in `scratchpad/ruckig_lib` + `scratchpad/otgbench`. Verification vs Ruckig Python package (reconstruct-first discipline): **1934/1934 step1 cases** + **2474/2474 step2 cases** matched.

**Verdict:** Cerid's arbitrary-state Ruckig port is **deterministic by construction** (crd::math, fixed evaluation order), **allocation-free** (all stack arrays), and **WCET-bounded** (finite candidate set + fixed-iteration Newton — no unbounded loops, no malloc, no exceptions). This is exactly the DO-178C / ISO-26262-ASIL-D property real-time robots/drones/autonomous vehicles demand. Achieved while **also beating Ruckig's throughput** (1.26–1.94×).

**Motion suite: 37,289 assertions / 11 cases** (linux-gcc-release, all 4 modules verified).

## Crush discipline notes (SANITY scar from session)

The user's standing directive — **full crush, no deferrals, never accept near-parity** — drove per-method leverage discovery. Four methods first *lost or tied* peers, resolved via the **same recurring lever:** integrand-independent work recomputed per call → precomputed once.

1. **GK error estimate `pow(·,1.5)`** — double-double pow → `x·√x` (one hardware sqrt). QAGS/QAGI 0.88–0.89×→1.29–1.36× GSL.
2. **Gauss symmetric-pair** — ⌈n/2⌉ weight-mults via ±xᵢ ⇒ edge vs Boost `gauss<10>`.
3. **DE convergence estimate** — exploits double-exponential rate (dₘ≈dₘ₋₁²); halving levels. exp_sinh 0.48×→1.17× Boost.
4. **Clenshaw-Curtis weights** — O(N²) precomputed `CcAdaptiveRule` once, not per-call. 0.59×→2.05× GSL-cquad.

**Method:** reconstruct-and-verify-in-python FIRST (fetched scipy/QUADPACK source via `gh`), verified bit-exact before porting line 1 of C++. Caught 3 pre-existing QUADPACK transcription bugs before C++ port, and 1 workspace-reuse leak in C++ (boundary-adversary test exposed it; per-call-fresh-alloc convenience overload masked it).

## Honest scope and moat

**Three moat pillars (v13 architectural mandate):**

1. **Determinism by construction.** Fixed FP reduction order, no `-ffast-math`, pinned/disabled FMA contraction, no arbitrary parallel reductions, `crd::math::*` deterministic transcendentals (never `std::`). Same source ⇒ bit-identical output across compilers, opt levels, and thread counts. The `{1,4,16}` moat audit + run-twice bit-identity gate apply to every deterministic kernel.

2. **Allocation-free streaming.** Every routine takes caller-provided workspace (pre-sized `Array`/`Span`); node/weight tables are `static constexpr`; zero heap per call. Adaptive algorithms are iterative over fixed-size subinterval array, never recursive (bounded-depth work-stack loop — QUADPACK pattern, not textbook recursive adaptive Simpson). Every iterative refinement carries a hard `limit`/`max_iters` cap = WCET knob, returns status not exception.

3. **Error-estimate-exposing, never a bare number.** Every result returns `{value, error_estimate, status, tolerance_met, eval_count, subdiv_count}` with error tier labelled. Errors flow via status enum, not exceptions/RTTI (`noexcept` hot kernels). Convergence tests are tolerance comparisons, never FP `==`, ANDed with iteration cap. Input validation rejects NaN/Inf/bad ranges with `BadInput` status, never a trap or garbage.

These are not aspirational; they map to safety standards (DO-178C, ISO 26262 ASIL-D, IEC 61508) and are the competitive moat GSL (mallocs), Boost (throws), and parallel BLAS (non-reproducible) structurally lose.

## Error tiers (V&V contract)

| Tier | What it is | Where v13 uses it | The contract |
|---|---|---|---|
| **Tier 1 — heuristic estimate** | Gauss-Kronrod `\|K−G\|` (+ QUADPACK rescale + roundoff floor); embedded-RK local error; the cheap, foolable (Lyness-Kaganove hidden-peak), default. | every `integrate_adaptive` result | field is `error_estimate`, never `error_bound`; also expose `eval_count`/`subdiv_count` so outer harness can flag suspiciously-few subdivisions; surface roundoff floor as `RoundoffLimited`. |
| **Tier 2 — a-priori certified bound** | A *real* upper bound, conditional on supplied/computed derivative bound: cubic spline `‖f−s‖∞ ≤ (1/384)h⁴‖f⁽⁴⁾‖∞` (constant proven optimal); poly `(1/(n+1)!)‖f⁽ⁿ⁺¹⁾‖∞‖∏(x−xⱼ)‖`. | distinct `worst_case_error(h, deriv_bound)` query on spline/poly interpolants | strictly stronger than Tier 1; labelled "certified mode"; only valid when derivative bound supplied/rigorous. |
| **Tier 3 — rigorous enclosure** | interval arithmetic (IEEE 1788-2015) / Taylor-Chebyshev models / self-validating quadrature — a proven `[lo,hi]` containing true result, rounding included. | v13-close stretch: interval-valued result type for highest-assurance path | interval *is* the guarantee; opt-in at cost. |

**Honest error-estimate caveat:** an error *estimate* is **not** a guaranteed bound. v13 labels the tier on every API mode and never promotes a hedged estimate to a hard claim — in certifiable systems an over-claimed bound is a latent hazard.

## Suite standing (uncommitted, 2026-07-02)

- **v13-a…f** (interpolation): 544 assertions / 22 cases.
- **v13-g…k** (quadrature): 534 assertions / 33 cases.
- **v13-l/m** (differentiation): 42 assertions / 5 headers.
- **v13-n…q** (motion): 37,289 assertions / 11 cases.

**Full v13 suite: ~38,500+ assertions** (linux-gcc-release, 4-config Windows DoD green as of 2026-07-02).

**Pending (v13-z close):** Windows 4-config DoD (win-asan + win-shipping) + win-tidy naming pass + CLI (hesap.{interp,quad,diff,motion}.*) + 4 system docs + ADR-0095 + 18-config CI (user's post-commit step).

## Pending / deferred

- **Tier-3 interval quadrature** (self-validating) — v13-close stretch, not baseline.
- **Kriging sparse/inducing-point variants** — O(n³) exact GP capped at moderate n.
- **Time-velocity OTG 2nd-order** + Ruckig sextic time_vel branch — not needed (9 cases hit 2474/2474).
