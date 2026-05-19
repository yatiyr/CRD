# Session 2026-05-19 — geometry-v10b sampling + flattening

## Summary

Shipped Phase 3.1.7 v10b: sampling + flattening over the `crd-geometry-
curves` substrate. Three sample modes + a convenience wrapper, every
one generic over the curve kind. Every sampler calls `evaluate(curve, t)`
internally — D186 algorithm-definition contract enforced by construction.

## What landed

### Public API (`engine/geometry-curves/include/crd/geometry/curves/sample.hpp`)

```cpp
namespace crd::geometry::curves {

// Parameter-uniform. Open emits n+1 points (t in [0,1] inclusive);
// closed emits n points (t in [0,1)) + Polyline3 closed = true.
template <typename Curve>
[[nodiscard]] Polyline3<typename Curve::scalar_t> sample_uniform(
    const Curve& curve, crd::u32 n_segments, IAllocator* alloc) noexcept;

// Explicit-stack chord-error subdivision (D193). Depth cap = 16 (D196).
template <typename Curve>
[[nodiscard]] Polyline3<typename Curve::scalar_t> sample_adaptive(
    const Curve& curve, typename Curve::scalar_t tolerance,
    IAllocator* alloc) noexcept;

// Angle-uniform via `dot(unit_t0, unit_t1) >= det::cos(max_angle_step)`
// (D194). No acos; CPU<->GPU bit-portable.
template <typename Curve>
[[nodiscard]] Polyline3<typename Curve::scalar_t> sample_by_curvature(
    const Curve& curve, typename Curve::scalar_t max_angle_step,
    IAllocator* alloc) noexcept;

// Convenience: sample_adaptive with default tolerance.
template <typename Curve>
[[nodiscard]] Polyline3<typename Curve::scalar_t> to_polyline(
    const Curve& curve, IAllocator* alloc) noexcept;

inline constexpr crd::u32 k_sample_max_subdivision_depth = 16U;

template <crd::math::MathScalar T>
[[nodiscard]] constexpr T sample_default_tolerance() noexcept;
}
```

### Generic-over-curve-kind dispatch

Every curve type gained `using scalar_t = T;` so the sampler templates
can deduce T cleanly:

- `Polyline3<T>` + `Polyline3View<T>` + `Polyline2<T>` + `Polyline2View<T>`
- `QuadBezier3<T>` + `CubicBezier3<T>` + 2D peers
- `CubicHermite3<T>`
- `CatmullRom3<T>`, `BSpline3<T>`
- `CircularArc3<T>` + `EllipseArc3<T>` + 2D peers

Bezier + Hermite also gained `static constexpr bool closed = false;` so
the generic sampler's `curve.closed` access works uniformly across ALL
curve kinds. Bezier closure (closed-loop authoring) requires
`MultiCubicBezier3` segmented form — filed for v10-followon.

### Subdivision driver (D193)

`sample_adaptive` and `sample_by_curvature` share
`detail::subdivide_drive(curve, is_leaf, alloc)`. The driver:

1. Pushes `{t0=0, t1=1, depth=0}` onto an explicit `Array` stack.
2. Emits `evaluate(curve, 0)` as the first point.
3. While stack non-empty: pop top, evaluate p0/p1/midpoint, call
   `is_leaf(...)`. If leaf or depth-cap-hit: emit `p1`. Else: push
   `{tm, t1, d+1}` THEN `{t0, tm, d+1}` (right half FIRST → left half
   popped first → monotonic-t emission order without sorting).
4. For closed curves: drop the duplicate t=1 sample, set `closed=true`.

Predicates:
- **Adaptive**: `length_squared(p_midpoint - (p0+p1)/2) <= tolerance²`.
- **Curvature**: `dot(d0, d1) / (|d0| · |d1|) >= cos(max_angle_step)`.

### Tests (`tests/geometry-curves/test_curves_sample.cpp`)

11 cases / ~2 114 new assertions:

| Test | What it verifies |
|---|---|
| `sample_uniform open` | n+1 points; boundaries bit-equal to `evaluate(curve, 0/1)`; `closed=false`. |
| `sample_uniform closed` | n points; `closed=true`; first point matches evaluator. |
| `sample_uniform determinism` | Two runs byte-identical sample-by-sample. |
| `sample_adaptive boundaries` | First + last sample bit-equal to evaluator at t=0/1. |
| `sample_adaptive convergence` | Tighter tolerance → more samples (monotone). |
| `sample_adaptive bound` | Monotonic-on-tolerance + depth-cap finite-output. |
| `sample_by_curvature` | Boundaries + monotonic-on-angle. |
| `to_polyline` | Identical output to `sample_adaptive(default_tol)`. |
| `cross-kind smoke` (~7 inner sub-tests) | Every curve kind (Polyline / Quad/Cubic Bezier / Hermite / CatmullRom / BSpline / Circular/EllipseArc) samples without compile or runtime error. |
| `f64 instantiations` | Both `double` evaluator paths run + produce expected boundaries. |

Module total: **48 cases / 2 249 assertions PASS** (was v10a 37 / 135;
+11 cases / +2 114 assertions).

## Decisions queued for v10-close (ADR-0076 §27)

- **D193** — Adaptive sampler uses **explicit-stack subdivision**, not
  recursion. Right-half-first push order gives monotonic-t emission
  without sort. Bounded stack depth.
- **D194** — Curvature sampler uses
  `crd::math::deterministic::cos(max_angle_step)` as the dot-product
  threshold. No `acos`. Cos comparison is monotonic on `[0, π]` and
  CPU↔GPU bit-portable.
- **D195** — Closed-curve output convention: emit `n` points covering
  `t ∈ [0, 1)`, set `Polyline3::closed = true`. The implicit wrap is
  consumer's responsibility (matches `Polyline3` v10a storage).
- **D196** — Subdivision depth cap = 16. Cap-hit silently emits result-
  so-far. Output stays numerically correct at every emitted t; the
  curve is just under-resolved in pathological subregions. Never
  asserts.
- **D197** — `to_polyline` default tolerance =
  `crd::geometry::primitives::k_distance_epsilon<T>()` (1e-6 for f32,
  1e-12 for f64).

## Scope discipline + filed follow-ons

**v10b ships 3D only.** 2D-peer samplers (`Polyline2`, `QuadBezier2`,
etc.) ship when an authoring consumer asks — straight `Vec2`-substitution
of the same code path. Filed as `v10b-2d-sampling`.

Adaptive **analytic chord-error bound** at every t (not just per-segment
midpoint) — stronger contract than what we ship. Would require knowing
each emitted sample's t-value. Filed as `v10b-stronger-chord-bound` if
a consumer needs the tighter guarantee.

## What's next

- **v10c** — Arc-length system: `build_arclength_table`, `length_of`,
  `t_at_distance`, `distance_at_t`.
- **v10d** — Queries: AABB / closest-point Newton / ray intersection.
- **v10e** — Frames + viz + sandbox showcase.

## Files added

- `engine/geometry-curves/include/crd/geometry/curves/sample.hpp`
- `tests/geometry-curves/test_curves_sample.cpp`

## Files changed

- 5 curve-type headers (`polyline.hpp`, `bezier.hpp`, `hermite.hpp`,
  `catmull_rom.hpp`, `bspline.hpp`, `arc.hpp`) — added `using scalar_t =
  T;` to all 15 curve types; Bezier + Hermite also gained
  `static constexpr bool closed = false;`.
- `engine/geometry-curves/include/crd/geometry/curves/curves.hpp` —
  added `#include "sample.hpp"` to the umbrella.
- `tests/geometry-curves/CMakeLists.txt` — added
  `test_curves_sample.cpp`.
- `docs/phases/phase-3.1.7-geometry.md` — v10b row marked ✅.
