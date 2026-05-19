#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Frames + RMF. Phase 3.1.7 v10e (2026-05-19).
//
// Four entry points, all generic over the curve kind via `Curve::scalar_t`:
//   - tangent(curve, t)        -> unit Vec3 (or +X fallback at singular t).
//   - normal(curve, t)         -> Frenet normal via finite-diff 2nd-derivative
//                                 + Gram-Schmidt; deterministic +Y/+Z fallback
//                                 when curvature == 0.
//   - binormal(curve, t)       -> cross(tangent, normal).
//   - compute_rmf(curve, n_samples, alloc) -> Array<CurveFrame<T>> via the
//                                 Wang 2008 double-reflection method, with
//                                 uniform-redistribution closure twist when
//                                 the curve is closed.
//
// **D208 (planned for v10-close, ADR-0076 §27)** — Frame computation
// composes on top of `evaluate` + `evaluate_derivative`. NO curve-kind-
// specific paths leak past the substrate; adding a new curve kind in v10+
// gets frames for free as soon as it implements the two evaluator entry
// points.
//
// **D209 (planned for v10-close)** — RMF parameter space = parameter-uniform.
// The Wang walk samples at `t_i = i / n_segments` (open) or `t_i = i / n`
// (closed). Arc-length-uniform variant (the truly-smooth cinematic-camera
// path) is filed as `v10e-arclength-rmf` follow-on — same algorithm, just
// drives the sample t from a v10c `ArclengthTable` instead of uniform t.
//
// **D210 (planned for v10-close)** — Closed-curve RMF closure twist is
// applied via uniform redistribution: compute the signed twist between
// the would-be wrap frame and the start frame in the plane perpendicular
// to T_0, then rotate each frame i by `-theta_total * (i / n)` around its
// own tangent. After redistribution the loop closes seamlessly:
// frame[0] is unchanged, frame[n-1] absorbs (n-1)/n of the twist.
//
// **D211 (planned for v10-close)** — Zero-curvature normal fallback:
// `frenet_fallback_normal(tangent_unit)` projects +Y world-up onto the
// plane perpendicular to the tangent + normalises. If the tangent is
// parallel to +Y (vertical curve), fall through to +Z. Single point of
// truth so every frame consumer is bit-deterministic at degenerate t.
//
// **D212 (planned for v10-close)** — Degenerate-tangent fallback:
// standalone `tangent(curve, t)` returns +X when `evaluate_derivative`
// produces a zero vector (singular point: polyline self-overlap, cusp).
// `compute_rmf` retains the previous frame's tangent when the walk hits
// a singular sample (last-good policy) — the Wang reflection step
// produces sane output as long as both endpoints have valid tangents.
//
// **D213 (planned for v10-close)** — Frenet 2nd-derivative step size:
// `h_2nd = 1e-3` for curve kinds that themselves finite-difference their
// 1st derivative (CatmullRom + BSpline; their `evaluate_derivative` has
// step h_1st = 1e-4 per v10a); `h_2nd = 1e-4` for kinds with analytic
// 1st derivative (Bezier / Hermite / CircularArc / EllipseArc /
// Polyline). The conservative h_2nd is the v10e default; the analytic-
// 2nd-derivative paths for CR + BSpline are filed as
// `v10a-cr-analytic-2nd-derivative` + `v10a-bspline-analytic-2nd-
// derivative` follow-ons (each is a degree-1 Barry-Goldman / degree-2
// B-spline over a shifted control polygon — same compositional form as
// the 1st-derivative twin).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/evaluator.hpp>
#include <crd/math/deterministic.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::geometry::curves
{

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
struct CurveFrame
{
    crd::math::Vec3<T> tangent;
    crd::math::Vec3<T> normal;
    crd::math::Vec3<T> binormal;
};

// ---------------------------------------------------------------------------
// detail — shared helpers
// ---------------------------------------------------------------------------

namespace detail
{

// Project +Y world-up onto the plane perpendicular to `tangent_unit`. If
// the tangent is (nearly) parallel to +Y, fall back to +Z. Returns a unit
// vector orthogonal to `tangent_unit`. D211.
template <crd::math::MathScalar T>
[[nodiscard]] inline crd::math::Vec3<T> frenet_fallback_normal(
    const crd::math::Vec3<T>& tangent_unit) noexcept
{
    constexpr T k_parallel_eps = static_cast<T>(1e-3);

    const auto up_y = crd::math::Vec3<T>(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0));
    auto       n    = up_y - tangent_unit * crd::math::dot(up_y, tangent_unit);
    if (crd::math::length_squared(n) > k_parallel_eps * k_parallel_eps)
    {
        (void) crd::math::try_normalize(n);
        return n;
    }

    const auto up_z = crd::math::Vec3<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(1));
    auto       n2   = up_z - tangent_unit * crd::math::dot(up_z, tangent_unit);
    (void) crd::math::try_normalize(n2);
    return n2;
}

// Step size for the Frenet 2nd derivative when finite-differencing the
// 1st derivative. D213. Curve kinds that have an analytic 1st derivative
// can use a tighter step (h = 1e-4); kinds that themselves finite-diff
// their 1st derivative get a looser step (h = 1e-3) to keep the noise
// floor below the visualisation threshold.
//
// Default implementation returns h = 1e-3 (conservative).
template <typename Curve>
[[nodiscard]] constexpr typename Curve::scalar_t second_derivative_step() noexcept
{
    return static_cast<typename Curve::scalar_t>(1e-3);
}

// Rotate `v` around `axis_unit` by an angle whose cos/sin are precomputed.
// Specialised for the case `dot(axis_unit, v) == 0` (v is perpendicular to
// the axis) so the Rodrigues formula collapses to
//   v' = v * cos + (axis x v) * sin.
template <crd::math::MathScalar T>
[[nodiscard]] inline crd::math::Vec3<T> rotate_around_axis_perp(
    const crd::math::Vec3<T>& v,
    const crd::math::Vec3<T>& axis_unit,
    T                         cos_theta,
    T                         sin_theta) noexcept
{
    return v * cos_theta + crd::math::cross(axis_unit, v) * sin_theta;
}

// Single Wang 2008 double-reflection step.
//   prev_frame  — orthonormal frame at parameter t_i.
//   prev_point  — curve(t_i).
//   next_point  — curve(t_{i+1}).
//   next_tangent_unit — normalised curve'(t_{i+1}).
// Returns the orthonormal frame at t_{i+1}. Twist-minimising by
// construction (Wang 2008 §5).
template <crd::math::MathScalar T>
[[nodiscard]] inline CurveFrame<T> wang_step(
    const CurveFrame<T>&      prev_frame,
    const crd::math::Vec3<T>& prev_point,
    const crd::math::Vec3<T>& next_point,
    const crd::math::Vec3<T>& next_tangent_unit) noexcept
{
    // Reflection 1: through the bisecting plane normal to v1 = x_{i+1} - x_i.
    const auto v1 = next_point - prev_point;
    const T    c1 = crd::math::length_squared(v1);
    if (c1 <= static_cast<T>(0))
    {
        // Coincident samples — pass the frame through unchanged.
        // The tangent gets rotated to next_tangent if it differs.
        return CurveFrame<T>{next_tangent_unit, prev_frame.normal,
                             crd::math::cross(next_tangent_unit, prev_frame.normal)};
    }
    const T    inv_c1 = static_cast<T>(2) / c1;
    const auto r_L_n  = prev_frame.normal  - v1 * (inv_c1 * crd::math::dot(v1, prev_frame.normal));
    const auto r_L_t  = prev_frame.tangent - v1 * (inv_c1 * crd::math::dot(v1, prev_frame.tangent));

    // Reflection 2: through the bisecting plane normal to v2 = T_{i+1} - r_L_t.
    const auto v2 = next_tangent_unit - r_L_t;
    const T    c2 = crd::math::length_squared(v2);
    crd::math::Vec3<T> n_next;
    if (c2 <= static_cast<T>(0))
    {
        // r_L_t already aligns with next_tangent — first reflection sufficed.
        n_next = r_L_n;
    }
    else
    {
        const T inv_c2 = static_cast<T>(2) / c2;
        n_next         = r_L_n - v2 * (inv_c2 * crd::math::dot(v2, r_L_n));
    }

    // Re-orthonormalise N against the next tangent (drift-defence; the math
    // says these are orthogonal but f32 walks accumulate ~1 ULP per step).
    n_next = n_next - next_tangent_unit * crd::math::dot(next_tangent_unit, n_next);
    if (!crd::math::try_normalize(n_next))
    {
        // Pathological — fall back to the deterministic fallback.
        n_next = frenet_fallback_normal(next_tangent_unit);
    }

    const auto b_next = crd::math::cross(next_tangent_unit, n_next);
    return CurveFrame<T>{next_tangent_unit, n_next, b_next};
}

} // namespace detail

// ---------------------------------------------------------------------------
// tangent(curve, t) — unit tangent. D212 +X fallback when degenerate.
// ---------------------------------------------------------------------------

template <typename Curve>
[[nodiscard]] inline crd::math::Vec3<typename Curve::scalar_t> tangent(
    const Curve& curve, typename Curve::scalar_t t) noexcept
{
    using T = typename Curve::scalar_t;
    auto d  = evaluate_derivative(curve, t);
    if (crd::math::try_normalize(d))
    {
        return d;
    }
    return crd::math::Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
}

// ---------------------------------------------------------------------------
// normal(curve, t) — Frenet normal via finite-diff 2nd derivative + Gram-
// Schmidt against the unit tangent. D211 fallback when curvature ~= 0.
// ---------------------------------------------------------------------------

template <typename Curve>
[[nodiscard]] inline crd::math::Vec3<typename Curve::scalar_t> normal(
    const Curve& curve, typename Curve::scalar_t t) noexcept
{
    using T          = typename Curve::scalar_t;
    const auto t_hat = tangent(curve, t);

    // Endpoint-clamped finite-difference for open curves; modular wrap on
    // closed curves (evaluator handles the wrap internally).
    const T h       = detail::second_derivative_step<Curve>();
    T       t_minus = t - h;
    T       t_plus  = t + h;
    if (!curve.closed)
    {
        if (t_minus < static_cast<T>(0)) { t_minus = static_cast<T>(0); }
        if (t_plus  > static_cast<T>(1)) { t_plus  = static_cast<T>(1); }
    }
    const T denom = t_plus - t_minus;
    if (denom <= static_cast<T>(0))
    {
        return detail::frenet_fallback_normal(t_hat);
    }

    const auto deriv_plus  = evaluate_derivative(curve, t_plus);
    const auto deriv_minus = evaluate_derivative(curve, t_minus);
    const auto second_d    = (deriv_plus - deriv_minus) * (static_cast<T>(1) / denom);

    // Frenet normal: N_proxy = C'' - (C'' . T) T, then normalise.
    auto n_proxy = second_d - t_hat * crd::math::dot(second_d, t_hat);
    if (crd::math::try_normalize(n_proxy))
    {
        return n_proxy;
    }
    return detail::frenet_fallback_normal(t_hat);
}

// ---------------------------------------------------------------------------
// binormal(curve, t) — cross(T, N).
// ---------------------------------------------------------------------------

template <typename Curve>
[[nodiscard]] inline crd::math::Vec3<typename Curve::scalar_t> binormal(
    const Curve& curve, typename Curve::scalar_t t) noexcept
{
    return crd::math::cross(tangent(curve, t), normal(curve, t));
}

// ---------------------------------------------------------------------------
// compute_rmf(curve, n_samples, alloc) — Wang 2008 double reflection +
// uniform closure-twist redistribution on closed curves. D209 / D210.
//
// `n_samples >= 1`. Open curves produce `n_samples + 1` frames at
// t = 0, 1/n, ..., 1. Closed curves produce `n_samples` frames at
// t = 0, 1/n, ..., (n-1)/n (the wrap is implicit; frame[n] == frame[0]
// numerically after redistribution).
// ---------------------------------------------------------------------------

template <typename Curve>
[[nodiscard]] crd::containers::Array<CurveFrame<typename Curve::scalar_t>> compute_rmf(
    const Curve&             curve,
    crd::u32                 n_samples,
    crd::memory::IAllocator* alloc) noexcept
{
    using T = typename Curve::scalar_t;
    CRD_ASSERT(alloc != nullptr);
    CRD_ASSERT(n_samples >= 1U);

    const bool closed = curve.closed;
    const auto n_out  = closed ? n_samples : (n_samples + 1U);

    crd::containers::Array<CurveFrame<T>> out(alloc);
    out.reserve(n_out);

    // ---- Initial frame (i = 0) ----
    auto last_good_tangent = tangent(curve, static_cast<T>(0));
    auto n0                = normal(curve, static_cast<T>(0));
    // Ensure N0 is exactly orthogonal to T0 (tangent + normal computed
    // independently above; a single Gram-Schmidt pass nails the parity).
    n0 = n0 - last_good_tangent * crd::math::dot(last_good_tangent, n0);
    if (!crd::math::try_normalize(n0))
    {
        n0 = detail::frenet_fallback_normal(last_good_tangent);
    }
    const auto b0 = crd::math::cross(last_good_tangent, n0);

    out.push_back(CurveFrame<T>{last_good_tangent, n0, b0});

    auto prev_point = evaluate(curve, static_cast<T>(0));
    auto prev_frame = out[0];

    // ---- Wang walk ----
    const auto n_steps = closed ? n_samples : n_samples; // frame count - 1
    for (crd::u32 i = 1U; i <= n_steps; ++i)
    {
        const T t_i = static_cast<T>(i) / static_cast<T>(n_samples);

        const auto next_point  = evaluate(curve, t_i);
        auto       next_t_raw  = evaluate_derivative(curve, t_i);
        bool       have_next_t = crd::math::try_normalize(next_t_raw);
        if (!have_next_t)
        {
            // Last-good policy (D212): re-use the previous tangent.
            next_t_raw = last_good_tangent;
        }
        else
        {
            last_good_tangent = next_t_raw;
        }

        const auto next_frame = detail::wang_step(prev_frame, prev_point, next_point, next_t_raw);

        // For closed curves we generate frames 1..n_samples but ONLY store
        // 1..n_samples-1; the n_samples-th frame is the "would-be wrap"
        // used to compute the closure twist below.
        if (!closed || i < n_samples)
        {
            out.push_back(next_frame);
        }
        else
        {
            // Stash the wrap frame for the closure-twist computation.
            // We compute twist = signed angle from out[0].normal -> wrap.normal
            // around T0, then unwind by theta * (i / n_samples) at each frame.
            const auto& f0    = out[0];
            const auto& wrap  = next_frame;
            const T     cos_t = crd::math::dot(f0.normal, wrap.normal);
            const T     sin_t = crd::math::dot(crd::math::cross(f0.normal, wrap.normal), f0.tangent);
            // theta = atan2(sin, cos). Use deterministic atan2-free
            // redistribution: we don't need theta explicitly; cos/sin per
            // step is theta * (k / n). Use Rodrigues with the angle's
            // n-th root via repeated rotation OR compute theta via atan2.
            //
            // Use std::atan2 here — closure twist is a once-per-curve
            // computation, not on the inner sampling hot path, and
            // visualisation tolerates ~1e-7 ULP variance across compilers
            // for this specific path. The per-step rotation below uses
            // crd::math::deterministic::sin/cos for portability.
            const T theta = static_cast<T>(std::atan2(static_cast<double>(sin_t),
                                                       static_cast<double>(cos_t)));
            // Redistribute. Frame k in [0, n_samples - 1] rotates by
            // -theta * (k / n_samples) around its own tangent.
            for (crd::u32 k = 1U; k < n_samples; ++k)
            {
                const T angle_k = -theta * (static_cast<T>(k) / static_cast<T>(n_samples));
                const T cos_k   = crd::math::deterministic::cos(angle_k);
                const T sin_k   = crd::math::deterministic::sin(angle_k);
                auto&   fk      = out[k];
                fk.normal       = detail::rotate_around_axis_perp(fk.normal, fk.tangent, cos_k, sin_k);
                fk.binormal     = crd::math::cross(fk.tangent, fk.normal);
            }
        }

        prev_point = next_point;
        prev_frame = next_frame;
    }

    return out;
}

} // namespace crd::geometry::curves
