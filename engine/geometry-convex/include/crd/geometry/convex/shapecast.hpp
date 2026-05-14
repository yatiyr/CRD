#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — GJK-based convex shapecast (Phase 3.1.7 v2f).
//
// Catto 2013 ("Continuous Collision", GDC) algorithm: given a moving convex
// shape A with a translational sweep direction and a static convex shape B,
// find the first time-of-impact (TOI) ∈ [0, tmax] when their surfaces touch.
//
// **Translational-only** (PIN): A's rotation is fixed; only its translation
// advances along `sweep_dir * t`. Rotational shapecast is eylem v6 CCD's job
// (Conservative Advancement + repeated GJK), not v2f's.
//
// **The iteration loop** (secant / Newton-style root-find on the smooth-
// distance regime):
//
//   t = 0
//   for iter in [0, 32):
//     gjk = gjk_distance(A_at_t, B)
//     if gjk.overlapping:
//       // Already overlapping at this `t` — we've overshot OR we started
//       // already inside. If iter == 0 (started inside): TOI = 0 with
//       // EPA's normal. Else: rewind to the midpoint of (prev_t, t) and
//       // retry — Catto's robustness hack against polyhedral overshoot.
//       ...
//     d = sqrt(gjk.distance_squared)
//     if d <= eps:
//       // Touching: return TOI = t.
//       ...
//     n = unit(witness_b - witness_a)       // A→B direction
//     vn = dot(sweep_dir, n)
//     if vn <= eps:
//       return nullopt                       // Not approaching
//     delta_t = d / vn
//     new_t = t + delta_t
//     if new_t > tmax:
//       return nullopt                       // Out of range
//     t = new_t
//
// **Convergence character**: for SMOOTH inputs (sphere-vs-sphere, sphere-
// vs-capsule), the loop converges quadratically near the root — typical
// iteration count 3-6. For POLYHEDRAL inputs (box-vs-box, hull-vs-hull),
// the support point can jump between vertices/edges/faces as `t` advances,
// breaking the smooth-Newton assumption. Catto's "rewind" mechanism
// catches overshoots: if a linear step lands `t' > true_TOI` (detected
// by `gjk.overlapping == true` at `t'`), back off to `(t_prev + t') / 2`
// and try again. Maintains correctness (no oscillation) at the cost of
// 1-2 extra iters on polyhedral inputs.
//
// **Output convention** (`ConvexShapecastResult<T>`):
//   - `toi` — time-of-impact, ∈ [0, tmax]. Zero ⇒ already touching /
//     overlapping at start.
//   - `normal` — unit vector A→B at TOI. The contact normal at the
//     moment of first impact, pointing from A toward B (so A's surface
//     is on the −normal side, B's on the +normal side at the contact).
//   - `witness_a_world` / `witness_b_world` — contact points on each
//     shape at TOI (A's translation is `xform_a_start.translation +
//     sweep_dir * toi`; the witness on A is in that world frame).
//   - `iteration_count` — observed convergence cost.
//   - `converged` — `true` on natural exit (touching OR overlapping);
//     `false` on iter cap (rare; degenerate input).
//
// **Facade**: `crd::geometry::cast_convex(...) -> optional<T>` returns
// just the TOI (matches `cast_ray` / `cast_sphere` / `cast_box`). Callers
// needing witnesses use `shapecast_convex(...)` directly.
//
// **Determinism**: inherits GJK + EPA determinism pins from v2a/v2c.
// Replay-stable for fixed inputs.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/convex/epa.hpp>
#include <crd/geometry/convex/gjk.hpp>
#include <crd/geometry/convex/support.hpp>
#include <crd/geometry/primitives/constants.hpp>
#include <crd/geometry/result_types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/transform.hpp>
#include <crd/math/vec.hpp>

#include <cmath>
#include <limits>
#include <optional>

namespace crd::geometry::convex
{
using crd::math::MathScalar;
using crd::math::Transform;
using crd::math::Vec3;

template <MathScalar T> struct ConvexShapecastResult
{
    T toi{0};
    Vec3<T> normal{};         // unit, A→B at toi
    Vec3<T> witness_a_world{}; // on A's surface at toi
    Vec3<T> witness_b_world{}; // on B's surface
    crd::u8 iteration_count{0};
    bool converged{false};
};

// Main driver. Returns `nullopt` if A never reaches B within [0, tmax]
// (sweep_dir not approaching, or `tmax * |sweep_dir|` is less than the
// initial distance). Returns `Some(result)` with `toi ∈ [0, tmax]`
// otherwise.
//
// `tmax`: scalar upper bound on TOI in the same time-scale as `sweep_dir`
// (so the maximum translation traveled is `tmax * sweep_dir`). For
// "advance up to one frame", pass `tmax = 1` with `sweep_dir = velocity *
// dt`. For "advance until traveling 10 world units", pass `tmax = 10`
// with `sweep_dir = unit_vector`.
template <MathScalar T, typename A, typename B>
    requires ConvexShape<A, T> && ConvexShape<B, T>
[[nodiscard]] inline std::optional<ConvexShapecastResult<T>>
shapecast_convex(const A& a, const Transform<T>& xform_a_start, const Vec3<T>& sweep_dir, T tmax, const B& b,
                 const Transform<T>& xform_b) noexcept
{
    constexpr crd::u8 k_max_iter = 32;
    const T eps = crd::geometry::primitives::k_distance_epsilon<T>();

    // Newton+bisection hybrid (Catto): track both `t_lower` (known
    // SEPARATED) and `t_upper` (known OVERLAPPING — initially +∞, i.e.
    // unbounded). Newton step takes the Cyrus-Beck linear estimate; if
    // it lands at-or-past `t_upper`, fall back to bisection of
    // `[t_lower, t_upper]`. This converges on the TOI from below
    // (monotonic) and avoids the oscillation that pure Newton produces
    // when the linear step lands exactly at the overlap point.
    T t = static_cast<T>(0);
    T t_lower = static_cast<T>(0);
    T t_upper = std::numeric_limits<T>::infinity();
    Transform<T> xform_a = xform_a_start;
    ConvexShapecastResult<T> result;
    result.converged = false;

    auto set_a_at = [&](T new_t) {
        xform_a.translation = Vec3<T>(xform_a_start.translation.x + sweep_dir.x * new_t,
                                       xform_a_start.translation.y + sweep_dir.y * new_t,
                                       xform_a_start.translation.z + sweep_dir.z * new_t);
    };

    for (crd::u8 iter = 0; iter < k_max_iter; ++iter)
    {
        result.iteration_count = static_cast<crd::u8>(iter + 1);

        const GjkResult<T> gjk = gjk_distance<T>(a, xform_a, b, xform_b);

        if (gjk.overlapping)
        {
            if (t <= eps)
            {
                // Started already overlapping. Run EPA for accurate normal.
                const EpaResult<T> epa = epa_penetration<T>(a, xform_a, b, xform_b, gjk.simplex);
                result.toi = static_cast<T>(0);
                if (epa.converged)
                {
                    result.normal = epa.normal;
                    result.witness_a_world = epa.witness_a_world;
                    result.witness_b_world = epa.witness_b_world;
                }
                else
                {
                    result.witness_a_world = gjk.witness_a_world;
                    result.witness_b_world = gjk.witness_b_world;
                    const T sd_sq = crd::math::dot(sweep_dir, sweep_dir);
                    if (sd_sq > std::numeric_limits<T>::min())
                    {
                        const T inv = static_cast<T>(1) / static_cast<T>(std::sqrt(sd_sq));
                        result.normal = Vec3<T>(sweep_dir.x * inv, sweep_dir.y * inv, sweep_dir.z * inv);
                    }
                    else
                    {
                        result.normal = Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
                    }
                }
                result.converged = true;
                return result;
            }
            // Overlap detected at t > 0. The true TOI is in [t_lower, t].
            // Tighten the upper bound and bisect.
            t_upper = t;
            // If the interval is small enough, we've converged on the TOI.
            // Return with t_lower as the TOI (last known SEPARATED — that's
            // the moment of first contact, by definition).
            if (t_upper - t_lower <= eps)
            {
                // Use witnesses from a final GJK at t_lower to get the
                // surface contact pair (not the slight-overlap witnesses).
                set_a_at(t_lower);
                const GjkResult<T> gjk_final = gjk_distance<T>(a, xform_a, b, xform_b);
                result.toi = t_lower;
                result.witness_a_world = gjk_final.witness_a_world;
                result.witness_b_world = gjk_final.witness_b_world;
                const Vec3<T> d_vec(gjk_final.witness_b_world.x - gjk_final.witness_a_world.x,
                                    gjk_final.witness_b_world.y - gjk_final.witness_a_world.y,
                                    gjk_final.witness_b_world.z - gjk_final.witness_a_world.z);
                const T dv_sq = crd::math::dot(d_vec, d_vec);
                if (dv_sq > std::numeric_limits<T>::min())
                {
                    const T inv = static_cast<T>(1) / static_cast<T>(std::sqrt(dv_sq));
                    result.normal = Vec3<T>(d_vec.x * inv, d_vec.y * inv, d_vec.z * inv);
                }
                else
                {
                    const T sd_sq = crd::math::dot(sweep_dir, sweep_dir);
                    if (sd_sq > std::numeric_limits<T>::min())
                    {
                        const T inv = static_cast<T>(1) / static_cast<T>(std::sqrt(sd_sq));
                        result.normal = Vec3<T>(sweep_dir.x * inv, sweep_dir.y * inv, sweep_dir.z * inv);
                    }
                    else
                    {
                        result.normal = Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
                    }
                }
                result.converged = true;
                return result;
            }
            // Bisect: try midpoint.
            t = (t_lower + t_upper) * static_cast<T>(0.5);
            set_a_at(t);
            continue;
        }

        // Separated. Distance and contact normal:
        const T dist_sq = gjk.distance_squared;
        if (dist_sq <= eps * eps)
        {
            // Touching at this t. TOI converged.
            result.toi = t;
            result.witness_a_world = gjk.witness_a_world;
            result.witness_b_world = gjk.witness_b_world;
            const Vec3<T> d_vec(gjk.witness_b_world.x - gjk.witness_a_world.x,
                                gjk.witness_b_world.y - gjk.witness_a_world.y,
                                gjk.witness_b_world.z - gjk.witness_a_world.z);
            const T dv_sq = crd::math::dot(d_vec, d_vec);
            if (dv_sq > std::numeric_limits<T>::min())
            {
                const T inv = static_cast<T>(1) / static_cast<T>(std::sqrt(dv_sq));
                result.normal = Vec3<T>(d_vec.x * inv, d_vec.y * inv, d_vec.z * inv);
            }
            else
            {
                const T sd_sq = crd::math::dot(sweep_dir, sweep_dir);
                if (sd_sq > std::numeric_limits<T>::min())
                {
                    const T inv = static_cast<T>(1) / static_cast<T>(std::sqrt(sd_sq));
                    result.normal = Vec3<T>(sweep_dir.x * inv, sweep_dir.y * inv, sweep_dir.z * inv);
                }
                else
                {
                    result.normal = Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
                }
            }
            result.converged = true;
            return result;
        }

        // We're SEPARATED at t. Mark this as a new lower bound.
        t_lower = t;

        const T d = static_cast<T>(std::sqrt(dist_sq));
        const Vec3<T> d_vec(gjk.witness_b_world.x - gjk.witness_a_world.x,
                            gjk.witness_b_world.y - gjk.witness_a_world.y,
                            gjk.witness_b_world.z - gjk.witness_a_world.z);
        const T inv_d = static_cast<T>(1) / d;
        const Vec3<T> n(d_vec.x * inv_d, d_vec.y * inv_d, d_vec.z * inv_d);
        const T vn = crd::math::dot(sweep_dir, n);

        if (vn <= eps)
        {
            // Not approaching at this t. If we have no upper bound
            // (t_upper = inf), no impact possible. If we DO have an upper
            // bound, the geometry is non-monotonic in this neighborhood —
            // fall back to bisection.
            if (t_upper == std::numeric_limits<T>::infinity())
            {
                return std::nullopt;
            }
            t = (t_lower + t_upper) * static_cast<T>(0.5);
            set_a_at(t);
            continue;
        }

        // Newton step (Cyrus-Beck linear estimate).
        const T delta_t = d / vn;
        T new_t = t + delta_t;
        if (new_t > tmax)
        {
            // Won't reach contact within tmax.
            return std::nullopt;
        }
        // If Newton lands at-or-past the known overlap upper bound,
        // Newton's linear prediction has converged on the TOI. Return now
        // (clamped to t_upper to ensure TOI ≤ confirmed-overlap point).
        // This is the key path for smooth pairs: Newton predicts exactly
        // at t_upper, return immediately (2-3 iters typical for sphere-
        // vs-sphere) instead of bisecting linearly (~22 iters).
        if (new_t >= t_upper - eps)
        {
            result.toi = (new_t < t_upper) ? new_t : t_upper;
            result.witness_a_world = gjk.witness_a_world;
            result.witness_b_world = gjk.witness_b_world;
            result.normal = n;
            result.converged = true;
            return result;
        }
        if (new_t <= t)
        {
            // Step would not advance (numerical noise). Treat as converged.
            result.toi = t;
            result.witness_a_world = gjk.witness_a_world;
            result.witness_b_world = gjk.witness_b_world;
            result.normal = n;
            result.converged = true;
            return result;
        }
        t = new_t;
        set_a_at(t);
    }

    // Iter cap reached. Return t_lower (last known separated) as the
    // best-known TOI lower bound. Useful for callers who'd rather have a
    // conservative TOI than `nullopt`.
    if (t_upper < std::numeric_limits<T>::infinity())
    {
        // We bracketed the TOI but didn't tighten within iter cap. Return
        // t_lower with witnesses from there.
        set_a_at(t_lower);
        const GjkResult<T> gjk_final = gjk_distance<T>(a, xform_a, b, xform_b);
        result.toi = t_lower;
        result.witness_a_world = gjk_final.witness_a_world;
        result.witness_b_world = gjk_final.witness_b_world;
        const Vec3<T> d_vec(gjk_final.witness_b_world.x - gjk_final.witness_a_world.x,
                            gjk_final.witness_b_world.y - gjk_final.witness_a_world.y,
                            gjk_final.witness_b_world.z - gjk_final.witness_a_world.z);
        const T dv_sq = crd::math::dot(d_vec, d_vec);
        if (dv_sq > std::numeric_limits<T>::min())
        {
            const T inv = static_cast<T>(1) / static_cast<T>(std::sqrt(dv_sq));
            result.normal = Vec3<T>(d_vec.x * inv, d_vec.y * inv, d_vec.z * inv);
        }
        else
        {
            result.normal = Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
        }
        result.converged = false; // didn't tighten to eps; caller may want to investigate
        return result;
    }
    // No overlap ever detected within iter cap — declare no impact.
    return std::nullopt;
}

} // namespace crd::geometry::convex

// ---- Unified facade overload (matches cast_ray / cast_sphere / cast_box) ---
//
// `crd::geometry::cast_convex(A, xform_a_start, sweep_dir, tmax, B, xform_b)`
// — returns `optional<T>` (TOI only); for full contact info call
// `shapecast_convex` directly.

namespace crd::geometry
{
template <crd::math::MathScalar T, typename A, typename B>
    requires convex::ConvexShape<A, T> && convex::ConvexShape<B, T>
[[nodiscard]] inline std::optional<T> cast_convex(const A& a, const crd::math::Transform<T>& xform_a_start,
                                                  const crd::math::Vec3<T>& sweep_dir, T tmax, const B& b,
                                                  const crd::math::Transform<T>& xform_b) noexcept
{
    const auto r = convex::shapecast_convex<T>(a, xform_a_start, sweep_dir, tmax, b, xform_b);
    if (!r.has_value())
    {
        return std::nullopt;
    }
    return r->toi;
}
} // namespace crd::geometry
