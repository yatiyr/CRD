#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — ConvexHullView queries (Phase 3.1.7 v2e).
//
// Three queries on the v1h `ConvexHullView<T>` type:
//
//   - **`ray_vs_hull`** — Cyrus-Beck parametric face-plane clipping. For
//     each face plane, compute the ray parameter where the ray crosses
//     the plane; classify as entry (denom < 0 — ray's direction has a
//     component opposite the outward normal, so the ray is heading INTO
//     the halfspace) or exit (denom > 0); track max(entries) and
//     min(exits). Returns the entry hit with the entering face's index
//     as payload. Lowest-face-index tiebreak on coincident `t_enter`
//     (ADR-0076 §4 pin #14).
//
//   - **`closest_point(hull, p)`** — runs GJK between a `PointShape{p}`
//     wrapper (any direction's support of a point is the point itself)
//     and the hull. Separated case: GJK's `witness_b_world` is the
//     closest point. Overlap case (`p` inside the hull): project `p`
//     onto the face plane with smallest `|signed_distance|`.
//
//   - **`contains(hull, p)`** — already shipped in v1h's
//     `crd-geometry-primitives::primitives.hpp`. v2e adds the facade
//     overload (transparently, since the primitives function is already
//     in scope when convex.hpp is included).
//
// **From-inside ray convention (PINNED)**: `ray_vs_hull` returns
// `std::nullopt` when the ray origin is INSIDE the hull. Callers who
// want from-inside semantics call `contains(hull, ray.origin)` first.
//
// This is INTENTIONALLY ASYMMETRIC with `bvh_raycast`, which reports a
// hit when the ray origin is inside an AABB (returning the exit-t as
// the hit). The asymmetry exists because:
//   - BVH raycast asks "did any AABB region contain a ray segment?";
//     from-inside is a yes.
//   - Hull raycast asks "did the ray enter the hull from outside?";
//     from-inside has no meaningful entry point — the ray starts inside.
// Both decisions are correct for their use case. Callers using the
// unified `queries.hpp::raycast(...)` facade must check the docstring
// for the specific shape type to know the from-inside semantics.
//
// **Closest-point-on-hull-from-inside limit (PINNED)**: when `p` is
// inside the hull, `closest_point` returns a point on the nearest face's
// PLANE — NOT necessarily on the face's actual polygon boundary. For
// points whose true closest surface point is on a face's BOUNDARY EDGE
// (rather than the face interior), the returned point may be slightly
// outside the polygon. The signed-distance magnitude is correct. v2j's
// Sutherland-Hodgman polygon clipping resolves this for callers needing
// exact face-polygon clipping; v2e ships the face-plane form (matches
// Box2D's convention and is fine for physics contact projection).
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/convex/gjk.hpp>
#include <crd/geometry/convex/support.hpp>
#include <crd/geometry/primitives/constants.hpp>
#include <crd/geometry/primitives/primitives.hpp>
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
using crd::math::Vec3;

// `PointShape<T>` — a zero-extent ConvexShape. Used internally by
// `closest_point(hull, p)` to drive GJK against the hull; exposed publicly
// because eylem v1 narrowphase particle-vs-shape queries can wrap a
// particle's world position in one of these to query the unified
// `compute_contact(...)` facade.
//
// `support(PointShape, dir) → SupportPoint{point, k_invalid_vertex}` for
// any direction (a point IS its own support — there's nothing to
// maximize). `vertex_idx = k_invalid_vertex` keeps GJK on the geometric-
// termination path (the same path Sphere uses, since "smooth shapes"
// can't form bit-exact index-match termination).
template <MathScalar T> struct PointShape
{
    Vec3<T> point{};

    constexpr PointShape() noexcept = default;
    constexpr explicit PointShape(const Vec3<T>& p) noexcept : point(p) {}
};

template <MathScalar T>
[[nodiscard]] inline SupportPoint<T> support(const PointShape<T>& s, const Vec3<T>& /*dir*/) noexcept
{
    return SupportPoint<T>{s.point, k_invalid_vertex};
}

// ---- ray_vs_hull (Cyrus-Beck) ----------------------------------------------
//
// Returns the entry hit (from-outside) or `std::nullopt` (no hit OR
// from-inside, per the convention pin above). Payload is the entering
// face's index in `hull.faces`.
//
// Time complexity: O(F) where F = `hull.faces.size()`. No allocations.
template <MathScalar T>
[[nodiscard]] inline std::optional<RayHit<crd::u32>>
ray_vs_hull(const primitives::ConvexHullView<T>& hull, const primitives::Ray3<T>& ray,
            T tmax = std::numeric_limits<T>::infinity()) noexcept
{
    CRD_ASSERT(!hull.faces.empty());
    const T eps = crd::geometry::primitives::k_distance_epsilon<T>();

    // Cyrus-Beck running parameters: ray enters the polytope at t_enter
    // (the max over entering planes) and exits at t_exit (the min over
    // exit planes). Initial: enters at -∞ (start of ray), exits at tmax.
    T t_enter = -std::numeric_limits<T>::infinity();
    T t_exit = tmax;
    crd::u32 entry_face = ~crd::u32{0};

    for (crd::usize i = 0; i < hull.faces.size(); ++i)
    {
        const primitives::Plane<T>& F = hull.faces[i];
        const T denom = crd::math::dot(F.normal, ray.direction);
        const T sd_origin = crd::math::dot(F.normal, ray.origin) + F.d;

        if (denom == static_cast<T>(0))
        {
            // Ray parallel to this face plane. If origin is on the OUTWARD
            // side (positive signed distance), the ray can never enter the
            // halfspace and there's no intersection. Else, the ray is
            // entirely inside this halfspace — neither an entry nor an
            // exit constraint comes from this plane.
            if (sd_origin > eps)
            {
                return std::nullopt;
            }
            continue;
        }

        const T t_face = -sd_origin / denom;

        if (denom > static_cast<T>(0))
        {
            // EXIT plane (ray heading OUT through this face). Track min.
            if (t_face < t_exit)
            {
                t_exit = t_face;
            }
        }
        else
        {
            // ENTRY plane. Track max with lowest-face-index tiebreak on
            // coincident t (ADR-0076 §4 pin #14).
            if (t_face > t_enter + eps)
            {
                t_enter = t_face;
                entry_face = static_cast<crd::u32>(i);
            }
            else if (t_face > t_enter - eps && static_cast<crd::u32>(i) < entry_face)
            {
                // Tied within eps; lower index wins. (Don't update t_enter —
                // keep the running max — only the face index.)
                entry_face = static_cast<crd::u32>(i);
            }
        }
    }

    // Empty intersection: enter is past exit.
    if (t_enter > t_exit + eps)
    {
        return std::nullopt;
    }
    // From-inside (origin is in the hull): t_enter is negative, ray has no
    // entry hit. Per convention, return nullopt; callers use `contains(...)`
    // to detect from-inside.
    if (t_enter < static_cast<T>(0))
    {
        return std::nullopt;
    }
    // From-outside hit past tmax.
    if (t_enter > tmax)
    {
        return std::nullopt;
    }
    return RayHit<crd::u32>{t_enter, entry_face};
}

// ---- closest_point(ConvexHullView, Vec3) -----------------------------------
//
// Returns the closest point on the hull's surface to `p`. Algorithm:
// run GJK with a `PointShape{p}` against the hull at identity transforms.
// Separated case: GJK's `witness_b_world` is the answer. Overlap case
// (`p` inside the hull): GJK reports overlap but its witnesses are at
// origin (Minkowski-diff origin); recover the surface point by projecting
// `p` onto the face PLANE with smallest `|signed_distance|`. See the
// header doc for the "closest-point-on-hull-from-inside limit" note.
template <MathScalar T>
[[nodiscard]] inline Vec3<T> closest_point(const primitives::ConvexHullView<T>& hull, const Vec3<T>& p) noexcept
{
    const PointShape<T> ps(p);
    const crd::math::Transform<T> id(Vec3<T>(static_cast<T>(0)), crd::math::Quat<T>::identity());
    const GjkResult<T> gjk = gjk_distance<T>(ps, id, hull, id);
    if (!gjk.overlapping)
    {
        return gjk.witness_b_world;
    }
    // p is inside the hull. Project to the closest face plane.
    CRD_ASSERT(!hull.faces.empty());
    T smallest_abs_sd = std::numeric_limits<T>::infinity();
    Vec3<T> closest = p; // fallback (degenerate hull with no faces)
    for (crd::usize i = 0; i < hull.faces.size(); ++i)
    {
        const T sd = primitives::signed_distance(hull.faces[i], p);
        const T abs_sd = sd < static_cast<T>(0) ? -sd : sd;
        if (abs_sd < smallest_abs_sd)
        {
            smallest_abs_sd = abs_sd;
            closest = Vec3<T>(p.x - hull.faces[i].normal.x * sd, p.y - hull.faces[i].normal.y * sd,
                              p.z - hull.faces[i].normal.z * sd);
        }
    }
    return closest;
}

template <MathScalar T>
[[nodiscard]] inline T distance_squared(const primitives::ConvexHullView<T>& hull, const Vec3<T>& p) noexcept
{
    const Vec3<T> cp = closest_point(hull, p);
    const Vec3<T> diff(cp.x - p.x, cp.y - p.y, cp.z - p.z);
    return crd::math::dot(diff, diff);
}

template <MathScalar T>
[[nodiscard]] inline T distance(const primitives::ConvexHullView<T>& hull, const Vec3<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(hull, p)));
}

} // namespace crd::geometry::convex

// ---- Unified facade overloads ---------------------------------------------
//
// `crd::geometry::raycast(ConvexHullView, Ray3, tmax)` overload — found via
// ADL alongside the BVH `raycast` family in `<crd/geometry/queries.hpp>`.
// Same signature shape (returns `optional<RayHit<u32>>`), so eylem's
// raycast caller code is uniform across BVH-broadphase and hull-narrow-
// phase paths. From-inside semantics differ (see hull_queries.hpp header
// comment) — `contains(hull, ray.origin)` is the from-inside detection.

namespace crd::geometry
{
template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<RayHit<crd::u32>>
raycast(const primitives::ConvexHullView<T>& hull, const primitives::Ray3<T>& ray,
        T tmax = std::numeric_limits<T>::infinity()) noexcept
{
    return convex::ray_vs_hull<T>(hull, ray, tmax);
}
} // namespace crd::geometry
