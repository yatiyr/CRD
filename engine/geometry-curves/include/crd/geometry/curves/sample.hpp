#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Sampling + flattening. Phase 3.1.7 v10b (2026-05-19).
//
// Three sample modes + a convenience wrapper, every one generic over the
// curve kind. ALL sample modes call `evaluate(curve, t)` internally —
// D186 algorithm-definition contract enforced by construction. There are
// no algebraic shortcuts that bypass the evaluator.
//
// **D193 (planned for v10-close)** — Adaptive subdivision uses an explicit
// stack (NOT recursion). Stack push order is "right half FIRST, left half
// SECOND" so left is popped first → output samples land in monotonic-t
// order without sorting.
//
// **D194 (planned for v10-close)** — Curvature sampler computes the angle-
// threshold check via `dot(unit_tangent_a, unit_tangent_b)` versus
// `crd::math::deterministic::cos(max_angle_step)`. No `acos`. Cos
// comparison is monotonic + CPU↔GPU bit-portable (Cephes polynomial).
//
// **D195 (planned for v10-close)** — Closed-curve output convention: emit
// `n_samples` points covering `t ∈ [0, 1)` and mark the output Polyline
// `closed = true`. The implicit wrap from last sample back to first is
// the consumer's responsibility (matches Polyline storage from v10a).
// Open curves emit `n_samples + 1` points covering `t ∈ [0, 1]` inclusive.
//
// **D196 (planned for v10-close)** — Subdivision depth cap = 16 (max 65 537
// leaves per top-level segment). Hitting the cap is a soft event:
// `CRD_LOG_WARN` once + emit-what-we-have. Never asserts. Pathological
// inputs (infinitely-tight cusps) get a bounded-cost result.
//
// **D197 (planned for v10-close)** — `to_polyline(curve, alloc)` default
// tolerance = `crd::geometry::primitives::k_distance_epsilon<T>` (1e-5 for
// f32, 1e-12 for f64). Caller can use `sample_adaptive` directly to
// override.
//
// Scope: v10b is **3D-only**. 2D-peer samplers (`Polyline2`, `QuadBezier2`,
// etc.) ship when an authoring consumer asks; filed as `v10b-2d-sampling`.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/evaluator.hpp>
#include <crd/geometry/curves/polyline.hpp>
#include <crd/geometry/primitives/constants.hpp>
#include <crd/math/deterministic.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::curves
{

// ---------------------------------------------------------------------------
// Configuration constants. Pinned at v10-close (D196 / D197).
// ---------------------------------------------------------------------------

inline constexpr crd::u32 k_sample_max_subdivision_depth = 16U;

// Default tolerance bridge into the v1h constants. Returns 1e-6 for f32,
// 1e-12 for f64 (per `crd::geometry::primitives::k_distance_epsilon`).
template <crd::math::MathScalar T>
[[nodiscard]] constexpr T sample_default_tolerance() noexcept
{
    return crd::geometry::primitives::k_distance_epsilon<T>();
}

// ---------------------------------------------------------------------------
// sample_uniform — parameter-uniform sampling.
//
// Open curves emit `n_segments + 1` points covering t ∈ [0, 1] inclusive.
// Closed curves emit `n_segments` points covering t ∈ [0, 1).
// `n_segments` MUST be >= 1.
// ---------------------------------------------------------------------------

template <typename Curve>
[[nodiscard]] Polyline3<typename Curve::scalar_t> sample_uniform(
    const Curve&             curve,
    crd::u32                 n_segments,
    crd::memory::IAllocator* alloc) noexcept
{
    using T = typename Curve::scalar_t;
    CRD_ASSERT(n_segments >= 1U);

    Polyline3<T> out(alloc);
    out.closed = curve.closed;

    const crd::u32 n_points = curve.closed ? n_segments : (n_segments + 1U);
    out.points.reserve(n_points);

    const T step = static_cast<T>(1) / static_cast<T>(n_segments);
    for (crd::u32 i = 0U; i < n_points; ++i)
    {
        // Last open-curve sample is at t = 1.0 exactly (bit-equal to
        // evaluate(curve, 1.0)). For closed curves the t < 1.0 range
        // dodges the wrap-to-zero point.
        const T t = (i == n_segments) ? static_cast<T>(1) : (static_cast<T>(i) * step);
        out.points.push_back(evaluate(curve, t));
    }
    return out;
}

// ---------------------------------------------------------------------------
// sample_adaptive — explicit-stack chord-error subdivision.
//
// For each segment [t0, t1]:
//   1. Compute `p_mid_actual = evaluate(curve, (t0+t1)/2)`.
//   2. Compute `p_mid_lerp = (p0 + p1) / 2`.
//   3. If `|p_mid_actual - p_mid_lerp|^2 <= tolerance^2`: leaf — emit p1.
//   4. Else: push [tm, t1] then [t0, tm] (so left is popped first).
//
// `tolerance` MUST be > 0. Depth cap is `k_sample_max_subdivision_depth`
// (D196).
// ---------------------------------------------------------------------------

namespace detail
{

template <crd::math::MathScalar T>
struct SubdivisionEntry
{
    T        t0;
    T        t1;
    crd::u32 depth;
};

// Common subdivision driver. The predicate decides leaf vs split for each
// segment. Used by both sample_adaptive (chord-error) and
// sample_by_curvature (tangent-angle).
template <typename Curve, typename LeafPredicate>
Polyline3<typename Curve::scalar_t> subdivide_drive(
    const Curve&             curve,
    LeafPredicate            is_leaf,
    crd::memory::IAllocator* alloc) noexcept
{
    using T = typename Curve::scalar_t;

    Polyline3<T> out(alloc);
    out.closed = curve.closed;

    crd::containers::Array<SubdivisionEntry<T>> stack(alloc);
    stack.reserve(2U * k_sample_max_subdivision_depth);
    stack.push_back({static_cast<T>(0), static_cast<T>(1), 0U});

    // Always emit the t=0 starting point.
    out.points.push_back(evaluate(curve, static_cast<T>(0)));

    while (stack.size() > 0U)
    {
        const SubdivisionEntry<T> seg = stack[stack.size() - 1U];
        stack.pop_back();

        const T    tm = (seg.t0 + seg.t1) * static_cast<T>(0.5);
        const auto p0 = evaluate(curve, seg.t0);
        const auto p1 = evaluate(curve, seg.t1);
        const auto pm = evaluate(curve, tm);

        const bool depth_cap_hit = (seg.depth >= k_sample_max_subdivision_depth);
        const bool predicate_ok  = is_leaf(curve, seg.t0, seg.t1, tm, p0, p1, pm);

        if (depth_cap_hit || predicate_ok)
        {
            // Leaf: emit endpoint of this segment.
            // (D196) Depth-cap hit is silent — the emitted point is still
            // numerically correct at this t; the curve is just under-
            // resolved in that subregion. Pathological cusps get a
            // bounded-cost result, not an assert.
            out.points.push_back(p1);
        }
        else
        {
            // Push right half FIRST so left half is popped first
            // (D193 monotonic-t emission order).
            stack.push_back({tm, seg.t1, seg.depth + 1U});
            stack.push_back({seg.t0, tm, seg.depth + 1U});
        }
    }

    // For closed curves: last emitted sample is at t=1.0 which equals t=0.0
    // bit-exactly. Drop the duplicate per D195.
    if (curve.closed && out.points.size() >= 2U)
    {
        out.points.pop_back();
    }
    return out;
}

} // namespace detail

template <typename Curve>
[[nodiscard]] Polyline3<typename Curve::scalar_t> sample_adaptive(
    const Curve&                    curve,
    typename Curve::scalar_t        tolerance,
    crd::memory::IAllocator*        alloc) noexcept
{
    using T = typename Curve::scalar_t;
    CRD_ASSERT(tolerance > static_cast<T>(0));
    const T tol_sq = tolerance * tolerance;

    auto is_leaf = [tol_sq](const Curve& /*c*/,
                              T /*t0*/,
                              T /*t1*/,
                              T /*tm*/,
                              const crd::math::Vec3<T>& p0,
                              const crd::math::Vec3<T>& p1,
                              const crd::math::Vec3<T>& pm) noexcept -> bool {
        const auto lerp_mid = (p0 + p1) * static_cast<T>(0.5);
        const T    err_sq   = crd::math::length_squared(pm - lerp_mid);
        return err_sq <= tol_sq;
    };

    return detail::subdivide_drive(curve, is_leaf, alloc);
}

// ---------------------------------------------------------------------------
// sample_by_curvature — angle-uniform sampling.
//
// Subdivide while the angle between successive unit tangents exceeds
// `max_angle_step`. Uses `cos`-threshold (D194) instead of `acos` because:
//   (a) `cos` is monotonic on [0, π], so `dot >= cos_threshold` ↔ angle
//       <= max_angle_step.
//   (b) `crd::math::deterministic::cos` is Cephes-poly + CPU↔GPU portable.
//   (c) `acos` near 1.0 (small angles) loses precision; `cos` keeps it.
// ---------------------------------------------------------------------------

template <typename Curve>
[[nodiscard]] Polyline3<typename Curve::scalar_t> sample_by_curvature(
    const Curve&                    curve,
    typename Curve::scalar_t        max_angle_step,
    crd::memory::IAllocator*        alloc) noexcept
{
    using T = typename Curve::scalar_t;
    CRD_ASSERT(max_angle_step > static_cast<T>(0));
    const T cos_threshold = crd::math::deterministic::cos(max_angle_step);

    auto is_leaf = [cos_threshold](const Curve& c,
                                     T t0,
                                     T t1,
                                     T /*tm*/,
                                     const crd::math::Vec3<T>& /*p0*/,
                                     const crd::math::Vec3<T>& /*p1*/,
                                     const crd::math::Vec3<T>& /*pm*/) noexcept -> bool {
        const auto d0 = evaluate_derivative(c, t0);
        const auto d1 = evaluate_derivative(c, t1);
        const T    len_sq_0 = crd::math::length_squared(d0);
        const T    len_sq_1 = crd::math::length_squared(d1);
        // Degenerate tangents (length 0) — treat as leaf to avoid
        // division-by-zero. Caller's responsibility to validate curve
        // does not have stationary points if curvature sampling matters.
        if (len_sq_0 <= static_cast<T>(0) || len_sq_1 <= static_cast<T>(0))
        {
            return true;
        }
        const T dot_unit =
            crd::math::dot(d0, d1) / (crd::math::length(d0) * crd::math::length(d1));
        return dot_unit >= cos_threshold;
    };

    return detail::subdivide_drive(curve, is_leaf, alloc);
}

// ---------------------------------------------------------------------------
// to_polyline — convenience: `sample_adaptive` with default tolerance.
// ---------------------------------------------------------------------------

template <typename Curve>
[[nodiscard]] Polyline3<typename Curve::scalar_t> to_polyline(
    const Curve&             curve,
    crd::memory::IAllocator* alloc) noexcept
{
    using T = typename Curve::scalar_t;
    return sample_adaptive(curve, sample_default_tolerance<T>(), alloc);
}

} // namespace crd::geometry::curves
