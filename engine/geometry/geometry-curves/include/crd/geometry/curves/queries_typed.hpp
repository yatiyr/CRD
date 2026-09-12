#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves -- Typed boundary layer. Phase 3.1.7 v10-close (2026-05-19).
//
// Quantity-aware wrappers over the entire v10 public surface (evaluate +
// sample + arclength + queries + frames). Mirrors the
// `crd-geometry-primitives` `queries_typed.hpp` pattern (Phase 3.1.7.5 v0d-2):
// strip the Dim tag at the wrapper boundary, call the raw algorithm, re-tag
// the result. Algorithms in the substrate stay raw `f32`/`f64`; the dim tag
// rides at the API surface only (ADR-0078 §5).
//
// **D214 (planned for ADR-0076 §27)** -- Strip pattern: unified
// `detail_typed::strip(typed_curve, alloc)` for every kind. Value kinds
// (Bezier / Hermite / Arc / EllipseArc) ignore `alloc` and constexpr-strip
// at zero runtime cost. Owning kinds (Polyline3 / View / CatmullRom3 /
// BSpline3) allocate a short-lived raw copy via the function's existing
// `IAllocator*` parameter; scratch is freed when the wrapper returns. To
// keep the API uniform, every `*_typed` wrapper takes `IAllocator* alloc`,
// even on entry points (evaluate / evaluate_derivative / tangent / normal /
// binormal) whose raw signatures don't -- the alloc is the honest cost of
// the typed boundary on owning kinds and a free pass-through on value kinds.
//
// **D215 (planned for ADR-0076 §27)** -- `ArclengthTable<T>` stays raw.
// Typed `build_arclength_table_typed` accepts a typed curve and returns
// the raw table. Typed accessors (`length_of_typed`, `t_at_distance_typed`,
// `distance_at_t_typed`) re-tag at the read site. Reason: the table's
// monotone-`t` binary search is dimensionless; tagging knots with Length
// would be a categorical lie.
//
// **D216 (planned for ADR-0076 §27)** -- `tangent_typed`, `normal_typed`,
// `binormal_typed`, `compute_rmf_typed` return raw `Vec3<T>` and raw
// `Array<CurveFrame<T>>`. Unit vectors are dimensionless by definition;
// tagging them with the input curve's Length dim would be a categorical
// lie. Consumers wishing to scale a frame to a Length compose
// `Vec3<Length<T>>{tangent * len}` at the call site.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/arc.hpp>
#include <crd/geometry/curves/arclength.hpp>
#include <crd/geometry/curves/bezier.hpp>
#include <crd/geometry/curves/bspline.hpp>
#include <crd/geometry/curves/catmull_rom.hpp>
#include <crd/geometry/curves/evaluator.hpp>
#include <crd/geometry/curves/frames.hpp>
#include <crd/geometry/curves/hermite.hpp>
#include <crd/geometry/curves/polyline.hpp>
#include <crd/geometry/curves/queries.hpp>
#include <crd/geometry/curves/sample.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <optional>
#include <type_traits>

namespace crd::geometry::curves
{

using crd::math::from_raw_vec;
using crd::math::to_raw_vec;

// ---------------------------------------------------------------------------
// detail_typed -- strip + retag helpers.
// ---------------------------------------------------------------------------

namespace detail_typed
{

// Trait: does the type T look like Quantity<D, S>?
template <typename T>
struct quantity_traits
{
    static constexpr bool is_quantity = false;
};
template <typename D, typename T>
struct quantity_traits<crd::units::Quantity<D, T>>
{
    static constexpr bool is_quantity = true;
    using dim_t                       = D;
    using scalar_t                    = T;
};

template <typename Q>
using quantity_dim_t = typename quantity_traits<Q>::dim_t;

template <typename Q>
using quantity_scalar_t = typename quantity_traits<Q>::scalar_t;

// Value-kind strips: constexpr; alloc parameter accepted + ignored to keep a
// uniform `strip(curve, alloc)` dispatch shape.

template <typename D, typename T>
[[nodiscard]] constexpr QuadBezier3<T> strip(const QuadBezier3<crd::units::Quantity<D, T>>& c,
                                              crd::memory::IAllocator* /*alloc*/) noexcept
{
    return QuadBezier3<T>{to_raw_vec(c.p0), to_raw_vec(c.p1), to_raw_vec(c.p2)};
}

template <typename D, typename T>
[[nodiscard]] constexpr CubicBezier3<T> strip(const CubicBezier3<crd::units::Quantity<D, T>>& c,
                                               crd::memory::IAllocator* /*alloc*/) noexcept
{
    return CubicBezier3<T>{to_raw_vec(c.p0), to_raw_vec(c.p1), to_raw_vec(c.p2), to_raw_vec(c.p3)};
}

template <typename D, typename T>
[[nodiscard]] constexpr CubicHermite3<T> strip(const CubicHermite3<crd::units::Quantity<D, T>>& c,
                                                crd::memory::IAllocator* /*alloc*/) noexcept
{
    return CubicHermite3<T>{to_raw_vec(c.p0), to_raw_vec(c.p1), to_raw_vec(c.t0), to_raw_vec(c.t1)};
}

template <typename D, typename T>
[[nodiscard]] constexpr CircularArc3<T> strip(const CircularArc3<crd::units::Quantity<D, T>>& c,
                                               crd::memory::IAllocator* /*alloc*/) noexcept
{
    return CircularArc3<T>{to_raw_vec(c.center), to_raw_vec(c.axis_u), to_raw_vec(c.axis_v),
                           c.radius.value,       c.sweep_radians.value, c.closed};
}

template <typename D, typename T>
[[nodiscard]] constexpr EllipseArc3<T> strip(const EllipseArc3<crd::units::Quantity<D, T>>& c,
                                              crd::memory::IAllocator* /*alloc*/) noexcept
{
    return EllipseArc3<T>{to_raw_vec(c.center),    to_raw_vec(c.axis_u),    to_raw_vec(c.axis_v),
                          c.radius_u.value,        c.radius_v.value,        c.sweep_radians.value,
                          c.closed};
}

// Owning-kind strips: allocate raw owning copies via the function's alloc.

template <typename D, typename T>
[[nodiscard]] Polyline3<T> strip(const Polyline3<crd::units::Quantity<D, T>>& c,
                                  crd::memory::IAllocator*                    alloc)
{
    Polyline3<T> raw(alloc);
    raw.closed = c.closed;
    raw.points.reserve(c.points.size());
    for (const auto& p : c.points) { raw.points.push_back(to_raw_vec(p)); }
    return raw;
}

template <typename D, typename T>
[[nodiscard]] Polyline3<T> strip(const Polyline3View<crd::units::Quantity<D, T>>& v,
                                  crd::memory::IAllocator*                         alloc)
{
    Polyline3<T> raw(alloc);
    raw.closed = v.closed;
    raw.points.reserve(v.points.size());
    for (const auto& p : v.points) { raw.points.push_back(to_raw_vec(p)); }
    return raw;
}

template <typename D, typename T>
[[nodiscard]] CatmullRom3<T> strip(const CatmullRom3<crd::units::Quantity<D, T>>& c,
                                    crd::memory::IAllocator*                       alloc)
{
    crd::containers::Array<crd::math::Vec3<T>> raw_pts(alloc);
    raw_pts.reserve(c.points.size());
    for (const auto& p : c.points) { raw_pts.push_back(to_raw_vec(p)); }
    return CatmullRom3<T>(alloc,
                          crd::containers::ConstSpan<crd::math::Vec3<T>>{raw_pts.data(), raw_pts.size()},
                          c.param, c.closed);
}

template <typename D, typename T>
[[nodiscard]] BSpline3<T> strip(const BSpline3<crd::units::Quantity<D, T>>& c,
                                 crd::memory::IAllocator*                    alloc)
{
    BSpline3<T> raw(alloc);
    raw.closed = c.closed;
    raw.points.reserve(c.points.size());
    for (const auto& p : c.points) { raw.points.push_back(to_raw_vec(p)); }
    raw.knots.reserve(c.knots.size());
    for (const auto& k : c.knots) { raw.knots.push_back(k.value); }
    return raw;
}

// Adapter: pass-through for value / view kinds; .view() for owning Polyline3
// (the raw `evaluate` for polylines is defined on `Polyline3View<T>`, not
// `Polyline3<T>`, so the stripped owning raw curve must be projected to a
// view before downstream algorithms can deduce its `evaluate` overload).
template <typename T>
struct is_polyline3_owning : std::false_type
{
};
template <typename T>
struct is_polyline3_owning<Polyline3<T>> : std::true_type
{
};

template <typename Curve>
[[nodiscard]] inline decltype(auto) to_view(const Curve& curve) noexcept
{
    if constexpr (is_polyline3_owning<Curve>::value)
    {
        return curve.view();
    }
    else
    {
        return (curve); // parenthesised to force `const Curve&` deduction
    }
}

// Re-tag every point in a raw Polyline3<T> back to Polyline3<Quantity<D, T>>.
template <typename D, typename T>
[[nodiscard]] Polyline3<crd::units::Quantity<D, T>> retag_polyline(
    const Polyline3<T>& raw, crd::memory::IAllocator* alloc)
{
    Polyline3<crd::units::Quantity<D, T>> out(alloc);
    out.closed = raw.closed;
    out.points.reserve(raw.points.size());
    for (const auto& p : raw.points) { out.points.push_back(from_raw_vec<D>(p)); }
    return out;
}

template <typename D, typename T>
[[nodiscard]] constexpr crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>> retag_aabb(
    const crd::geometry::primitives::AABB3<T>& raw) noexcept
{
    return crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>{from_raw_vec<D>(raw.min),
                                                                          from_raw_vec<D>(raw.max)};
}

} // namespace detail_typed

// ---------------------------------------------------------------------------
// Concept used on every typed wrapper: Curve::scalar_t must be a Quantity.
// ---------------------------------------------------------------------------

template <typename Curve>
concept TypedCurve = detail_typed::quantity_traits<typename Curve::scalar_t>::is_quantity;

// ---------------------------------------------------------------------------
// evaluate_typed / evaluate_derivative_typed -- both take alloc (D214).
// ---------------------------------------------------------------------------

template <TypedCurve Curve>
[[nodiscard]] inline crd::math::Vec3<typename Curve::scalar_t> evaluate_typed(
    const Curve& curve, detail_typed::quantity_scalar_t<typename Curve::scalar_t> t,
    crd::memory::IAllocator* alloc) noexcept
{
    using D        = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    auto raw_curve = detail_typed::strip(curve, alloc);
    return from_raw_vec<D>(evaluate(detail_typed::to_view(raw_curve), t));
}

template <TypedCurve Curve>
[[nodiscard]] inline crd::math::Vec3<typename Curve::scalar_t> evaluate_derivative_typed(
    const Curve& curve, detail_typed::quantity_scalar_t<typename Curve::scalar_t> t,
    crd::memory::IAllocator* alloc) noexcept
{
    using D        = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    auto raw_curve = detail_typed::strip(curve, alloc);
    return from_raw_vec<D>(evaluate_derivative(detail_typed::to_view(raw_curve), t));
}

// ---------------------------------------------------------------------------
// Sampling: sample_uniform_typed / sample_adaptive_typed /
// sample_by_curvature_typed / to_polyline_typed.
// ---------------------------------------------------------------------------

template <TypedCurve Curve>
[[nodiscard]] inline Polyline3<typename Curve::scalar_t> sample_uniform_typed(
    const Curve& curve, crd::u32 n_segments, crd::memory::IAllocator* alloc) noexcept
{
    using D = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    using T = detail_typed::quantity_scalar_t<typename Curve::scalar_t>;
    auto raw_curve = detail_typed::strip(curve, alloc);
    auto raw       = sample_uniform(detail_typed::to_view(raw_curve), n_segments, alloc);
    return detail_typed::retag_polyline<D, T>(raw, alloc);
}

template <TypedCurve Curve>
[[nodiscard]] inline Polyline3<typename Curve::scalar_t> sample_adaptive_typed(
    const Curve&                                                                                 curve,
    crd::units::Quantity<detail_typed::quantity_dim_t<typename Curve::scalar_t>,
                          detail_typed::quantity_scalar_t<typename Curve::scalar_t>>             tolerance,
    crd::memory::IAllocator*                                                                     alloc) noexcept
{
    using D        = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    using T        = detail_typed::quantity_scalar_t<typename Curve::scalar_t>;
    auto raw_curve = detail_typed::strip(curve, alloc);
    auto raw       = sample_adaptive(detail_typed::to_view(raw_curve), tolerance.value, alloc);
    return detail_typed::retag_polyline<D, T>(raw, alloc);
}

template <TypedCurve Curve>
[[nodiscard]] inline Polyline3<typename Curve::scalar_t> sample_by_curvature_typed(
    const Curve&                                                                  curve,
    crd::units::Angle<detail_typed::quantity_scalar_t<typename Curve::scalar_t>>  max_angle_step,
    crd::memory::IAllocator*                                                      alloc) noexcept
{
    using D        = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    using T        = detail_typed::quantity_scalar_t<typename Curve::scalar_t>;
    auto raw_curve = detail_typed::strip(curve, alloc);
    auto raw = sample_by_curvature(detail_typed::to_view(raw_curve), max_angle_step.value, alloc);
    return detail_typed::retag_polyline<D, T>(raw, alloc);
}

template <TypedCurve Curve>
[[nodiscard]] inline Polyline3<typename Curve::scalar_t> to_polyline_typed(
    const Curve& curve, crd::memory::IAllocator* alloc) noexcept
{
    using D        = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    using T        = detail_typed::quantity_scalar_t<typename Curve::scalar_t>;
    auto raw_curve = detail_typed::strip(curve, alloc);
    auto raw       = to_polyline(detail_typed::to_view(raw_curve), alloc);
    return detail_typed::retag_polyline<D, T>(raw, alloc);
}

// ---------------------------------------------------------------------------
// Arc-length: build_arclength_table_typed returns the raw table per D215.
// length_of_typed / t_at_distance_typed / distance_at_t_typed re-tag at read.
// ---------------------------------------------------------------------------

template <TypedCurve Curve>
[[nodiscard]] inline ArclengthTable<detail_typed::quantity_scalar_t<typename Curve::scalar_t>>
build_arclength_table_typed(const Curve& curve, crd::u32 n_samples,
                             crd::memory::IAllocator* alloc) noexcept
{
    auto raw_curve = detail_typed::strip(curve, alloc);
    return build_arclength_table(detail_typed::to_view(raw_curve), n_samples, alloc);
}

// length_of_typed(raw_table) -> Length<T> (typed read accessor on a raw table).
template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T> length_of_typed(
    const ArclengthTable<T>& table) noexcept
{
    return crd::units::Quantity<D, T>{table.total_length};
}

// length_of_typed(curve, n, alloc) -- convenience that builds the table then
// reads its total_length.
template <TypedCurve Curve>
[[nodiscard]] inline typename Curve::scalar_t length_of_typed(
    const Curve& curve, crd::u32 n_samples, crd::memory::IAllocator* alloc) noexcept
{
    using D    = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    using T    = detail_typed::quantity_scalar_t<typename Curve::scalar_t>;
    auto table = build_arclength_table_typed(curve, n_samples, alloc);
    return length_of_typed<D, T>(table);
}

template <typename D, typename T>
[[nodiscard]] inline T t_at_distance_typed(const ArclengthTable<T>& table,
                                            crd::units::Quantity<D, T> distance) noexcept
{
    return t_at_distance(table, distance.value);
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T> distance_at_t_typed(
    const ArclengthTable<T>& table, T t) noexcept
{
    return crd::units::Quantity<D, T>{distance_at_t(table, t)};
}

// ---------------------------------------------------------------------------
// aabb_of_typed
// ---------------------------------------------------------------------------

template <TypedCurve Curve>
[[nodiscard]] inline crd::geometry::primitives::AABB3<typename Curve::scalar_t> aabb_of_typed(
    const Curve& curve, crd::memory::IAllocator* alloc) noexcept
{
    using D = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    using T = detail_typed::quantity_scalar_t<typename Curve::scalar_t>;
    auto raw_curve = detail_typed::strip(curve, alloc);
    return detail_typed::retag_aabb<D, T>(aabb_of(detail_typed::to_view(raw_curve), alloc));
}

// ---------------------------------------------------------------------------
// closest_point_typed / distance_typed
// ---------------------------------------------------------------------------

template <typename D, typename T>
struct CurveClosestPointQ
{
    T                                              t;            // raw curve parameter
    crd::math::Vec3<crd::units::Quantity<D, T>>    point;        // typed position
    crd::units::Quantity<crd::units::dim::Area, T> distance_squared;
};

template <TypedCurve Curve>
[[nodiscard]] inline CurveClosestPointQ<detail_typed::quantity_dim_t<typename Curve::scalar_t>,
                                          detail_typed::quantity_scalar_t<typename Curve::scalar_t>>
closest_point_typed(const Curve&                                       curve,
                     const crd::math::Vec3<typename Curve::scalar_t>&    p,
                     detail_typed::quantity_scalar_t<typename Curve::scalar_t> tolerance,
                     crd::memory::IAllocator*                            alloc) noexcept
{
    using D  = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    using T  = detail_typed::quantity_scalar_t<typename Curve::scalar_t>;
    auto raw_curve = detail_typed::strip(curve, alloc);
    auto raw = closest_point(detail_typed::to_view(raw_curve), to_raw_vec(p), tolerance, alloc);
    return CurveClosestPointQ<D, T>{raw.t, from_raw_vec<D>(raw.point),
                                      crd::units::Quantity<crd::units::dim::Area, T>{raw.distance_squared}};
}

template <TypedCurve Curve>
[[nodiscard]] inline typename Curve::scalar_t distance_typed(
    const Curve&                                       curve,
    const crd::math::Vec3<typename Curve::scalar_t>&    p,
    detail_typed::quantity_scalar_t<typename Curve::scalar_t> tolerance,
    crd::memory::IAllocator*                            alloc) noexcept
{
    using D = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    using T = detail_typed::quantity_scalar_t<typename Curve::scalar_t>;
    auto raw_curve = detail_typed::strip(curve, alloc);
    return crd::units::Quantity<D, T>{distance(detail_typed::to_view(raw_curve), to_raw_vec(p),
                                                tolerance, alloc)};
}

// ---------------------------------------------------------------------------
// intersect_ray_typed
// ---------------------------------------------------------------------------

template <typename D, typename T>
struct CurveRayHitQ
{
    T                                              t_curve; // raw curve param
    T                                              t_ray;   // raw ray param
    crd::math::Vec3<crd::units::Quantity<D, T>>    point;   // typed position
};

template <TypedCurve Curve>
[[nodiscard]] inline std::optional<
    CurveRayHitQ<detail_typed::quantity_dim_t<typename Curve::scalar_t>,
                  detail_typed::quantity_scalar_t<typename Curve::scalar_t>>>
intersect_ray_typed(const Curve&                                              curve,
                     const crd::geometry::primitives::Ray3<typename Curve::scalar_t>& ray,
                     detail_typed::quantity_scalar_t<typename Curve::scalar_t> tolerance,
                     crd::memory::IAllocator*                            alloc) noexcept
{
    using D = detail_typed::quantity_dim_t<typename Curve::scalar_t>;
    using T = detail_typed::quantity_scalar_t<typename Curve::scalar_t>;
    const crd::geometry::primitives::Ray3<T> raw_ray{to_raw_vec(ray.origin), to_raw_vec(ray.direction)};
    auto raw_curve = detail_typed::strip(curve, alloc);
    auto raw_hit = intersect_ray(detail_typed::to_view(raw_curve), raw_ray, tolerance, alloc);
    if (!raw_hit) { return std::nullopt; }
    return CurveRayHitQ<D, T>{raw_hit->t_curve, raw_hit->t_ray, from_raw_vec<D>(raw_hit->point)};
}

// ---------------------------------------------------------------------------
// Frames: tangent_typed / normal_typed / binormal_typed / compute_rmf_typed.
// All return RAW Vec3<T> / Array<CurveFrame<T>> per D216.
// ---------------------------------------------------------------------------

template <TypedCurve Curve>
[[nodiscard]] inline crd::math::Vec3<detail_typed::quantity_scalar_t<typename Curve::scalar_t>>
tangent_typed(const Curve& curve, detail_typed::quantity_scalar_t<typename Curve::scalar_t> t,
               crd::memory::IAllocator* alloc) noexcept
{
    auto raw_curve = detail_typed::strip(curve, alloc);
    return tangent(detail_typed::to_view(raw_curve), t);
}

template <TypedCurve Curve>
[[nodiscard]] inline crd::math::Vec3<detail_typed::quantity_scalar_t<typename Curve::scalar_t>>
normal_typed(const Curve& curve, detail_typed::quantity_scalar_t<typename Curve::scalar_t> t,
              crd::memory::IAllocator* alloc) noexcept
{
    auto raw_curve = detail_typed::strip(curve, alloc);
    return normal(detail_typed::to_view(raw_curve), t);
}

template <TypedCurve Curve>
[[nodiscard]] inline crd::math::Vec3<detail_typed::quantity_scalar_t<typename Curve::scalar_t>>
binormal_typed(const Curve& curve, detail_typed::quantity_scalar_t<typename Curve::scalar_t> t,
                crd::memory::IAllocator* alloc) noexcept
{
    auto raw_curve = detail_typed::strip(curve, alloc);
    return binormal(detail_typed::to_view(raw_curve), t);
}

template <TypedCurve Curve>
[[nodiscard]] inline crd::containers::Array<
    CurveFrame<detail_typed::quantity_scalar_t<typename Curve::scalar_t>>>
compute_rmf_typed(const Curve& curve, crd::u32 n_samples,
                   crd::memory::IAllocator* alloc) noexcept
{
    auto raw_curve = detail_typed::strip(curve, alloc);
    return compute_rmf(detail_typed::to_view(raw_curve), n_samples, alloc);
}

} // namespace crd::geometry::curves
