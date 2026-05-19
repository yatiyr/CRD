#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Parametric evaluator. Phase 3.1.7 v10a (2026-05-19).
//
// **D186 (planned for v10-close, ADR-0076 §27)** — `evaluate(curve, t)` IS
// the algorithm definition. Every downstream query (sampling, arc-length,
// closest-point, AABB) eventually calls `evaluate` or `evaluate_derivative`.
// Mirrors:
//   - D134 `crd-geometry-bvh-gpu`: CPU IS the algorithm definition.
//   - D157 `crd-geometry-bvh-gpu`: `build_lbvh_cpu` IS the LBVH algorithm.
//   - D170 `crd-geometry-shader-helpers`: `evaluate<T>(ir, p)` IS the SDF.
//
// Numerical-stability discipline:
//   - **de Casteljau** for Bezier (NOT Bernstein polynomial expansion —
//     de Casteljau is unconditionally-stable repeated linear interp).
//   - **Hermite basis** for `CubicHermite3` — direct evaluation of the
//     four cubic Hermite basis functions h00 / h10 / h01 / h11.
//   - **Catmull-Rom centripetal** uses √chord-length knot spacing per
//     Yuksel-Schaefer-Keyser 2011; uniform uses unit spacing.
//   - **Cox-de-Boor** for B-spline (degree-3, clamped open by default).
//   - **`crd::math::deterministic::sin/cos`** for circular + elliptic arcs
//     — Cephes-polynomial port, same one v9e ships into GLSL/HLSL
//     preludes. CPU↔GPU bit-portable across compilers.
//
// Closed-curve handling: `t_wrap(t, closed)` applies modular reduction at
// the parameter boundary when `closed == true`. `evaluate(curve, 1.0)`
// is bit-equal to `evaluate(curve, 0.0)` for closed curves.
// ---------------------------------------------------------------------------

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/arc.hpp>
#include <crd/geometry/curves/bezier.hpp>
#include <crd/geometry/curves/bspline.hpp>
#include <crd/geometry/curves/catmull_rom.hpp>
#include <crd/geometry/curves/hermite.hpp>
#include <crd/geometry/curves/polyline.hpp>
#include <crd/geometry/curves/types.hpp>
#include <crd/math/deterministic.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <cmath>

namespace crd::geometry::curves
{

// ---------------------------------------------------------------------------
// Parameter wrap for closed curves. t is normalised to [0, 1] for the
// duration of the curve. The "closed wrap" sends t=1 to t=0 bit-exactly
// when the closed flag is set.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
[[nodiscard]] constexpr T t_wrap(T t, bool closed) noexcept
{
    if (!closed) { return t; }
    // Modular wrap. Branchless on the common path (0 <= t <= 1).
    // For t outside [0, 1] we do explicit floor-based modular reduction.
    if (t >= static_cast<T>(0) && t < static_cast<T>(1)) { return t; }
    if (t == static_cast<T>(1)) { return static_cast<T>(0); }
    // Generic path: t mod 1.
    const T floor_t = static_cast<T>(static_cast<crd::i64>(t < static_cast<T>(0) ? t - static_cast<T>(1) : t));
    return t - floor_t;
}

// ---------------------------------------------------------------------------
// Polyline3 — segment interpolation.
// ---------------------------------------------------------------------------

// Result kind: `Vec3<T>`. `t` is the global parameter in [0, 1] spanning the
// entire chain. Segment i = [points[i], points[i+1]] covers the t-slice
// [i / N, (i+1) / N] where N = num_segments.
template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> evaluate(const Polyline3View<T>& curve, T t) noexcept
{
    CRD_ASSERT(curve.points.size() >= 2U);
    const T t_eff = t_wrap(t, curve.closed);

    const auto n_pts  = static_cast<crd::u32>(curve.points.size());
    const auto n_segs = curve.closed ? n_pts : (n_pts - 1U);
    if (n_segs == 0U) { return curve.points[0]; }

    // Clamp to [0, 1] for open curves; closed curves already wrapped above.
    const T t_clamped = t_eff <= static_cast<T>(0)   ? static_cast<T>(0)
                        : t_eff >= static_cast<T>(1) ? static_cast<T>(1)
                                                     : t_eff;
    const T scaled = t_clamped * static_cast<T>(n_segs);
    auto    i      = static_cast<crd::u32>(scaled);
    if (i >= n_segs) { i = n_segs - 1U; }
    const T u = scaled - static_cast<T>(i);

    const auto& a = curve.points[i];
    const auto& b = curve.points[(i + 1U) % n_pts];
    return a + (b - a) * u;
}

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> evaluate_derivative(const Polyline3View<T>& curve, T t) noexcept
{
    CRD_ASSERT(curve.points.size() >= 2U);
    const T t_eff = t_wrap(t, curve.closed);

    const auto n_pts  = static_cast<crd::u32>(curve.points.size());
    const auto n_segs = curve.closed ? n_pts : (n_pts - 1U);
    if (n_segs == 0U) { return crd::math::Vec3<T>{}; }

    const T t_clamped = t_eff <= static_cast<T>(0)   ? static_cast<T>(0)
                        : t_eff >= static_cast<T>(1) ? static_cast<T>(1)
                                                     : t_eff;
    const T scaled = t_clamped * static_cast<T>(n_segs);
    auto    i      = static_cast<crd::u32>(scaled);
    if (i >= n_segs) { i = n_segs - 1U; }

    const auto& a = curve.points[i];
    const auto& b = curve.points[(i + 1U) % n_pts];
    // Chord direction scaled by segments-per-unit-t (= n_segs) since dt/du
    // = 1 / n_segs and we return d(position)/d(t) which is (b-a) * n_segs.
    return (b - a) * static_cast<T>(n_segs);
}

// ---------------------------------------------------------------------------
// QuadBezier3 — de Casteljau.
//   B(t) = lerp(lerp(P0, P1, t), lerp(P1, P2, t), t)
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> evaluate(const QuadBezier3<T>& curve, T t) noexcept
{
    const auto q0 = curve.p0 + (curve.p1 - curve.p0) * t;
    const auto q1 = curve.p1 + (curve.p2 - curve.p1) * t;
    return q0 + (q1 - q0) * t;
}

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> evaluate_derivative(const QuadBezier3<T>& curve, T t) noexcept
{
    // Derivative of a quadratic Bezier is a linear Bezier:
    //   B'(t) = 2 * lerp(P1 - P0, P2 - P1, t)
    const auto d0 = (curve.p1 - curve.p0);
    const auto d1 = (curve.p2 - curve.p1);
    return (d0 + (d1 - d0) * t) * static_cast<T>(2);
}

// ---------------------------------------------------------------------------
// CubicBezier3 — de Casteljau.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> evaluate(const CubicBezier3<T>& curve, T t) noexcept
{
    const auto q0 = curve.p0 + (curve.p1 - curve.p0) * t;
    const auto q1 = curve.p1 + (curve.p2 - curve.p1) * t;
    const auto q2 = curve.p2 + (curve.p3 - curve.p2) * t;
    const auto r0 = q0 + (q1 - q0) * t;
    const auto r1 = q1 + (q2 - q1) * t;
    return r0 + (r1 - r0) * t;
}

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> evaluate_derivative(const CubicBezier3<T>& curve, T t) noexcept
{
    // Derivative of a cubic Bezier is a quadratic Bezier with control
    // points {3(P1-P0), 3(P2-P1), 3(P3-P2)}.
    const auto d0 = (curve.p1 - curve.p0) * static_cast<T>(3);
    const auto d1 = (curve.p2 - curve.p1) * static_cast<T>(3);
    const auto d2 = (curve.p3 - curve.p2) * static_cast<T>(3);
    const auto q0 = d0 + (d1 - d0) * t;
    const auto q1 = d1 + (d2 - d1) * t;
    return q0 + (q1 - q0) * t;
}

// ---------------------------------------------------------------------------
// CubicHermite3 — Hermite basis (h00, h10, h01, h11).
//   H(t) = h00(t) * P0 + h10(t) * T0 + h01(t) * P1 + h11(t) * T1
// with:
//   h00(t) =  2t³ - 3t² + 1
//   h10(t) =    t³ - 2t² + t
//   h01(t) = -2t³ + 3t²
//   h11(t) =    t³ -   t²
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> evaluate(const CubicHermite3<T>& curve, T t) noexcept
{
    const T t2 = t * t;
    const T t3 = t2 * t;
    const T h00 = static_cast<T>(2) * t3 - static_cast<T>(3) * t2 + static_cast<T>(1);
    const T h10 = t3 - static_cast<T>(2) * t2 + t;
    const T h01 = static_cast<T>(-2) * t3 + static_cast<T>(3) * t2;
    const T h11 = t3 - t2;
    return curve.p0 * h00 + curve.t0 * h10 + curve.p1 * h01 + curve.t1 * h11;
}

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> evaluate_derivative(const CubicHermite3<T>& curve, T t) noexcept
{
    // h00'(t) =  6t² - 6t
    // h10'(t) =  3t² - 4t + 1
    // h01'(t) = -6t² + 6t
    // h11'(t) =  3t² - 2t
    const T t2  = t * t;
    const T h00 = static_cast<T>(6) * t2 - static_cast<T>(6) * t;
    const T h10 = static_cast<T>(3) * t2 - static_cast<T>(4) * t + static_cast<T>(1);
    const T h01 = static_cast<T>(-6) * t2 + static_cast<T>(6) * t;
    const T h11 = static_cast<T>(3) * t2 - static_cast<T>(2) * t;
    return curve.p0 * h00 + curve.t0 * h10 + curve.p1 * h01 + curve.t1 * h11;
}

// ---------------------------------------------------------------------------
// CircularArc3 — analytic parametric.
//   C(t) = center + cos(theta(t)) * axis_u * radius + sin(theta(t)) * axis_v * radius
//   theta(t) = t * sweep_radians
// Uses `crd::math::deterministic::sin/cos` for CPU↔GPU portability.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
[[nodiscard]] crd::math::Vec3<T> evaluate(const CircularArc3<T>& curve, T t) noexcept
{
    const T t_eff = t_wrap(t, curve.closed);
    const T theta = t_eff * curve.sweep_radians;
    const T c     = crd::math::deterministic::cos(theta);
    const T s     = crd::math::deterministic::sin(theta);
    return curve.center + (curve.axis_u * c + curve.axis_v * s) * curve.radius;
}

template <crd::math::MathScalar T>
[[nodiscard]] crd::math::Vec3<T> evaluate_derivative(const CircularArc3<T>& curve, T t) noexcept
{
    const T t_eff = t_wrap(t, curve.closed);
    const T theta = t_eff * curve.sweep_radians;
    const T c     = crd::math::deterministic::cos(theta);
    const T s     = crd::math::deterministic::sin(theta);
    // d/dt = sweep_radians * radius * (-sin(theta) * u + cos(theta) * v)
    return (curve.axis_u * (-s) + curve.axis_v * c) * (curve.sweep_radians * curve.radius);
}

// ---------------------------------------------------------------------------
// EllipseArc3 — same form, radius_u and radius_v.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
[[nodiscard]] crd::math::Vec3<T> evaluate(const EllipseArc3<T>& curve, T t) noexcept
{
    const T t_eff = t_wrap(t, curve.closed);
    const T theta = t_eff * curve.sweep_radians;
    const T c     = crd::math::deterministic::cos(theta);
    const T s     = crd::math::deterministic::sin(theta);
    return curve.center + curve.axis_u * (c * curve.radius_u) + curve.axis_v * (s * curve.radius_v);
}

template <crd::math::MathScalar T>
[[nodiscard]] crd::math::Vec3<T> evaluate_derivative(const EllipseArc3<T>& curve, T t) noexcept
{
    const T t_eff = t_wrap(t, curve.closed);
    const T theta = t_eff * curve.sweep_radians;
    const T c     = crd::math::deterministic::cos(theta);
    const T s     = crd::math::deterministic::sin(theta);
    return (curve.axis_u * (-s * curve.radius_u) + curve.axis_v * (c * curve.radius_v)) * curve.sweep_radians;
}

// ---------------------------------------------------------------------------
// CatmullRom3 — centripetal Yuksel 2011 / uniform variant.
//
// For a segment between P[i] and P[i+1] (using neighbours P[i-1] and
// P[i+2]), we compute knot values t0..t3 from the parameterisation:
//   - Uniform: t_k = k.
//   - Centripetal: t_{k+1} = t_k + |P_{k+1} - P_k|^0.5
//   - (Chordal alpha=1 is NOT exposed in v10a; same code path easy to add.)
//
// Then in segment-local parameter u in [0, 1] we map to global tt:
//   tt = t1 + (t2 - t1) * u
// and evaluate via the Barry-Goldman 1988 nested-linear-interp form
// (numerically equivalent to direct cubic-poly evaluation; no matrix
// solve, no transcendentals).
//
// Boundary segments use phantom points via reflection:
//   P_{-1} = 2*P_0 - P_1
//   P_{n}  = 2*P_{n-1} - P_{n-2}
//
// Closed-curve: neighbour lookup wraps mod n.
// ---------------------------------------------------------------------------

namespace detail
{

// Compute the 4-knot vector (t0, t1, t2, t3) for a Catmull-Rom segment
// covering points (p0, p1, p2, p3) under the chosen parameterisation.
template <crd::math::MathScalar T>
constexpr void cr_knots(const crd::math::Vec3<T>& p0,
                         const crd::math::Vec3<T>& p1,
                         const crd::math::Vec3<T>& p2,
                         const crd::math::Vec3<T>& p3,
                         CatmullRomParam           param,
                         T&                        t0_out,
                         T&                        t1_out,
                         T&                        t2_out,
                         T&                        t3_out) noexcept
{
    if (param == CatmullRomParam::Uniform)
    {
        t0_out = static_cast<T>(0);
        t1_out = static_cast<T>(1);
        t2_out = static_cast<T>(2);
        t3_out = static_cast<T>(3);
        return;
    }

    // Centripetal: t_{k+1} = t_k + |P_{k+1} - P_k|^0.5. Yuksel 2011.
    // chord = sqrt(len_sq); chord^0.5 = sqrt(chord) = sqrt(sqrt(len_sq)).
    // We use std::sqrt (IEEE 754 correctly-rounded — deterministic per
    // ADR-0063 build flags).
    const T len_sq_01 = crd::math::length_squared(p1 - p0);
    const T len_sq_12 = crd::math::length_squared(p2 - p1);
    const T len_sq_23 = crd::math::length_squared(p3 - p2);
    const T e01       = static_cast<T>(std::sqrt(std::sqrt(static_cast<double>(len_sq_01))));
    const T e12       = static_cast<T>(std::sqrt(std::sqrt(static_cast<double>(len_sq_12))));
    const T e23       = static_cast<T>(std::sqrt(std::sqrt(static_cast<double>(len_sq_23))));
    t0_out            = static_cast<T>(0);
    t1_out            = t0_out + e01;
    t2_out            = t1_out + e12;
    t3_out            = t2_out + e23;
}

// Barry-Goldman nested lerp.
template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> cr_segment_eval(const crd::math::Vec3<T>& p0,
                                                            const crd::math::Vec3<T>& p1,
                                                            const crd::math::Vec3<T>& p2,
                                                            const crd::math::Vec3<T>& p3,
                                                            T                         t0,
                                                            T                         t1,
                                                            T                         t2,
                                                            T                         t3,
                                                            T                         tt) noexcept
{
    // Safe division — t0..t3 are all distinct so divisors are nonzero by
    // construction (centripetal builds distinct knots from finite chord
    // lengths; uniform is always strictly increasing).
    const auto A1 = p0 * ((t1 - tt) / (t1 - t0)) + p1 * ((tt - t0) / (t1 - t0));
    const auto A2 = p1 * ((t2 - tt) / (t2 - t1)) + p2 * ((tt - t1) / (t2 - t1));
    const auto A3 = p2 * ((t3 - tt) / (t3 - t2)) + p3 * ((tt - t2) / (t3 - t2));
    const auto B1 = A1 * ((t2 - tt) / (t2 - t0)) + A2 * ((tt - t0) / (t2 - t0));
    const auto B2 = A2 * ((t3 - tt) / (t3 - t1)) + A3 * ((tt - t1) / (t3 - t1));
    return B1 * ((t2 - tt) / (t2 - t1)) + B2 * ((tt - t1) / (t2 - t1));
}

// Fetch (p0, p1, p2, p3) for segment i of a Catmull-Rom spline, handling
// boundary reflection on open curves and modular wrap on closed curves.
template <crd::math::MathScalar T>
constexpr void cr_segment_points(const CatmullRom3<T>& curve,
                                  crd::u32              segment_i,
                                  crd::math::Vec3<T>&   p0_out,
                                  crd::math::Vec3<T>&   p1_out,
                                  crd::math::Vec3<T>&   p2_out,
                                  crd::math::Vec3<T>&   p3_out) noexcept
{
    const auto n = static_cast<crd::u32>(curve.points.size());
    if (curve.closed)
    {
        p0_out = curve.points[(segment_i + n - 1U) % n];
        p1_out = curve.points[segment_i % n];
        p2_out = curve.points[(segment_i + 1U) % n];
        p3_out = curve.points[(segment_i + 2U) % n];
    }
    else
    {
        // Open: phantom-point reflection at the boundaries.
        const auto& a = curve.points[segment_i];
        const auto& b = curve.points[segment_i + 1U];
        p1_out        = a;
        p2_out        = b;
        if (segment_i == 0U)
        {
            // P_{-1} = 2*P_0 - P_1
            p0_out = a + (a - b);
        }
        else
        {
            p0_out = curve.points[segment_i - 1U];
        }
        if (segment_i + 2U >= n)
        {
            // P_{n} = 2*P_{n-1} - P_{n-2}
            p3_out = b + (b - a);
        }
        else
        {
            p3_out = curve.points[segment_i + 2U];
        }
    }
}

} // namespace detail

template <crd::math::MathScalar T>
[[nodiscard]] crd::math::Vec3<T> evaluate(const CatmullRom3<T>& curve, T t) noexcept
{
    CRD_ASSERT(curve.points.size() >= 2U);
    const T t_eff = t_wrap(t, curve.closed);

    const auto n_pts  = static_cast<crd::u32>(curve.points.size());
    const auto n_segs = curve.closed ? n_pts : (n_pts - 1U);
    if (n_segs == 0U) { return curve.points[0]; }

    const T t_clamped = t_eff <= static_cast<T>(0)   ? static_cast<T>(0)
                        : t_eff >= static_cast<T>(1) ? static_cast<T>(1)
                                                     : t_eff;
    const T scaled = t_clamped * static_cast<T>(n_segs);
    auto    i      = static_cast<crd::u32>(scaled);
    if (i >= n_segs) { i = n_segs - 1U; }
    const T u = scaled - static_cast<T>(i);

    crd::math::Vec3<T> p0, p1, p2, p3;
    detail::cr_segment_points(curve, i, p0, p1, p2, p3);

    T t0, t1, t2, t3;
    detail::cr_knots(p0, p1, p2, p3, curve.param, t0, t1, t2, t3);
    const T tt = t1 + (t2 - t1) * u;
    return detail::cr_segment_eval(p0, p1, p2, p3, t0, t1, t2, t3, tt);
}

template <crd::math::MathScalar T>
[[nodiscard]] crd::math::Vec3<T> evaluate_derivative(const CatmullRom3<T>& curve, T t) noexcept
{
    // Numerical derivative via central finite-difference. Simple + works for
    // both parameterisations + closed curves. v10b/c/d may want an analytic
    // form for perf — filed as `v10a-cr-analytic-derivative` follow-on. The
    // finite-difference form is `evaluate` cost × 2; closed-form is ~1.2×
    // (same Barry-Goldman with different basis coefficients).
    const T h = static_cast<T>(1e-4);
    const auto p_minus = evaluate(curve, t - h);
    const auto p_plus  = evaluate(curve, t + h);
    return (p_plus - p_minus) * (static_cast<T>(1) / (static_cast<T>(2) * h));
}

// ---------------------------------------------------------------------------
// BSpline3 — Cox-de-Boor recursion, degree 3.
//
// Standard formulation:
//   N_{i, 0}(t) = 1 if knots[i] <= t < knots[i+1], else 0
//   N_{i, p}(t) = (t - knots[i]) / (knots[i+p] - knots[i]) * N_{i, p-1}(t)
//               + (knots[i+p+1] - t) / (knots[i+p+1] - knots[i+1]) * N_{i+1, p-1}(t)
//
// Curve(t) = sum_i N_{i, p}(t) * P_i
//
// For degree 3 (k_degree = 3), only 4 basis functions are nonzero at any
// given t. We compute them iteratively (no recursion) for stability.
// ---------------------------------------------------------------------------

namespace detail
{

// Find the knot span: returns the index `k` such that knots[k] <= t < knots[k+1].
// Handles the clamped-endpoint case (t == knots[last]) by returning the last
// valid span.
template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::u32 bspline_find_span(crd::containers::ConstSpan<T> knots, crd::u32 n_control, T t) noexcept
{
    constexpr crd::u32 degree = 3U;
    // Clamped-endpoint convention: at t == knots[n_control], return n_control - 1
    // so we get the last interior span.
    if (t >= knots[n_control]) { return n_control - 1U; }
    if (t <= knots[degree]) { return degree; }

    // Binary search in [degree, n_control].
    crd::u32 lo = degree;
    crd::u32 hi = n_control;
    while (lo + 1U < hi)
    {
        const crd::u32 mid = (lo + hi) / 2U;
        if (t < knots[mid]) { hi = mid; }
        else { lo = mid; }
    }
    return lo;
}

// Compute the 4 nonzero degree-3 basis function values at parameter `t` in
// span `k`. Output written to `basis[0..3]` corresponding to
//   N_{k-3, 3}, N_{k-2, 3}, N_{k-1, 3}, N_{k, 3}.
template <crd::math::MathScalar T>
constexpr void bspline_basis_d3(crd::containers::ConstSpan<T> knots, crd::u32 k, T t, T (&basis)[4U]) noexcept
{
    // Iterative Cox-de-Boor (Piegl & Tiller "The NURBS Book", Algorithm A2.2)
    // specialised to degree 3.
    T left[4U]  = {T{}, T{}, T{}, T{}};
    T right[4U] = {T{}, T{}, T{}, T{}};
    basis[0]    = static_cast<T>(1);

    for (crd::u32 j = 1U; j <= 3U; ++j)
    {
        left[j]  = t - knots[k + 1U - j];
        right[j] = knots[k + j] - t;
        T saved  = T{};
        for (crd::u32 r = 0U; r < j; ++r)
        {
            const T temp = basis[r] / (right[r + 1U] + left[j - r]);
            basis[r]     = saved + right[r + 1U] * temp;
            saved        = left[j - r] * temp;
        }
        basis[j] = saved;
    }
}

} // namespace detail

template <crd::math::MathScalar T>
[[nodiscard]] crd::math::Vec3<T> evaluate(const BSpline3<T>& curve, T t) noexcept
{
    constexpr crd::u32 degree    = 3U;
    const auto         n_control = static_cast<crd::u32>(curve.points.size());
    CRD_ASSERT(n_control >= degree + 1U);
    CRD_ASSERT(static_cast<crd::u32>(curve.knots.size()) == n_control + degree + 1U);

    // Map t in [0, 1] to the parametric range [knots[degree], knots[n_control]].
    const T t_eff = t_wrap(t, curve.closed);
    const T t_lo  = curve.knots[degree];
    const T t_hi  = curve.knots[n_control];
    const T t_clamped = t_eff <= static_cast<T>(0)   ? static_cast<T>(0)
                        : t_eff >= static_cast<T>(1) ? static_cast<T>(1)
                                                     : t_eff;
    const T t_param = t_lo + (t_hi - t_lo) * t_clamped;

    const auto knots_view = crd::containers::ConstSpan<T>{curve.knots.data(), curve.knots.size()};
    const auto k          = detail::bspline_find_span<T>(knots_view, n_control, t_param);
    T          basis[4U]  = {T{}, T{}, T{}, T{}};
    detail::bspline_basis_d3<T>(knots_view, k, t_param, basis);

    // Sum N_{k-3+r, 3}(t) * P_{k-3+r} for r=0..3.
    crd::math::Vec3<T> result{};
    for (crd::u32 r = 0U; r < 4U; ++r) { result = result + curve.points[k - degree + r] * basis[r]; }
    return result;
}

template <crd::math::MathScalar T>
[[nodiscard]] crd::math::Vec3<T> evaluate_derivative(const BSpline3<T>& curve, T t) noexcept
{
    // Finite-difference derivative for v10a substrate. The B-spline analytic
    // derivative is itself a degree-2 B-spline over a shifted control polygon
    // (Piegl & Tiller A3.4) — filed as `v10a-bspline-analytic-derivative`
    // follow-on. The substrate's job is to ship a correct + composable
    // derivative; analytic optimisation lands at v10b/c when sampling pulls
    // on it hot enough to matter.
    const T h       = static_cast<T>(1e-4);
    const auto p_minus = evaluate(curve, t - h);
    const auto p_plus  = evaluate(curve, t + h);
    return (p_plus - p_minus) * (static_cast<T>(1) / (static_cast<T>(2) * h));
}

} // namespace crd::geometry::curves
