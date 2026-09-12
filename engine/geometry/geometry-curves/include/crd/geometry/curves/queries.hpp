#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Curve queries. Phase 3.1.7 v10d (2026-05-19).
//
// Three queries + a convenience wrapper:
//   - aabb_of(curve)             — subdivision-bounded AABB.
//   - closest_point(curve, p)    — global minimum via subdivision-rejection
//                                  + Newton-Raphson on `(curve(t) - p) ·
//                                  curve'(t) = 0`.
//   - distance(curve, p)         — convenience wrapper around closest_point.
//   - intersect_ray(curve, ray)  — first-hit ray-vs-flattened-polyline.
//
// All four are generic over the curve kind (deduced via `Curve::scalar_t`)
// and bottom out on `evaluate(curve, t)` per D186.
//
// **D203 (planned for v10-close)** — `aabb_of` uses adaptive-subdivision
// flattening + min/max walk. Universal across curve kinds. Analytic
// specialisations (CircularArc + EllipseArc admit closed-form AABBs;
// Bezier admits convex-hull-of-control-points loose AABB) filed as
// `v10d-analytic-aabb` follow-on.
//
// **D204 (planned for v10-close)** — `closest_point` initial-guess via
// 16-uniform-sample rejection (the seed for Newton). Picks the GLOBAL
// minimum, not the local minimum near t=0.5.
//
// **D205 (planned for v10-close)** — Newton-Raphson convergence: stop on
// `|delta_t| <= tolerance` OR after `k_closest_point_max_iters = 32`
// iterations. Open curves clamp `t` to `[0, 1]`; closed curves wrap.
// f'(t) is computed via finite-difference of f(t) to avoid needing a
// curve-kind-specific second-derivative path; the analytic second
// derivative is a `v10d-analytic-second-derivative` follow-on if a
// consumer needs the perf.
//
// **D206 (planned for v10-close)** — `intersect_ray` returns the FIRST hit
// (smallest `t_ray`) where the ray comes within `tolerance` of any
// polyline segment. Returns `std::nullopt` on miss. Curves are zero-
// thickness, so "intersection" means "closest-approach within
// tolerance"; this matches cinematic-camera trigger-volume + editor
// cursor-pick semantics. Multi-hit form filed as `v10d-multi-hit-ray`.
//
// **D207 (planned for v10-close)** — `tolerance` units = length (same
// scalar as curve control points). No `tolerance²` API quirk. Default:
// `crd::geometry::primitives::k_distance_epsilon<T>()`.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/evaluator.hpp>
#include <crd/geometry/curves/sample.hpp>
#include <crd/geometry/primitives/constants.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <optional>

namespace crd::geometry::curves
{

inline constexpr crd::u32 k_closest_point_initial_samples = 16U;
inline constexpr crd::u32 k_closest_point_max_iters       = 32U;

template <crd::math::MathScalar T> struct CurveClosestPoint
{
    T                  t;                // curve parameter [0, 1]
    crd::math::Vec3<T> point;             // evaluate(curve, t)
    T                  distance_squared;  // ||point - p||²
};

template <crd::math::MathScalar T> struct CurveRayHit
{
    T                  t_curve;  // curve parameter at the hit
    T                  t_ray;    // ray parameter at the hit
    crd::math::Vec3<T> point;    // ray.origin + t_ray * ray.direction (≈ curve(t_curve))
};

// ---------------------------------------------------------------------------
// aabb_of — bounded AABB via uniform-sampling flattening.
//
// Uses `sample_uniform(curve, k_aabb_default_samples=64)` instead of the
// adaptive sampler — AABB doesn't need adaptive's fine refinement near
// high curvature (it just needs enough samples to capture extremes), and
// `sample_adaptive` at the default `k_distance_epsilon` tolerance hits
// the depth cap on most curves (65 K samples → multi-MB temp allocation
// per query). Uniform 64-sample is bounded-cost + sufficient AABB
// fidelity for any realistic use case. Caller wanting a tighter AABB
// calls `sample_adaptive` directly + walks min/max.
// ---------------------------------------------------------------------------

inline constexpr crd::u32 k_aabb_default_samples = 64U;

template <typename Curve>
[[nodiscard]] crd::geometry::primitives::AABB3<typename Curve::scalar_t> aabb_of(
    const Curve& curve, crd::memory::IAllocator* alloc) noexcept
{
    using T = typename Curve::scalar_t;

    const auto polyline = sample_uniform(curve, k_aabb_default_samples, alloc);
    CRD_ASSERT(polyline.points.size() >= 1U);

    auto mn = polyline.points[0];
    auto mx = polyline.points[0];
    for (crd::usize i = 1U; i < polyline.points.size(); ++i)
    {
        const auto& p = polyline.points[i];
        mn.x = crd::math::min(mn.x, p.x);
        mn.y = crd::math::min(mn.y, p.y);
        mn.z = crd::math::min(mn.z, p.z);
        mx.x = crd::math::max(mx.x, p.x);
        mx.y = crd::math::max(mx.y, p.y);
        mx.z = crd::math::max(mx.z, p.z);
    }
    return crd::geometry::primitives::AABB3<T>{mn, mx};
}

// ---------------------------------------------------------------------------
// closest_point — global minimum via subdivision-rejection + Newton.
//
// Algorithm:
//   1. Sample N=16 uniform t values, pick the one minimising
//      ||curve(t) - p||² as the initial-guess seed.
//   2. Newton-Raphson on f(t) = (curve(t) - p) · curve'(t) = 0:
//          delta_t = -f(t) / f'(t)
//          t_new = clamp/wrap(t + delta_t)
//      f'(t) computed via finite-difference (D205): f' ≈ (f(t+h) - f(t))/h.
//   3. Stop on |delta_t| <= tolerance OR k_closest_point_max_iters reached.
//
// Closed curves wrap; open curves clamp t to [0, 1].
// ---------------------------------------------------------------------------

template <typename Curve>
[[nodiscard]] CurveClosestPoint<typename Curve::scalar_t> closest_point(
    const Curve&                                                          curve,
    const crd::math::Vec3<typename Curve::scalar_t>&                      p,
    typename Curve::scalar_t                                              tolerance,
    crd::memory::IAllocator* /*unused; kept for API symmetry with aabb_of*/) noexcept
{
    using T = typename Curve::scalar_t;
    CRD_ASSERT(tolerance > static_cast<T>(0));

    // Step 1: subdivision-rejection initial guess (D204).
    T best_t  = static_cast<T>(0);
    T best_d2 = crd::math::length_squared(evaluate(curve, static_cast<T>(0)) - p);
    for (crd::u32 i = 1U; i <= k_closest_point_initial_samples; ++i)
    {
        const T t  = static_cast<T>(i) / static_cast<T>(k_closest_point_initial_samples);
        const T d2 = crd::math::length_squared(evaluate(curve, t) - p);
        if (d2 < best_d2)
        {
            best_d2 = d2;
            best_t  = t;
        }
    }

    // Step 2: Newton-Raphson (D205).
    T       t              = best_t;
    const T fd_step        = static_cast<T>(1e-4);
    for (crd::u32 iter = 0U; iter < k_closest_point_max_iters; ++iter)
    {
        const auto curve_t = evaluate(curve, t);
        const auto deriv_t = evaluate_derivative(curve, t);
        const auto diff    = curve_t - p;
        const T    f       = crd::math::dot(diff, deriv_t);

        // Finite-difference f'(t).
        const T    t_plus  = t + fd_step;
        const auto curve_p = evaluate(curve, t_plus);
        const auto deriv_p = evaluate_derivative(curve, t_plus);
        const T    f_plus  = crd::math::dot(curve_p - p, deriv_p);
        const T    fp      = (f_plus - f) / fd_step;

        // Defensive: if f'(t) is near zero, we're at a stationary point or
        // a saddle. Stop — best_t from subdivision is the best we have.
        if (crd::math::abs(fp) <= tolerance) { break; }

        T t_new = t - f / fp;
        if (curve.closed)
        {
            t_new = detail::floor_mod(t_new, static_cast<T>(1));
        }
        else
        {
            t_new = crd::math::clamp(t_new, static_cast<T>(0), static_cast<T>(1));
        }
        if (crd::math::abs(t_new - t) <= tolerance)
        {
            t = t_new;
            break;
        }
        t = t_new;
    }

    const auto cp = evaluate(curve, t);
    return CurveClosestPoint<T>{t, cp, crd::math::length_squared(cp - p)};
}

// Convenience: distance == sqrt(closest_point.distance_squared).
template <typename Curve>
[[nodiscard]] typename Curve::scalar_t distance(
    const Curve&                                     curve,
    const crd::math::Vec3<typename Curve::scalar_t>& p,
    typename Curve::scalar_t                         tolerance,
    crd::memory::IAllocator*                         alloc) noexcept
{
    using T          = typename Curve::scalar_t;
    const auto cp    = closest_point(curve, p, tolerance, alloc);
    return static_cast<T>(crd::math::sqrt(static_cast<double>(cp.distance_squared)));
}

// ---------------------------------------------------------------------------
// intersect_ray — first-hit ray-vs-flattened-polyline.
//
// Algorithm:
//   1. Flatten curve via `sample_adaptive(curve, tolerance, alloc)`.
//   2. For each polyline segment [a, b]: compute closest-approach between
//      the infinite ray and the finite segment. If min distance <= tolerance
//      AND t_ray >= 0: record the hit's t_ray + interpolate t_curve.
//   3. Return the smallest-t_ray hit, or nullopt if no segment qualified.
// ---------------------------------------------------------------------------

namespace detail
{

// Closest-approach between a ray (origin O + s * D, s >= 0) and a segment
// [a, b]. Output: `ray_param` (parametric along ray), `seg_param` (in
// [0, 1] along segment), `min_dist_sq` (squared distance at closest
// approach). Caller responsible for the tolerance check.
template <crd::math::MathScalar T>
struct RaySegmentClosest
{
    T ray_param;
    T seg_param;
    T min_dist_sq;
};

template <crd::math::MathScalar T>
[[nodiscard]] RaySegmentClosest<T> ray_segment_closest(
    const crd::math::Vec3<T>& origin,
    const crd::math::Vec3<T>& direction,
    const crd::math::Vec3<T>& a,
    const crd::math::Vec3<T>& b) noexcept
{
    // Standard "two skew lines closest approach", clamped on the segment.
    const auto d1 = direction;
    const auto d2 = b - a;
    const auto r  = origin - a;
    const T    a11 = crd::math::dot(d1, d1);
    const T    a22 = crd::math::dot(d2, d2);
    const T    a12 = crd::math::dot(d1, d2);
    const T    b1  = crd::math::dot(d1, r);
    const T    b2  = crd::math::dot(d2, r);
    const T    denom = a11 * a22 - a12 * a12;

    T s = static_cast<T>(0);
    T u = static_cast<T>(0);
    if (denom > static_cast<T>(0))
    {
        s = (a12 * b2 - a22 * b1) / denom;
        u = (a11 * b2 - a12 * b1) / denom;
    }
    else
    {
        // Parallel: pick s such that the foot on the ray lands at the
        // segment's midpoint, then clamp.
        u = static_cast<T>(0.5);
        s = (b1 + a12 * u) / (a11 > static_cast<T>(0) ? a11 : static_cast<T>(1));
    }
    // Clamp segment param to [0, 1]; ray param to [0, +inf).
    u = crd::math::clamp(u, static_cast<T>(0), static_cast<T>(1));
    if (s < static_cast<T>(0)) { s = static_cast<T>(0); }
    // Recompute distance at the clamped (s, u).
    const auto on_ray  = origin + d1 * s;
    const auto on_seg  = a + d2 * u;
    const T    dist_sq = crd::math::length_squared(on_ray - on_seg);
    return RaySegmentClosest<T>{s, u, dist_sq};
}

} // namespace detail

template <typename Curve>
[[nodiscard]] std::optional<CurveRayHit<typename Curve::scalar_t>> intersect_ray(
    const Curve&                                                                curve,
    const crd::geometry::primitives::Ray3<typename Curve::scalar_t>&            ray,
    typename Curve::scalar_t                                                    tolerance,
    crd::memory::IAllocator*                                                    alloc) noexcept
{
    using T = typename Curve::scalar_t;
    CRD_ASSERT(tolerance > static_cast<T>(0));

    const auto polyline = sample_adaptive(curve, tolerance, alloc);
    if (polyline.points.size() < 2U) { return std::nullopt; }

    const T tol_sq = tolerance * tolerance;
    const auto n_pts = static_cast<crd::u32>(polyline.points.size());
    const auto n_segs = polyline.closed ? n_pts : (n_pts - 1U);

    std::optional<CurveRayHit<T>> best;
    for (crd::u32 i = 0U; i < n_segs; ++i)
    {
        const auto& a = polyline.points[i];
        const auto& b = polyline.points[(i + 1U) % n_pts];
        const auto  cap = detail::ray_segment_closest(ray.origin, ray.direction, a, b);
        if (cap.min_dist_sq > tol_sq) { continue; }
        if (cap.ray_param < static_cast<T>(0)) { continue; }
        // Reconstruct t_curve: linear-interp the segment's t-fraction by
        // its position in the polyline. For an open polyline of N+1
        // samples there are N segments; segment i covers t in
        // [i/N, (i+1)/N]. For closed: segment i covers [i/N, (i+1)/N]
        // with the last segment wrapping to t=0 — we report t at the
        // segment START + u * (1/N) for both, since wrap is the consumer's
        // responsibility once they see closed=true on the original curve.
        const T seg_start_t = static_cast<T>(i) / static_cast<T>(n_segs);
        const T seg_span_t  = static_cast<T>(1) / static_cast<T>(n_segs);
        const T t_curve     = seg_start_t + cap.seg_param * seg_span_t;
        const auto hit_pt   = ray.origin + ray.direction * cap.ray_param;
        const CurveRayHit<T> candidate{t_curve, cap.ray_param, hit_pt};
        if (!best.has_value() || candidate.t_ray < best->t_ray)
        {
            best = candidate;
        }
    }
    return best;
}

} // namespace crd::geometry::curves
