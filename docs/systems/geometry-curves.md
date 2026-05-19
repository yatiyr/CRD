# crd-geometry-curves

Parametric curves substrate: Polyline + Bezier + Hermite + Catmull-Rom +
B-spline + circular / elliptic arcs, plus sampling, arc-length, queries
(closest-point / AABB / ray-intersect), Frenet + rotation-minimising
frames (Wang 2008), debug visualisation companion, and a Quantity-aware
typed boundary covering the whole surface. The 12th `crd-geometry-*`
sibling — closes the substrate side of `crd-geometry`.

> Module path: `engine/geometry-curves/`
> Target: `crd-geometry-curves`
> Namespace: `crd::geometry::curves`
> Opened: Phase 3.1.7 v10a (2026-05-19)
> Status: ✅ **CLUSTER CLOSED 2026-05-19** — v10a substrate ✅ + v10b
> sampling ✅ + v10c arc-length ✅ + v10d queries ✅ + v10e frames + viz
> + sandbox ✅ + **v10-close ADR-0076 §27 ✅ Accepted (D182-D216)**.
> Phase 3.1.7 sub-module 12 of 11.

## Public surface

| Header | Purpose |
|---|---|
| `crd/geometry/curves/types.hpp`        | `CurveKind3` / `CurveKind2` / `CatmullRomParam` enums (ABI-stable, append-only) |
| `crd/geometry/curves/polyline.hpp`     | `Polyline3` (owning) + `Polyline3View` (non-owning) + 2D peers |
| `crd/geometry/curves/bezier.hpp`       | `QuadBezier3` / `CubicBezier3` + 2D peers |
| `crd/geometry/curves/hermite.hpp`      | `CubicHermite3` (cubic Hermite basis) |
| `crd/geometry/curves/catmull_rom.hpp`  | `CatmullRom3` (Uniform + Centripetal — Yuksel 2011) |
| `crd/geometry/curves/bspline.hpp`      | `BSpline3` (degree 3, Cox-de-Boor, clamped-open factory) |
| `crd/geometry/curves/arc.hpp`          | `CircularArc3` + `EllipseArc3` (deterministic `det::sin/cos`) + 2D peers |
| `crd/geometry/curves/evaluator.hpp`    | `evaluate(curve, t)` + `evaluate_derivative(curve, t)` (D186: algorithm definition) |
| `crd/geometry/curves/validate.hpp`     | `validate(curve) -> CurveValidationResult` — 12 status codes (D191) |
| `crd/geometry/curves/sample.hpp`       | `sample_uniform` / `sample_adaptive` / `sample_by_curvature` / `to_polyline` |
| `crd/geometry/curves/arclength.hpp`    | `ArclengthTable<T>` + `build_arclength_table` + `length_of` + `t_at_distance` + `distance_at_t` |
| `crd/geometry/curves/queries.hpp`      | `aabb_of` + `closest_point` + `distance` + `intersect_ray` |
| `crd/geometry/curves/frames.hpp`       | `tangent` + `normal` (Frenet) + `binormal` + `compute_rmf` (Wang 2008 + closure twist) |
| `crd/geometry/curves/queries_typed.hpp`| Quantity-aware `*_typed` wrappers covering the WHOLE surface (D214-D216) |
| `crd/geometry/curves/curves.hpp`       | Umbrella include |

Debug-visualisation adapters ship in **`crd-geometry-viz`**
(`crd/geometry/viz/curves.hpp` — `draw_curve` / `draw_polyline` /
`draw_tangent_frame` / `draw_rmf`). `crd-geometry-curves` itself does
NOT link `crd-draw`; a headless / cooker / DAW build consumes curves
without pulling the GPU debug-draw layer.

## The pipeline

```
   designer / cooker / runtime ─┐
                                │
                  Curve type (8 kinds × 3D + 5 kinds × 2D)
                                │
                                ▼
                    ┌──────────────────────────┐
                    │   evaluate(curve, t)     │  ← D186 algorithm definition
                    │   evaluate_derivative()  │
                    └──┬───────────────────────┘
                       │
        ┌──────────────┼──────────────┬────────────────┬────────────┐
        ▼              ▼              ▼                ▼            ▼
   sample_*       arclength       queries          frames       (future
   (v10b)         (v10c)          (v10d)           (v10e)        consumer)
        │              │              │                │
        │              │              │                │
        └──────────────┼──────────────┴────────────────┘
                       │
                       ▼
              Typed boundary layer
              (`queries_typed.hpp`,
              `*_typed` suffix per D214-D216)
                       │
                       ▼
          Vec3<Length<T>>, Quantity<Length, T>,
          ArclengthTable<T> (raw per D215),
          raw CurveFrame<T> (per D216)
```

Cinematic / robotics consumers (camera dolly, motion path, trajectory
planner) consume the typed boundary; SIMD inner loops in samplers /
arc-length / queries stay raw `f32` / `f64` (ADR-0078 §5).

## v10a substrate — types + evaluator + validators

```cpp
namespace crd::geometry::curves {

template <crd::math::MathValue T> struct Polyline3View {
    using scalar_t = T;
    ConstSpan<Vec3<T>> points;
    bool               closed = false;
};

template <crd::math::MathValue T> struct CubicBezier3 {
    using scalar_t = T;
    static constexpr bool closed = false;
    Vec3<T> p0, p1, p2, p3;
};

template <crd::math::MathValue T> struct CatmullRom3 {
    using scalar_t = T;
    Array<Vec3<T>>  points;
    CatmullRomParam param  = CatmullRomParam::Centripetal;
    bool            closed = false;
};

template <crd::math::MathValue T> struct BSpline3 {
    using scalar_t = T;
    static constexpr u32 k_degree = 3U;
    Array<Vec3<T>> points;
    Array<T>       knots;
    bool           closed = false;
    static BSpline3<T> make_uniform_open(IAllocator*, ConstSpan<Vec3<T>>);
};

template <crd::math::MathValue T> struct CircularArc3 {
    using scalar_t = T;
    Vec3<T> center, axis_u, axis_v;
    T       radius, sweep_radians;
    bool    closed = false;
};

// + QuadBezier3 / CubicHermite3 / EllipseArc3 + Polyline3 (owning) +
// 2D peers (Polyline2 / QuadBezier2 / CubicBezier2 / CircularArc2 /
// EllipseArc2).

template <typename Curve>
Vec3<typename Curve::scalar_t> evaluate(const Curve& curve,
                                         typename Curve::scalar_t t) noexcept;

template <typename Curve>
Vec3<typename Curve::scalar_t> evaluate_derivative(const Curve& curve,
                                                    typename Curve::scalar_t t) noexcept;

template <typename Curve>
CurveValidationResult validate(const Curve& curve) noexcept;

} // namespace crd::geometry::curves
```

Stable-canonical forms (D192): **de Casteljau** for Bezier (numerically
stable repeated linear interpolation), **Hermite basis** for
`CubicHermite3`, **Barry-Goldman nested lerp** for Catmull-Rom,
**Cox-de-Boor recursion** for B-spline, **`det::sin/cos`** (Cephes-poly
port) for arcs.

## v10b sampling

```cpp
template <typename Curve>
Polyline3<typename Curve::scalar_t> sample_uniform(
    const Curve& curve, u32 n_segments, IAllocator* alloc) noexcept;

template <typename Curve>
Polyline3<typename Curve::scalar_t> sample_adaptive(
    const Curve& curve, typename Curve::scalar_t tolerance, IAllocator* alloc) noexcept;

template <typename Curve>
Polyline3<typename Curve::scalar_t> sample_by_curvature(
    const Curve& curve, typename Curve::scalar_t max_angle_step, IAllocator* alloc) noexcept;

template <typename Curve>
Polyline3<typename Curve::scalar_t> to_polyline(
    const Curve& curve, IAllocator* alloc) noexcept;
```

D193: adaptive subdivision via **explicit stack** with right-push-first
order → monotonic-t output. D194: curvature step uses
`dot(unit_T0, unit_T1) >= det::cos(max_angle_step)` (no `acos`; CPU↔GPU
portable). D195: closed-curve emits `n_samples` points covering
`t ∈ [0, 1)`; open emits `n_samples + 1` covering `[0, 1]`. D196: depth
cap = 16 (max 65 537 leaves); cap-hit is a soft log + emit-so-far.

## v10c arc-length

```cpp
template <crd::math::MathScalar T> struct ArclengthTable {
    Array<ArclengthSample<T>> samples;  // {t, distance}
    T                         total_length = 0;
    bool                      closed       = false;
};

template <typename Curve>
ArclengthTable<typename Curve::scalar_t> build_arclength_table(
    const Curve& curve, u32 n_samples, IAllocator* alloc) noexcept;

template <crd::math::MathScalar T>
T length_of(const ArclengthTable<T>& table) noexcept;

template <crd::math::MathScalar T>
T t_at_distance(const ArclengthTable<T>& table, T distance) noexcept;

template <crd::math::MathScalar T>
T distance_at_t(const ArclengthTable<T>& table, T t) noexcept;
```

D198: **uniform-t chord-length** sampling (NOT analytic) — consumers
tune accuracy via `n_samples`. D199: default `n = 64`. D200: open +
closed share the same table layout (`n + 1` entries, t = 0..1
inclusive). D201: lookup = binary search + linear interp (higher-order
interp doesn't help; the table is piecewise-linear by construction).
D202: closed curves use floor-based modular reduction on out-of-range
inputs.

## v10d queries

```cpp
template <typename Curve>
AABB3<typename Curve::scalar_t> aabb_of(
    const Curve& curve, IAllocator* alloc) noexcept;

template <crd::math::MathScalar T>
struct CurveClosestPoint { T t; Vec3<T> point; T distance_squared; };

template <typename Curve>
CurveClosestPoint<typename Curve::scalar_t> closest_point(
    const Curve& curve, const Vec3<typename Curve::scalar_t>& p,
    typename Curve::scalar_t tolerance, IAllocator* alloc) noexcept;

template <typename Curve>
typename Curve::scalar_t distance(
    const Curve& curve, const Vec3<typename Curve::scalar_t>& p,
    typename Curve::scalar_t tolerance, IAllocator* alloc) noexcept;

template <crd::math::MathScalar T>
struct CurveRayHit { T t_curve; T t_ray; Vec3<T> point; };

template <typename Curve>
std::optional<CurveRayHit<typename Curve::scalar_t>> intersect_ray(
    const Curve& curve, const Ray3<typename Curve::scalar_t>& ray,
    typename Curve::scalar_t tolerance, IAllocator* alloc) noexcept;
```

D203: `aabb_of` = uniform-64-sample flattening + min/max walk
(bounded-cost; analytic specialisations filed as `v10d-analytic-aabb`
follow-on). D204: `closest_point` initial guess via 16 uniform samples
(global, not local). D205: Newton-Raphson on
`(curve(t) - p) · curve'(t) = 0` with finite-diff `f'(t)`; max 32 iters;
open clamps, closed wraps. D206: `intersect_ray` returns first hit
(smallest `t_ray`) within `tolerance` of any flattened-polyline segment.

## v10e frames + RMF + viz + sandbox

```cpp
template <crd::math::MathScalar T>
struct CurveFrame { Vec3<T> tangent, normal, binormal; };

template <typename Curve>
Vec3<typename Curve::scalar_t> tangent(const Curve& curve,
                                        typename Curve::scalar_t t) noexcept;

template <typename Curve>
Vec3<typename Curve::scalar_t> normal(const Curve& curve,
                                       typename Curve::scalar_t t) noexcept;

template <typename Curve>
Vec3<typename Curve::scalar_t> binormal(const Curve& curve,
                                         typename Curve::scalar_t t) noexcept;

template <typename Curve>
Array<CurveFrame<typename Curve::scalar_t>> compute_rmf(
    const Curve& curve, u32 n_samples, IAllocator* alloc) noexcept;
```

D208: frames compose on `evaluate` + `evaluate_derivative` only — adding
a new curve kind gets frames free. D209: RMF = **parameter-uniform**
(arc-length variant filed as `v10e-arclength-rmf` follow-on for the
truly-smooth cinematic-camera path). D210: closed-curve closure-twist
via **uniform redistribution** — rotate each frame `k` by
`-theta_total * (k / n)` around its own tangent after the Wang walk.
D211: zero-curvature fallback `detail::frenet_fallback_normal` = +Y
projected onto tangent's perpendicular plane, +Z fallback if parallel
to +Y. D212: degenerate-tangent returns +X (scalar) / last-good (RMF).
D213: 2nd-derivative finite-difference step `h_2nd = 1e-3` (safe for
both analytic and finite-difference 1st-derivative kinds).

Sandbox: **`SandboxScene::CurvesShowcase = 3`** ships in
`sandbox/src/curves_showcase.{hpp,cpp}` (9 curve kinds incl. a
cinematic-camera Helix demo backed by Catmull-Rom; per-kind ImGui
`DragFloat3` control-point editor; frame-mode toggle Off / Frenet / RMF;
sample slider 4..256). 3D-viewport drag-gizmos are filed for the future
direct-manipulation UX cluster (see `docs/debt.md`).

`crd-geometry-viz` companion:

```cpp
namespace crd::geometry::viz {
void draw_polyline(RenderBuffer&, const Polyline3View<f32>&, ...);
void draw_polyline(RenderBuffer&, const Polyline3View<f64>&, ...);

template <typename Curve>
void draw_curve(RenderBuffer&, const Curve&, u32 n_segments, IAllocator*, ...);

template <typename Curve>
void draw_tangent_frame(RenderBuffer&, const Curve&, u32 n_samples,
                         typename Curve::scalar_t axis_len, ...);

void draw_rmf(RenderBuffer&, ConstSpan<CurveFrame<f32>>, ConstSpan<Vec3f>,
              f32 axis_len, ...);
}
```

Colour pin: tangent = red, normal = green, binormal = blue (matches the
axis-triad convention).

## v10-close typed boundary

Quantity-aware `*_typed` wrappers cover the WHOLE v10 surface in
`queries_typed.hpp`. Strip-then-compute-then-retag (ADR-0078 §5).

```cpp
template <TypedCurve Curve>  // Curve::scalar_t = Quantity<D, T>
Vec3<typename Curve::scalar_t> evaluate_typed(
    const Curve& curve, scalar_t t, IAllocator* alloc) noexcept;

template <TypedCurve Curve>
Polyline3<typename Curve::scalar_t> sample_uniform_typed(...);

template <TypedCurve Curve>
ArclengthTable<scalar_t> build_arclength_table_typed(...);  // returns RAW table per D215

template <typename D, typename T>
Quantity<D, T> length_of_typed(const ArclengthTable<T>& table) noexcept;  // re-tag on read

template <TypedCurve Curve>
CurveClosestPointQ<D, T> closest_point_typed(...);  // typed point + Area distance²

template <TypedCurve Curve>
std::optional<CurveRayHitQ<D, T>> intersect_ray_typed(...);

template <TypedCurve Curve>
Vec3<scalar_t_of_Curve> tangent_typed(...);  // returns RAW Vec3<T> per D216

template <TypedCurve Curve>
Array<CurveFrame<T>> compute_rmf_typed(...);  // returns RAW frames per D216
```

D214 unified strip pattern: `detail_typed::strip(curve, alloc)`
constexpr-strips value kinds (Bezier / Hermite / Arc) at zero cost;
allocates short-lived raw copies for owning kinds (Polyline3 /
CatmullRom3 / BSpline3). Every `*_typed` wrapper takes
`IAllocator* alloc` for API uniformity, even on entry points whose raw
signatures don't (cost is honest on owning kinds, zero on value kinds).
D215: `ArclengthTable<T>` stays raw; typed accessors re-tag at read.
D216: T / N / B / RMF return raw `Vec3<T>` / `Array<CurveFrame<T>>` —
unit vectors are dimensionless by definition.

## Test corpus

- **`crd-geometry-curves-tests`**: 95 cases / 3 010 assertions —
  substrate (37 / 135) + sampling (11 / ~2 114) + arc-length (13 / 196)
  + queries (11 / 285) + frames (9 / 387) + typed (14 / 89).
- **`crd-geometry-viz-tests`**: +5 cases / 5 assertions covering
  `draw_curve` / `draw_polyline` / `draw_tangent_frame` / `draw_rmf`.
- **`crd-sandbox-showcase-tests`**: +2 cases (every curve kind renders,
  frame-mode toggle adds 3×N hair lines) + 1 enum-stability `static_assert`
  pinning `SandboxScene::CurvesShowcase == 3`.

Discriminator tests (catch regressions reliably):
- **v10e Wang reflection-sign**: planar circular arc → all RMF normals
  coplanar with arc plane; all RMF binormals = plane normal up to a
  single global sign.
- **v10-close strip-then-call**: every `*_typed` call produces results
  bit-equal to `strip(typed) → raw` then call.

## Closed-cluster decision pins

ADR-0076 §27 locks **D182-D216** (35 decisions across 6 sub-slices).
Filed follow-on slices listed there are not regressions, just
consumer-pull deferrals.

## Linkage

```
crd-geometry-curves PUBLIC →
  crd-core / crd-containers / crd-memory / crd-math / crd-units /
  crd-geometry-primitives

crd-geometry-viz PUBLIC →
  crd-core / crd-math / crd-containers / crd-geometry-primitives /
  crd-geometry-bvh / crd-geometry-curves / crd-draw

sandbox (crd-sandbox executable) PRIVATE → crd-geometry-curves
                                            crd-geometry-viz
```

The substrate's downstream consumers stay narrow: typed curve fields on
ECS components (Phase 3.1+) consume `crd-geometry-curves` directly; the
sandbox + future editor pull `crd-geometry-viz` for visualisation; the
GPU side has no consumers yet (curves are CPU-side; GPU tessellation
would be a future module).
