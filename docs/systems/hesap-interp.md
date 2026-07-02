# crd-hesap-interp — interpolation (1-D + scattered/gridded N-D)

> Phase 3.1.6 v13 (a–f). ADR-0095. Plan: `docs/phases/phase-3.1.6-v13.md` (the v13 catalog).
> Status: **SHIPPED (2026-07-01)** — linux-gcc-green + Windows 4-config; suite ~544 assertions. Sessions:
> `2026-06-30-v13-quadrature-engine.md` (cluster kickoff), `2026-07-02-v13z-windows-close.md` (Windows verify + CLI).
> 1-D piecewise/PCHIP · cubic splines (all BCs) · Akima/makima/barycentric/Floater-Hormann · Chebyshev/trig/rational
> · scattered N-D RBF/Shepard/NNI · gridded N-D N-linear/cubic/B-spline/Clough-Tocher/kriging · CLI `hesap.interp.*`.

## What it is

Build-once / evaluate-many interpolation for every Cerid domain — satellite ephemeris resampling, robot lookup
tables, self-driving pose interpolation across asynchronous sensor timestamps, and game keyframe/terrain data. Every
interpolant is an object: the constructor precomputes tangents/coefficients/weights **once** (the init phase, the only
allocation), and `eval(xq)` is `noexcept`, allocation-free, O(1)-amortized via a cached last-segment index (the
real-time hot path). It is held to the v13 **certification bar** (ADR-0095): deterministic by construction,
allocation-free eval, status-not-exception, error-tier-labelled.

## Shipped surface

- **v13-a — 1-D piecewise + the contract.** linear · nearest/previous/next · cubic Hermite (the scalar cousin of the
  ODE `hermite_eval`) · **★PCHIP** (Fritsch-Carlson monotone, *provably no-overshoot* — the certifiable control-LUT
  default). `Interpolator` build-once/eval-many object + `InterpStatus` + `find_segment` (cached O(1) / branchless
  binary search) + the Tier-2 `linear_worst_case_error`.
- **v13-b — cubic splines.** natural / clamped / **★not-a-knot** / periodic, via the Thomas (cyclic-Thomas for
  periodic) tridiagonal solve; C² continuity + the `(1/384)h⁴‖f⁽⁴⁾‖` Tier-2 bound. Replicates
  `scipy.interpolate.CubicSpline(bc_type=…)` exactly.
- **v13-c — local + stable-polynomial.** Akima + **★modified Akima (makima)** (no flat-region overshoot) · **★barycentric
  Lagrange** (2nd form, backward-stable, O(n) eval) · Newton divided differences · **★Floater-Hormann** (barycentric
  rational, pole-free on all real nodes — the equispaced answer). Barycentric-on-Chebyshev beats Runge.
- **v13-d — spectral + rational.** **★Chebyshev interpolation** (DCT-fit via `crd-hesap-fft` + Clenshaw recurrence,
  exponential convergence) · trigonometric/Fourier · rational/Padé (spurious-pole guard).
- **v13-e — scattered N-D.** RBF: multiquadric · inverse-multiquadric · Gaussian · **★thin-plate spline** ·
  polyharmonic · **★Wendland compact-support** — over `crd-hesap-dense` (+ `interp_decomp` conditioning); Shepard/IDW;
  Sibson natural-neighbour (reuses `crd-geometry-delaunay`).
- **v13-f — gridded N-D + uncertainty.** bi/tri-linear (bounded, no-overshoot) · bi/tri-cubic (Keys) · cubic B-spline
  (Unser prefilter, vs `scipy.ndimage`) · Clough-Tocher C¹ (over Delaunay) · **★kriging / Gaussian process** (mean **+
  predictive variance** — the safety-critical sensor-fusion interpolant).

## The error-certification tiers (ADR-0095 §3)

Interpolants expose the tier honestly. **Tier 1** — heuristic (not shipped for interpolation; that's the quadrature
axis). **Tier 2** — an a-priori *certified* bound, conditional on a supplied derivative bound: `linear_worst_case_error`
(`h²/8·max|f″|`) and `cubic_spline_worst_case_error` (`(1/384)h⁴·‖f⁽⁴⁾‖`, the proven-optimal constant). Labelled
"certified mode"; valid only when the derivative bound is rigorous. An estimate is never promoted to a bound.

## Determinism

Pure FMUL/FADD kernels (`crd::math`, never `std::`; no fast-math), fixed per-element evaluation order ⇒ bit-identical
across compilers, opt-levels, and thread counts (the `{1,4,16}` moat; run-twice bit-identity gated per interpolant).
The build phase allocates once; eval allocates nothing and is `noexcept`.

## Crush

Bit-close (≤1e-12) to `scipy.interpolate.{interp1d, PchipInterpolator, CubicSpline, Akima1DInterpolator,
BarycentricInterpolator, RBFInterpolator, RegularGridInterpolator, CloughTocher2DInterpolator}` + `scipy.ndimage`
(Unser B-spline) + `sklearn.GaussianProcessRegressor` (mean + variance), plus the robustness invariants (PCHIP
no-overshoot on monotone data, barycentric-on-Chebyshev suppresses Runge, low-order-polynomial reproduction for RBF).
The native-C++ build-once/eval-many object + reused factorizations is the throughput lever over the per-call peers.

## CLI (`hesap.interp.*`)

Data (samples + query points) cross the boundary as f64 vectors (the callable is never needed for evaluation).

| command | params | output |
|---|---|---|
| `hesap.interp.pchip.f64` | `x`, `y`, `xq` | blob `yq` (monotone, no-overshoot) |
| `hesap.interp.cubic_spline.f64` | `x`, `y`, `xq`, `bc` (natural\|clamped\|notaknot\|periodic), `clamp_left`, `clamp_right` | blob `yq` |

Anchor `register_interp_cli_anchor()` forces the registration TU to survive the static-lib link; `test_cli.cpp` drives
both commands through the registry (interpolation-property + no-overshoot checks).

## Module edges (acyclic)

`crd-hesap-interp` → core/containers/memory · math · **crd-hesap** (CLI registry) · **crd-hesap-dense** (tridiagonal /
LU / lstsq / `interp_decomp`) · **crd-hesap-fft** (DCT for Chebyshev) · **crd-geometry-delaunay** (Sibson NNI). Nothing
it depends on references it.

## Tests

`tests/hesap-interp/` — `test_piecewise` · `test_cubic_spline` · `test_akima_bary` · `test_spectral_rational` ·
`test_rbf` · `test_grid` · `test_kriging` · `test_clough_tocher` · `test_cli`. ~544 assertions, green on win-debug +
the full win-tidy check set.
