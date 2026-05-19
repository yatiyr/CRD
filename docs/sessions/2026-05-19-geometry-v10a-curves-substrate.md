# Session 2026-05-19 — geometry-v10a curves substrate

## Summary

Shipped Phase 3.1.7 v10a: the `crd-geometry-curves` module substrate.
12th `crd-geometry` extension — the curve / spline foundation that v10b
(sampling), v10c (arc-length), v10d (queries), v10e (frames + viz) will
all build on.

This is the v10 cluster opener — the substrate-only slice that fixes the
type catalogue + evaluator algorithm definition. Per D186 (the v10
analog of v9e D170), `evaluate(curve, t)` IS the curve. Every downstream
query in v10b-e MUST eventually call it or `evaluate_derivative`.

## What landed

### Module

`engine/geometry-curves/` — new sibling under the `crd-geometry`
umbrella:

```
engine/geometry-curves/
  CMakeLists.txt
  include/crd/geometry/curves/
    types.hpp           — Kind enums (CurveKind3, CurveKind2, CatmullRomParam)
    polyline.hpp        — Polyline3<T>/2<T> + non-owning views (D184)
    bezier.hpp          — QuadBezier3/CubicBezier3 + 2D peers
    hermite.hpp         — CubicHermite3<T>
    catmull_rom.hpp     — CatmullRom3<T> (Uniform + Centripetal)
    bspline.hpp         — BSpline3<T> (degree-3) + make_uniform_open factory
    arc.hpp             — CircularArc3/EllipseArc3 + 2D peers
    evaluator.hpp       — evaluate + evaluate_derivative + detail helpers
    validate.hpp        — per-kind validator declarations
    curves.hpp          — umbrella
  src/
    validate.cpp        — per-kind validators + f32/f64 explicit instantiations
    curves.cpp          — force-link anchor
```

### Curve type catalogue (D183 — fixed at v10a)

3D — 8 kinds:

| Kind | Storage | Notes |
|---|---|---|
| `Polyline3<T>` | `Array<Vec3<T>>` + `bool closed` | Owning + view variants |
| `QuadBezier3<T>` | 3 control points | de Casteljau evaluator |
| `CubicBezier3<T>` | 4 control points | de Casteljau evaluator |
| `CubicHermite3<T>` | 2 positions + 2 tangents | Hermite basis evaluator |
| `CatmullRom3<T>` | `Array<Vec3<T>>` + `CatmullRomParam` + `bool closed` | Uniform / Centripetal |
| `BSpline3<T>` | `Array<Vec3<T>>` + `Array<T>` knots | Degree 3, Cox-de-Boor |
| `CircularArc3<T>` | center + axis_u/v + radius + sweep | Deterministic sin/cos |
| `EllipseArc3<T>` | center + axes + radius_u/v + sweep | Deterministic sin/cos |

2D peers — 5 kinds: `Polyline2`, `QuadBezier2`, `CubicBezier2`,
`CircularArc2`, `EllipseArc2`. `CubicHermite2` + 2D Catmull-Rom + 2D
B-spline filed as `v10a-2d-{hermite,catmull,bspline}` follow-ons (ship
when an authoring consumer asks — straight `Vec2`-substitution of the
3D code, no algorithm change).

### Public API

```cpp
// Evaluator — D186 algorithm definition
template <crd::math::MathScalar T>
[[nodiscard]] crd::math::Vec3<T> evaluate(const Curve& c, T t) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] crd::math::Vec3<T> evaluate_derivative(const Curve& c, T t) noexcept;

// Per-kind validators — D191
[[nodiscard]] CurveValidationResult validate(const Curve& c) noexcept;
```

`CurveValidationResult{status, offending_index}` — 12 status codes:
NotEnoughPoints / NonFinitePoint / NonFiniteTangent / AdjacentColocated
/ KnotCountMismatch / KnotNonMonotonic / KnotMultiplicityExceeded /
AxisNotUnit / AxesNotOrthogonal / InvalidRadius / SweepOutOfRange / Ok.

## Decisions to pin at v10-close (ADR-0076 §27)

11 decisions queued:

- **D182** — `crd-geometry-curves` 12th extension; library-only.
- **D183** — Type set fixed at v10a-close. Additive growth only.
- **D184** — Polyline gets owning + view split. Other curves single-form.
- **D185** — Templated on `T ∈ {f32, f64}` from day 1.
- **D186** — `evaluate(curve, t)` IS the algorithm definition.
- **D187** — Control points typed `Vec3<Length<T>>` at API surface;
  raw `Vec3<T>` internally per ADR-0078 §5 D34 (deferred to v10-close
  when a typed-units consumer wires; v10a ships raw).
- **D188** — Closed = per-instance `bool closed`, not a separate type.
- **D189** — B-spline knots are owned `Array<T>` inside `BSpline3<T>`.
  Cubic only (degree 3) at v10a; arbitrary-degree filed as follow-on.
- **D190** — CatmullRom3 supports both Uniform + Centripetal via
  construction-time enum. Centripetal = cinematic-camera default.
- **D191** — Validation via free function `validate(curve)`. Cooker-
  friendly, returns first-offending-index.
- **D192** — `evaluate_derivative` is analytic for Bezier / Hermite /
  Arc (bit-exact contracts) and finite-difference for CatmullRom /
  BSpline. Analytic forms for the FD-using kinds are filed as
  consumer-pull follow-ons (`v10a-cr-analytic-derivative` +
  `v10a-bspline-analytic-derivative`) — finite-difference is
  numerically correct + composable; analytic is a perf optimisation.

## Tests

`tests/geometry-curves/` — 3 files, **37 cases / 135 assertions PASS**:

- `test_curves_types.cpp` (9 cases) — constructibility + view/owning
  round-trip + `make_uniform_open` knot vector + enum stability.
- `test_curves_validate.cpp` (~11 cases) — Ok paths + every failure
  status code triggers on the matching malformed input.
- `test_curves_evaluator.cpp` (~17 cases) — the substantive corpus:

  - **Boundary equality** — every curve's `evaluate(curve, 0)` is
    bit-equal to its algebraic start, `evaluate(curve, 1)` is the
    end. (For Polyline, CubicBezier, QuadBezier, CubicHermite.)
  - **Bezier algebra** — de Casteljau midpoint matches Bernstein
    expansion `(P0 + 3P1 + 3P2 + P3) / 8` within 4 ULP at t=0.5.
  - **CubicBezier3 derivative** — `evaluate_derivative(c, 0) ==
    3*(P1-P0)` bit-exactly (analytic-derivative contract).
  - **Cross-kind equivalence** — `CubicHermite(P0, 3(P1-P0), P3,
    3(P3-P2)) ≡ CubicBezier(P0, P1, P2, P3)` within 4 ULP at 11 sample
    points. Proves the two evaluators agree to rounding error on
    different polynomial bases.
  - **Catmull-Rom interpolation** — passes through every control
    point at the segment joins (both Uniform and Centripetal).
  - **Closed-curve wrap** — `evaluate(curve, 1.0)` bit-equal to
    `evaluate(curve, 0.0)` for closed Polyline, closed CatmullRom,
    closed CircularArc.
  - **B-spline clamped endpoints** — uniform-open spline hits
    `points[0]` at t=0 and `points[n-1]` at t=1.
  - **B-spline smoothness probe** — 20-step bounded-chord check
    catches kind-routing bugs.
  - **CircularArc algebra** — `evaluate(arc, 0) = center + radius *
    axis_u`; half-revolution lands at `center - radius * axis_u`
    within 1e-5 abs (3 ULP Cephes-poly cost).
  - **CircularArc derivative direction** — `evaluate_derivative(arc,
    0)` is along `axis_v` with magnitude `sweep_radians * radius`.
  - **f64 instantiations** — full algebra works for `double`.

## Numerical-stability discipline

- **de Casteljau** for Bezier (not Bernstein polynomial expansion).
- **Hermite basis** functions h00 / h10 / h01 / h11.
- **Barry-Goldman nested-lerp** for Catmull-Rom (no matrix solve, no
  transcendentals on the hot path).
- **Cox-de-Boor** for B-spline (Piegl & Tiller A2.2, iterative form
  specialised to degree 3).
- **`crd::math::deterministic::sin/cos`** for arcs — Cephes polynomial
  approximation, ~3 ULP across compilers, matches the GLSL/HLSL
  preludes shipped at v9e-b/c.
- **No `std::sort`** anywhere — knot-vector ordering is validated
  (D191), not corrected.
- **`std::sqrt`** is permitted (IEEE 754 correctly-rounded —
  deterministic per ADR-0063 build flags).

## Filed follow-ons (consumer-pull, not blocking)

- `v10a-cr-analytic-derivative` — analytic Catmull-Rom derivative
  (Barry-Goldman differentiation). Ships when sampling perf pulls on it.
- `v10a-bspline-analytic-derivative` — derivative as a degree-2 B-spline
  over a shifted control polygon (Piegl & Tiller A3.4).
- `v10a-bspline-arbitrary-degree` — degrees other than 3 (degree 5 for
  cinematic surfaces, degree 1 = polyline). Ships when consumer asks.
- `v10a-2d-hermite` / `v10a-2d-catmull` / `v10a-2d-bspline` — 2D peers
  for the 3D-only kinds. Straight `Vec2` substitution.
- `v10a-typed-units` (D187 finalisation) — `Vec3<Length<T>>` API-surface
  wrappers when a typed-units consumer wires. v10a stays raw; the
  layer ships when a renderer / animation system pulls.
- `MultiCubicBezier3` — segmented Bezier for closed-loop authoring.

## What's next

- **v10b** — Sampling + flattening. `sample_uniform / sample_adaptive /
  sample_by_curvature / to_polyline`. Tolerance ties to
  `k_distance_epsilon` from v1h constants.
- **v10c** — Arc-length system.
- **v10d** — Queries (AABB / closest-point / ray intersection).
- **v10e** — Frames + viz + sandbox showcase.

## Files added

- `engine/geometry-curves/CMakeLists.txt`
- `engine/geometry-curves/include/crd/geometry/curves/*.hpp` (10 headers)
- `engine/geometry-curves/src/validate.cpp` + `curves.cpp`
- `tests/geometry-curves/CMakeLists.txt`
- `tests/geometry-curves/test_*.cpp` (3 files)

## Files changed

- `CMakeLists.txt` (root) — added `add_subdirectory(engine/geometry-curves)`.
- `tests/CMakeLists.txt` — added `add_subdirectory(geometry-curves)`.
- `docs/phases/phase-3.1.7-geometry.md` — v10a row marked ✅.
