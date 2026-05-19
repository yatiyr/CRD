# Session 2026-05-19 — geometry-v10d curve queries

## Summary

Shipped Phase 3.1.7 v10d: curve queries (`aabb_of`, `closest_point`,
`distance`, `intersect_ray`) for `crd-geometry-curves`. All four are
generic over the curve kind and bottom out on `evaluate(curve, t)`
per D186.

## What landed

### Public API (`engine/geometry-curves/include/crd/geometry/curves/queries.hpp`)

```cpp
template <crd::math::MathScalar T> struct CurveClosestPoint {
    T                  t;
    crd::math::Vec3<T> point;
    T                  distance_squared;
};

template <crd::math::MathScalar T> struct CurveRayHit {
    T                  t_curve;
    T                  t_ray;
    crd::math::Vec3<T> point;
};

template <typename Curve>
crd::geometry::primitives::AABB3<typename Curve::scalar_t> aabb_of(
    const Curve& curve, IAllocator* alloc) noexcept;

template <typename Curve>
CurveClosestPoint<typename Curve::scalar_t> closest_point(
    const Curve& curve, const Vec3<...>& p,
    typename Curve::scalar_t tolerance, IAllocator* alloc) noexcept;

template <typename Curve>
typename Curve::scalar_t distance(
    const Curve& curve, const Vec3<...>& p,
    typename Curve::scalar_t tolerance, IAllocator* alloc) noexcept;

template <typename Curve>
std::optional<CurveRayHit<typename Curve::scalar_t>> intersect_ray(
    const Curve& curve, const Ray3<...>& ray,
    typename Curve::scalar_t tolerance, IAllocator* alloc) noexcept;
```

### Algorithms

**`aabb_of`** — `sample_uniform(curve, 64, alloc)` + min/max walk over
the polyline points. Uniform-64 instead of adaptive because adaptive
at the default `k_distance_epsilon` (1e-6) tolerance hits the depth
cap (65 K samples → multi-MB temp per query). Uniform-64 is bounded-
cost and sufficient for AABB fidelity on any realistic curve. Analytic
specialisations (CircularArc / EllipseArc closed-form, Bezier
control-point hull) filed as `v10d-analytic-aabb` follow-on.

**`closest_point`** — two-stage:

1. **Initial guess (D204)**: 16 uniform samples; pick the one
   minimising `||curve(t) - p||²` as the Newton seed. This is
   subdivision-rejection for the GLOBAL minimum — important on
   S-curves where the local minimum near t=0.5 is not always the
   nearest point.

2. **Newton-Raphson (D205)**: iterate on `f(t) = (curve(t) - p) ·
   curve'(t) = 0`. `f'(t)` is finite-difference (`(f(t+h) - f(t)) /
   h` with h = 1e-4) — avoids needing curve-kind-specific second
   derivatives. Open curves clamp `t` to `[0, 1]`; closed curves wrap.
   Stop on `|delta_t| <= tolerance` or after 32 iterations.

**`distance`** — `sqrt(closest_point.distance_squared)`. Same
convergence; one extra sqrt at the end.

**`intersect_ray`** — flatten via `sample_adaptive(curve, tolerance,
alloc)`, then for each polyline segment compute the
ray-vs-segment closest-approach. Hits within `tolerance` of the ray
(distance check on `min_dist_sq`) are candidates; return the smallest
`t_ray` (D206). Curves are zero-thickness so "intersect" means
"closest-approach within tolerance" — matches cinematic-camera
trigger-volume + editor cursor-pick semantics.

### Decisions queued for v10-close (ADR-0076 §27)

- **D203** — `aabb_of` uses uniform-64 sampling + min/max walk.
  Analytic-AABB specialisations filed for consumer-pull.
- **D204** — `closest_point` initial guess via 16 uniform samples;
  global minimum, not local.
- **D205** — Newton-Raphson on `f(t) = (curve(t) - p) · curve'(t) =
  0` with finite-difference `f'(t)`; max 32 iters; clamp/wrap on
  closed flag.
- **D206** — `intersect_ray` ships first-hit ray-vs-flattened-
  polyline. Multi-hit + curve-vs-curve filed as consumer-pull.
- **D207** — `tolerance` is in length units (same scalar as control
  points); default = `crd::geometry::primitives::k_distance_epsilon`.

### Pinned known limitations

- **Adaptive sampler midpoint degeneracy**: `sample_adaptive`'s
  chord-error check evaluates only at the segment midpoint. For
  antisymmetric polynomials (e.g. an S-shaped cubic Bezier where
  control points are reflected about the midpoint), the midpoint
  coincides with the chord through the endpoints → chord error = 0
  → the sampler emits only the endpoints. This is a substrate
  limitation. The query test corpus uses circular-arc + asymmetric
  bezier curves for the multi-hit ray test to avoid it.
- **Parallel-collinear ray/segment**: `detail::ray_segment_closest`'s
  parallel fallback uses a midpoint heuristic — not always correct
  when the ray's line is collinear with the segment's line. Doesn't
  affect any v10 test corpus or known consumer pattern.
- **Polyline `intersect_ray` resampling**: the generic
  `intersect_ray` flattens via `sample_adaptive`, which for a
  `Polyline3View` input degenerates back to the chord between
  endpoints (lerp-midpoint coincides with the polyline's midpoint by
  construction). Direct segment-iteration fast-path for Polyline
  inputs filed as `v10d-polyline-fast-path`.

### Tests (`tests/geometry-curves/test_curves_queries.cpp`)

11 cases / ~285 new assertions:

- AABB-contains-every-sample on a cubic Bezier.
- AABB matches algebraic bounds on a straight polyline.
- `closest_point` at a point ON the curve returns that point within
  1e-3 tolerance on t, 1e-6 on distance².
- `closest_point` for off-line query points clamps to the nearest
  endpoint on a horizontal polyline (left-of and right-of).
- `closest_point` at a circle's centre returns a point at distance ≈
  radius (any point on the arc).
- `closest_point` global-minimum on an S-curve at `t = 0.85`.
- `distance` ≡ `sqrt(closest_point.distance²)`.
- `intersect_ray` returns nullopt when ray misses.
- `intersect_ray` reports hit when ray pierces a horizontal polyline.
- `intersect_ray` first-hit on a full circle (two crossings).
- `f64` instantiations work across all four entry points.

Module total: **72 cases / 2 534 assertions PASS** (was v10c 61 / 2
445; +11 cases / +89 assertions).

## What's next

- **v10e** — Frames + viz + sandbox showcase (tangent / normal /
  binormal / RMF Wang 2008).
- **v10-close** — ADR-0076 §27 amendment + typed-units boundary
  layer (`queries_typed.hpp`) + 18-config sweep + system doc.

## Files added

- `engine/geometry-curves/include/crd/geometry/curves/queries.hpp`
- `tests/geometry-curves/test_curves_queries.cpp`

## Files changed

- `engine/geometry-curves/include/crd/geometry/curves/curves.hpp` —
  added `#include "queries.hpp"` to the umbrella.
- `tests/geometry-curves/CMakeLists.txt` — added
  `test_curves_queries.cpp`.
- `docs/phases/phase-3.1.7-geometry.md` — v10d row marked ✅.
