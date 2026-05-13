#pragma once

// ---------------------------------------------------------------------------
// crd-geometry — unified query facade (Phase 3.1.7 v1i-a, ADR-0076 §15 + §16).
//
// `raycast` / `overlap` / `closest_point` / `contains` / `distance` as a single
// set of free-function overloads in `crd::geometry`, resolved at compile time
// over `{primitive shapes, BvhTree, Bvh4Tree, DynamicBvh}`. Zero overhead — the
// overloads forward to the existing per-backend functions (`bvh_raycast` etc.).
// No `IAcceleration` vtable; compile-time overload polymorphism per ADR-0076
// §16 pin #1.
//
// Result types are the templated `RayHit<P>` / `ClosestPointResult<P>` from
// `result_types.hpp`; backend aliases (`BvhRayHit = RayHit<u32>` etc.) document
// the payload at the call site without dragging in an over-generic variant
// (§16 pin #2).
//
// Primitive overloads (sphere×point, AABB×AABB, ray×AABB, …) are NOT redefined
// here — `primitives/closest_point.hpp` already provides `closest_point(Shape,
// Vec3) → Vec3` + `distance(Shape, Vec3) → T` + `distance_squared(Shape, Vec3)
// → T`; `primitives/intersect.hpp` already provides `intersects(A, B) → bool`
// (and the `intersect_ray_*` named forms with the `T& out_t` out-parameter).
// This header simply *includes* those, so a caller of the facade gets them via
// ADL alongside the BVH overloads. It also includes `signed_distance.hpp` for
// the v1h analytic SDFs.
//
// `find_overlapping_pairs(DynamicBvh, OutFn)` (broadphase self-overlap) and the
// shapecast surface (`cast_sphere` / `cast_box`) land in v1i-b / v1i-c.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh/bvh4.hpp>
#include <crd/geometry/bvh/bvh4_simd.hpp>
#include <crd/geometry/bvh/bvh_query.hpp>
#include <crd/geometry/bvh/bvh_shapecast.hpp>
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/bvh/dynamic_bvh.hpp>
#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/intersect.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/geometry/primitives/signed_distance.hpp>
#include <crd/geometry/result_types.hpp>

#include <limits>
#include <optional>

namespace crd::geometry
{
// ---- raycast (BVH backends) ------------------------------------------------
//
// Each `raycast(...)` overload returns `optional<BvhRayHit>` (= `RayHit<u32>`)
// — the nearest primitive whose AABB the ray enters within `[0, tmax]`, or
// `nullopt` if nothing is hit (including the empty-tree case). Bit-identical
// results across backends for a given input — the collapse to BVH4 / use of
// the parallel build / etc. only change traversal shape, not the chosen hit.
//
// `DynamicBvh::raycast` is intentionally NOT given a nearest-hit overload
// here — its native form is a visit-every-hit callback (broadphase). Eylem's
// v1c broadphase wraps that callback to its own narrowphase refinement.

[[nodiscard]] inline std::optional<bvh::BvhRayHit>
raycast(const bvh::BvhTree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
        const primitives::Ray3<crd::f32>& ray, crd::f32 tmax = std::numeric_limits<crd::f32>::infinity())
{
    return bvh::bvh_raycast(tree, prims, ray, tmax);
}

[[nodiscard]] inline std::optional<bvh::BvhRayHit>
raycast(const bvh::Bvh4Tree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
        const primitives::Ray3<crd::f32>& ray, crd::f32 tmax = std::numeric_limits<crd::f32>::infinity())
{
    return bvh::bvh4_raycast(tree, prims, ray, tmax);
}

// ---- overlap (BVH backends) ------------------------------------------------
//
// Visit every primitive whose AABB overlaps `box` via `on_prim(u32)` callback,
// or append the indices to `out`. Order matches the existing per-backend
// functions (traversal order, deterministic).

template <typename Fn>
inline void overlap(const bvh::BvhTree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
                    const primitives::AABB3<crd::f32>& box, Fn&& on_prim)
{
    bvh::bvh_overlap(tree, prims, box, static_cast<Fn&&>(on_prim));
}

inline void overlap(const bvh::BvhTree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
                    const primitives::AABB3<crd::f32>& box, crd::containers::Array<crd::u32>& out)
{
    bvh::bvh_overlap(tree, prims, box, out);
}

template <typename Fn>
inline void overlap(const bvh::Bvh4Tree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
                    const primitives::AABB3<crd::f32>& box, Fn&& on_prim)
{
    bvh::bvh4_overlap(tree, prims, box, static_cast<Fn&&>(on_prim));
}

inline void overlap(const bvh::Bvh4Tree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
                    const primitives::AABB3<crd::f32>& box, crd::containers::Array<crd::u32>& out)
{
    bvh::bvh4_overlap(tree, prims, box, out);
}

// For DynamicBvh, overlap visits every leaf whose *fat* AABB overlaps `box` —
// broadphase semantics; the caller's narrowphase refines against tight prim
// data. Reports `user_data` (not a prim index — the tree carries the payload).
template <typename Fn>
inline void overlap(const bvh::DynamicBvh& tree, const primitives::AABB3<crd::f32>& box, Fn&& on_leaf)
{
    tree.query(box, static_cast<Fn&&>(on_leaf));
}

inline void overlap(const bvh::DynamicBvh& tree, const primitives::AABB3<crd::f32>& box,
                    crd::containers::Array<crd::u32>& out)
{
    tree.query(box, out);
}

// ---- closest_point (BVH backends) -----------------------------------------
//
// Closest primitive (and the closest point on that primitive's AABB) within
// `max_dist`. `optional<BvhClosestPoint>` (= `ClosestPointResult<u32>`). The
// DynamicBvh form reports `user_data` in the payload (broadphase — closest by
// fat AABB; the caller's narrowphase refines).
//
// Primitive `closest_point(Shape, Vec3) → Vec3` lives in
// `primitives/closest_point.hpp` (transitively included above) — same name,
// different signature; the facade does not shadow it.

[[nodiscard]] inline std::optional<bvh::BvhClosestPoint>
closest_point(const bvh::BvhTree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
              const crd::math::Vec3<crd::f32>& query,
              crd::f32 max_dist = std::numeric_limits<crd::f32>::infinity())
{
    return bvh::bvh_closest_point(tree, prims, query, max_dist);
}

[[nodiscard]] inline std::optional<bvh::BvhClosestPoint>
closest_point(const bvh::Bvh4Tree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
              const crd::math::Vec3<crd::f32>& query,
              crd::f32 max_dist = std::numeric_limits<crd::f32>::infinity())
{
    return bvh::bvh4_closest_point(tree, prims, query, max_dist);
}

[[nodiscard]] inline std::optional<ClosestPointResult<crd::u32>>
closest_point(const bvh::DynamicBvh& tree, const crd::math::Vec3<crd::f32>& query,
              crd::f32 max_dist = std::numeric_limits<crd::f32>::infinity())
{
    return tree.closest_point(query, max_dist);
}

// ---- shapecast (sweep tests) — v1i-b --------------------------------------
//
// "Sweep a moving shape along `dir` from t=0 to t=tmax; report the first
// time-of-impact (TOI) with `target`." For closed-form moving-shape /
// target-shape pairs the implementation is a Minkowski-sum reduction onto a
// ray-cast against an inflated target (always exact when both are AABBs;
// conservative when a sphere is involved — the corners of the
// Minkowski-inflated AABB are square instead of rounded). The general
// convex-shape shapecast is GJK-cast in `-convex` v2; eylem v6 CCD's
// two-moving-convex case stays in eylem.
//
// Convention pinned (v1i-b decision): for primitive overloads the argument
// order is `(moving, dir, tmax, target)` — moving shape first, target last —
// matching the natural reading "cast a sphere along dir for tmax against a
// box". For BVH overloads the structure-first convention from `raycast` /
// `closest_point` holds: `(tree, prims, moving, dir, tmax)`.
//
// `t = 0` for an "already overlapping" start position (the slab test returns
// `tmin = 0` when the ray origin is inside the inflated target). For
// sphere-vs-sphere and sphere-vs-plane the `intersect_ray_*` primitives do
// not naturally give this — we add the overlap-at-start check explicitly.
//
// `dir` need not be unit; `t` is the parameter in `dir` units. No SIMD path
// for `bvh4_shapecast_*` in v1i-b (scalar per-child inflate-and-slab); the
// `Vec4f` ray-vs-4-inflated-AABB kernel reuse is a follow-up.

// ---- primitive `cast_ray` (renamed delegations) ---------------------------
//
// `cast_ray(Ray, Shape)` is a degenerate shapecast (zero-extent moving shape).
// It's the same operation as the existing `intersect_ray_*` family in
// `intersect.hpp` / `primitives.hpp` — those return `bool` + an out-param;
// the facade wraps them to return `optional<f32>` to match `cast_sphere` /
// `cast_box`. Caller can pick either style.

template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<T> cast_ray(const primitives::Ray3<T>& ray, const primitives::AABB3<T>& target,
                                               T tmax = std::numeric_limits<T>::infinity()) noexcept
{
    T t = static_cast<T>(0);
    if (!primitives::intersect_ray_aabb(ray, target, t) || t > tmax)
    {
        return std::nullopt;
    }
    return t;
}

template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<T> cast_ray(const primitives::Ray3<T>& ray, const primitives::OBB3<T>& target,
                                               T tmax = std::numeric_limits<T>::infinity()) noexcept
{
    T t = static_cast<T>(0);
    if (!primitives::intersect_ray_obb(ray, target, t) || t > tmax)
    {
        return std::nullopt;
    }
    return t;
}

template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<T> cast_ray(const primitives::Ray3<T>& ray, const primitives::Sphere<T>& target,
                                               T tmax = std::numeric_limits<T>::infinity()) noexcept
{
    T t = static_cast<T>(0);
    if (!primitives::intersect_ray_sphere(ray, target, t) || t > tmax)
    {
        return std::nullopt;
    }
    return t;
}

template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<T> cast_ray(const primitives::Ray3<T>& ray, const primitives::Plane<T>& target,
                                               T tmax = std::numeric_limits<T>::infinity()) noexcept
{
    T t = static_cast<T>(0);
    if (!primitives::intersect_ray_plane(ray, target, t) || t > tmax)
    {
        return std::nullopt;
    }
    return t;
}

// ---- primitive `cast_sphere` ----------------------------------------------

// Sphere-vs-AABB shapecast. Conservative: inflate `target` by `moving.radius`
// on every face (square corners — true Minkowski has rounded corners), then
// ray-cast `moving.center` along `dir`. The slab test naturally returns t=0
// when the inflated AABB contains the start position — matches the "already
// overlapping at start" convention.
template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<T> cast_sphere(const primitives::Sphere<T>& moving,
                                                  const crd::math::Vec3<T>& dir, T tmax,
                                                  const primitives::AABB3<T>& target) noexcept
{
    const crd::math::Vec3<T> pad(moving.radius, moving.radius, moving.radius);
    const primitives::AABB3<T> grown(
        crd::math::Vec3<T>(target.min.x - pad.x, target.min.y - pad.y, target.min.z - pad.z),
        crd::math::Vec3<T>(target.max.x + pad.x, target.max.y + pad.y, target.max.z + pad.z));
    const primitives::Ray3<T> ray(moving.center, dir);
    T t = static_cast<T>(0);
    if (!primitives::intersect_ray_aabb(ray, grown, t) || t > tmax)
    {
        return std::nullopt;
    }
    return t;
}

// Sphere-vs-sphere shapecast. Exact: replace `target` with a sphere of radius
// (moving.radius + target.radius) at `target.center`, ray-cast `moving.center`
// along `dir`. Returns t=0 if the spheres are already overlapping at start.
template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<T> cast_sphere(const primitives::Sphere<T>& moving,
                                                  const crd::math::Vec3<T>& dir, T tmax,
                                                  const primitives::Sphere<T>& target) noexcept
{
    const T r_sum = moving.radius + target.radius;
    const crd::math::Vec3<T> oc = moving.center - target.center;
    if (crd::math::dot(oc, oc) <= r_sum * r_sum)
    {
        return static_cast<T>(0); // already overlapping at start
    }
    const primitives::Ray3<T> ray(moving.center, dir);
    const primitives::Sphere<T> grown(target.center, r_sum);
    T t = static_cast<T>(0);
    if (!primitives::intersect_ray_sphere(ray, grown, t) || t > tmax)
    {
        return std::nullopt;
    }
    return t;
}

// Sphere-vs-plane shapecast. Exact: the moving sphere first touches the plane
// when its center is at distance `radius` from the plane. Two-sided — returns
// the earliest t≥0 regardless of which side the sphere approaches from.
template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<T> cast_sphere(const primitives::Sphere<T>& moving,
                                                  const crd::math::Vec3<T>& dir, T tmax,
                                                  const primitives::Plane<T>& target) noexcept
{
    const T sd_now = primitives::signed_distance(target, moving.center);
    if (sd_now * sd_now <= moving.radius * moving.radius)
    {
        return static_cast<T>(0); // already touching / overlapping
    }
    const T denom = crd::math::dot(target.normal, dir);
    if (denom == static_cast<T>(0))
    {
        // Reachable only when the sphere was NOT already touching the plane
        // (the early return above handles that case). Parallel-and-clear =
        // never touches → no hit is correct. If the early return is ever
        // refactored, this branch must be reaudited for the straddling case.
        return std::nullopt;
    }
    // Center is at signed-distance `sd_now`; want |sd_now + t·denom| = radius.
    // sd_now and radius have the same sign-relationship by the early return
    // above, so the relevant root is `sd_now + t·denom = ±radius` whichever
    // gives the smaller positive t.
    const T r = moving.radius;
    const T t_pos = (r - sd_now) / denom;
    const T t_neg = (-r - sd_now) / denom;
    T t = std::numeric_limits<T>::infinity();
    if (t_pos >= static_cast<T>(0) && t_pos < t)
    {
        t = t_pos;
    }
    if (t_neg >= static_cast<T>(0) && t_neg < t)
    {
        t = t_neg;
    }
    if (t > tmax)
    {
        return std::nullopt;
    }
    return t;
}

// ---- primitive `cast_box` -------------------------------------------------

// Box-vs-AABB shapecast. Exact: Minkowski sum of two AABBs is an AABB —
// inflate `target` by `moving`'s half-extents and ray-cast the moving box's
// center along `dir`.
template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<T> cast_box(const primitives::AABB3<T>& moving, const crd::math::Vec3<T>& dir,
                                               T tmax, const primitives::AABB3<T>& target) noexcept
{
    const crd::math::Vec3<T> half = (moving.max - moving.min) * static_cast<T>(0.5);
    const crd::math::Vec3<T> center = (moving.max + moving.min) * static_cast<T>(0.5);
    const primitives::AABB3<T> grown(
        crd::math::Vec3<T>(target.min.x - half.x, target.min.y - half.y, target.min.z - half.z),
        crd::math::Vec3<T>(target.max.x + half.x, target.max.y + half.y, target.max.z + half.z));
    const primitives::Ray3<T> ray(center, dir);
    T t = static_cast<T>(0);
    if (!primitives::intersect_ray_aabb(ray, grown, t) || t > tmax)
    {
        return std::nullopt;
    }
    return t;
}

// ---- broadphase self-overlap (v1i-c) --------------------------------------
//
// Dual-descent over a `DynamicBvh`: all `(i<j)` pairs of leaves whose fat
// AABBs overlap, in one traversal. The all-pairs primitive eylem v1c's
// broadphase wraps. `O(n + |pairs|)` for typical trees vs `O(n²)` brute force.
// Static-tree variants (BvhTree / Bvh4Tree) are NOT in v1i-c — the static
// trees don't store user_data on leaves, and the broadphase self-overlap use
// case is inherently dynamic; if a static-tree all-pairs ever surfaces, a
// thin wrapper at the call site can do it via `bvh_overlap` per-prim, or a
// dedicated dual-descent can be added without an API change.

template <typename Fn> inline void find_overlapping_pairs(const bvh::DynamicBvh& tree, Fn&& on_pair)
{
    tree.find_overlapping_pairs(static_cast<Fn&&>(on_pair));
}

inline void find_overlapping_pairs(const bvh::DynamicBvh& tree, crd::containers::Array<bvh::DynamicBvhPair>& out)
{
    tree.find_overlapping_pairs(out);
}

// Hot-path overloads — reuse caller-owned scratch buffers across calls
// instead of allocating per call (broadphase callers like eylem v1c hit
// this on every physics tick at 60-1000 Hz). The scratch's `clear()` is
// called inside.

template <typename Fn>
inline void find_overlapping_pairs(const bvh::DynamicBvh& tree, Fn&& on_pair,
                                   bvh::DynamicBvhPairScratch& scratch)
{
    tree.find_overlapping_pairs(static_cast<Fn&&>(on_pair), scratch);
}

inline void find_overlapping_pairs(const bvh::DynamicBvh& tree, crd::containers::Array<bvh::DynamicBvhPair>& out,
                                   bvh::DynamicBvhPairScratch& scratch)
{
    tree.find_overlapping_pairs(out, scratch);
}

// ---- BVH shapecast --------------------------------------------------------

[[nodiscard]] inline std::optional<bvh::BvhRayHit>
cast_sphere(const bvh::BvhTree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
            const primitives::Sphere<crd::f32>& moving, const crd::math::Vec3<crd::f32>& dir,
            crd::f32 tmax = std::numeric_limits<crd::f32>::infinity())
{
    return bvh::bvh_shapecast_sphere(tree, prims, moving, dir, tmax);
}

[[nodiscard]] inline std::optional<bvh::BvhRayHit>
cast_sphere(const bvh::Bvh4Tree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
            const primitives::Sphere<crd::f32>& moving, const crd::math::Vec3<crd::f32>& dir,
            crd::f32 tmax = std::numeric_limits<crd::f32>::infinity())
{
    return bvh::bvh4_shapecast_sphere(tree, prims, moving, dir, tmax);
}

[[nodiscard]] inline std::optional<bvh::BvhRayHit>
cast_box(const bvh::BvhTree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
         const primitives::AABB3<crd::f32>& moving, const crd::math::Vec3<crd::f32>& dir,
         crd::f32 tmax = std::numeric_limits<crd::f32>::infinity())
{
    return bvh::bvh_shapecast_box(tree, prims, moving, dir, tmax);
}

[[nodiscard]] inline std::optional<bvh::BvhRayHit>
cast_box(const bvh::Bvh4Tree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
         const primitives::AABB3<crd::f32>& moving, const crd::math::Vec3<crd::f32>& dir,
         crd::f32 tmax = std::numeric_limits<crd::f32>::infinity())
{
    return bvh::bvh4_shapecast_box(tree, prims, moving, dir, tmax);
}

// ---- contains (primitive shapes) ------------------------------------------
//
// `contains(Shape, Point)` — point-in-shape predicate. Forwards to the
// primitive layer's existing `contains` overloads in `primitives.hpp` /
// `barycentric.hpp` (`contains(AABB3, Point)`, `contains(OBB3, Point)`,
// `contains(Sphere, Point)`, `contains(Frustum, Point)`, `contains(Triangle3,
// Vec3)` via barycentric, …). Those overloads are picked up by ADL when this
// header is included; the facade itself doesn't redefine them — there is no
// "contains on a BVH" operation, only on shapes.

// ---- distance / distance_squared / signed_distance ------------------------
//
// Same delegation: `distance(Shape, Point)` / `distance_squared(Shape, Point)`
// live in `primitives/closest_point.hpp`; `signed_distance(Plane, Point)` /
// `signed_distance(Line2, Point)` live there too; the iq analytic SDFs
// `sd_sphere` / `sd_box` / etc. live in `primitives/signed_distance.hpp`.
// The facade includes them; this comment notes the unified availability.

} // namespace crd::geometry
