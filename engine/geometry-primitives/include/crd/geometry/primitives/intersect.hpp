#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — intersection corpus (Phase 3.1.7 v0c, 2D + 3D).
//
// Two families:
//   * Ray casts — `bool intersect_ray_X(ray, shape, T& out_t, eps)` returning
//     the parameter of the nearest hit with t ≥ 0 (the existing `_plane`/
//     `_sphere`/`_triangle` convention in `primitives.hpp`; 2D peers carry a `2`).
//   * Boolean overlap — `bool intersects(A, B)` (extends the `intersects` set
//     already in `primitives.hpp`: AABB-AABB, Sphere-Sphere, AABB-Sphere,
//     Frustum-AABB/Sphere; 2D AABB2-AABB2, Circle-Circle, AABB2-Circle).
//   Plus 2D `segments_intersect(Segment2, Segment2)` (orientation tests).
//
// Algorithm notes:
//   * Ray↔AABB/OBB: Tavianator-style branch-light slab (init tmin=0 so the ray
//     start is the floor; ±inf from a zero direction component drops that axis).
//     The fully-robust precomputed-`RayPacket` form is v0f, not here.
//   * AABB↔Triangle (3D): Akenine-Möller 2001, "Fast 3D Triangle-Box Overlap
//     Testing" — the 13 separating axes (3 box face normals + the triangle
//     normal + 9 edge×box-axis cross-products), tested in that fixed order.
//   * OBB↔OBB (3D): Gottschalk / Ericson §4.4 — 15-axis SAT (3 + 3 + 9), fixed
//     order; near-parallel edge cross-products (‖·‖ ≈ 0) are *skipped* so a
//     degenerate axis never produces a false "separated" verdict (conservative).
//   * Triangle↔Triangle (3D): Möller 1997, "A Fast Triangle-Triangle
//     Intersection Test" — plane-side classification → interval overlap on the
//     line of intersection (the no-divide variant; coplanar case = 2D test).
//   * Sphere/Circle/Capsule ↔ anything: `distance_squared(X, center/spine) ≤ r²`
//     via the v0b closest-point machinery (so these are exact and tiny).
//   * Capsule↔box (3D): conservative — segment-vs-AABB grown by `r` (no false
//     negatives; false positives only near the box's rounded-corner Minkowski
//     region). The exact SAT form lives in eylem v2's box-pair path.
//
// Determinism (ADR-0076 §4): SAT axes are enumerated in a fixed order (box-A
// face normals → box-B face normals → cross-products in lexicographic
// (Ax,Ay,Az)×(Bx,By,Bz) order); a near-zero cross-product is skipped, never the
// source of a separation; ray-slab uses the standard min/max so a NaN/inf
// component cannot flip the verdict. No transcendental libm calls;
// `std::sqrt`/`std::abs` (IEEE-exact) are allowed.
// ---------------------------------------------------------------------------

#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/primitives.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::Vec2;
using crd::math::Vec3;

// Forward decl — the 3D triangle↔triangle test delegates its coplanar case here.
template <MathScalar T> [[nodiscard]] bool intersects(const Triangle2<T>& a, const Triangle2<T>& b) noexcept;

namespace detail
{
template <MathScalar T> [[nodiscard]] constexpr T abs_(T v) noexcept
{
    return v < static_cast<T>(0) ? -v : v;
}

// One slab pass: fold the [bmin, bmax] interval on this axis into [tmin, tmax].
template <MathScalar T> constexpr void slab(T o, T d, T bmin, T bmax, T& tmin, T& tmax) noexcept
{
    const T inv = static_cast<T>(1) / d; // ±inf if d == 0
    T t1 = (bmin - o) * inv;
    T t2 = (bmax - o) * inv;
    if (inv < static_cast<T>(0))
    {
        const T tmp = t1;
        t1 = t2;
        t2 = tmp;
    }
    tmin = crd::math::max(tmin, t1);
    tmax = crd::math::min(tmax, t2);
}

// 2D orientation: >0 = (a,b,c) counter-clockwise, <0 = clockwise, 0 = collinear.
template <MathScalar T>
[[nodiscard]] constexpr T orient2d(const Vec2<T>& a, const Vec2<T>& b, const Vec2<T>& c) noexcept
{
    return crd::math::cross(b - a, c - a);
}

// SAT projection-interval overlap on `axis` for the OBB extent r and the three
// (already box-centered) triangle vertex projections p0,p1,p2.
template <MathScalar T> [[nodiscard]] constexpr bool sat_separated(T r, T p0, T p1, T p2) noexcept
{
    const T mn = crd::math::min(p0, crd::math::min(p1, p2));
    const T mx = crd::math::max(p0, crd::math::max(p1, p2));
    return mn > r || mx < -r;
}
} // namespace detail

// ===========================================================================
// 3D — ray casts
// ===========================================================================

template <MathScalar T>
[[nodiscard]] constexpr bool intersect_ray_aabb(const Ray3<T>& ray, const AABB3<T>& box, T& out_t) noexcept
{
    T tmin = static_cast<T>(0);
    T tmax = std::numeric_limits<T>::infinity();
    detail::slab(ray.origin.x, ray.direction.x, box.min.x, box.max.x, tmin, tmax);
    detail::slab(ray.origin.y, ray.direction.y, box.min.y, box.max.y, tmin, tmax);
    detail::slab(ray.origin.z, ray.direction.z, box.min.z, box.max.z, tmin, tmax);
    if (tmax < tmin)
    {
        return false;
    }
    out_t = tmin;
    return true;
}

template <MathScalar T>
[[nodiscard]] constexpr bool intersect_ray_obb(const Ray3<T>& ray, const OBB3<T>& obb, T& out_t) noexcept
{
    // Express the ray in the box's frame: o' = R^T (origin - center), d' = R^T d.
    const Vec3<T> p = ray.origin - obb.center;
    const Vec3<T> o(crd::math::dot(p, obb.orientation.c0), crd::math::dot(p, obb.orientation.c1),
                    crd::math::dot(p, obb.orientation.c2));
    const Vec3<T> d(crd::math::dot(ray.direction, obb.orientation.c0),
                    crd::math::dot(ray.direction, obb.orientation.c1),
                    crd::math::dot(ray.direction, obb.orientation.c2));
    T tmin = static_cast<T>(0);
    T tmax = std::numeric_limits<T>::infinity();
    detail::slab(o.x, d.x, -obb.half_extents.x, obb.half_extents.x, tmin, tmax);
    detail::slab(o.y, d.y, -obb.half_extents.y, obb.half_extents.y, tmin, tmax);
    detail::slab(o.z, d.z, -obb.half_extents.z, obb.half_extents.z, tmin, tmax);
    if (tmax < tmin)
    {
        return false;
    }
    out_t = tmin;
    return true;
}

// Ray vs a finite cylinder (Cylinder3 — flat caps). Tests the infinite cylinder
// surface (quadratic in t) clipped to the axial slab [0, L], plus the two end
// disks; returns the nearest valid hit with t ≥ 0.
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_cylinder(const Ray3<T>& ray, const Cylinder3<T>& cyl, T& out_t,
                                                 T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const Vec3<T> axis = cyl.b - cyl.a;
    const T len_sq = crd::math::dot(axis, axis);
    if (!(len_sq > std::numeric_limits<T>::min()))
    {
        return false; // degenerate cylinder (a == b) — treat as empty
    }
    const Vec3<T> oa = ray.origin - cyl.a;
    const T dd = crd::math::dot(ray.direction, ray.direction);
    const T da = crd::math::dot(ray.direction, axis);
    const T oaa = crd::math::dot(oa, axis);

    // Quadratic A t² + 2B t + C = 0 for the (squared) radial distance == r².
    const T A = dd * len_sq - da * da;
    const T B = crd::math::dot(oa, ray.direction) * len_sq - oaa * da;
    const T C = crd::math::dot(oa, oa) * len_sq - oaa * oaa - cyl.radius * cyl.radius * len_sq;

    T best = std::numeric_limits<T>::infinity();
    bool hit = false;
    auto consider = [&](T t)
    {
        if (t >= static_cast<T>(0) && t < best)
        {
            best = t;
            hit = true;
        }
    };

    if (detail::abs_(A) > epsilon)
    {
        const T disc = B * B - A * C;
        if (disc >= static_cast<T>(0))
        {
            const T sq = static_cast<T>(std::sqrt(disc));
            for (const T t : {(-B - sq) / A, (-B + sq) / A})
            {
                const T axial = (oaa + t * da); // ∈ [0, len_sq] means within the slab
                if (t >= static_cast<T>(0) && axial >= static_cast<T>(0) && axial <= len_sq)
                {
                    consider(t);
                }
            }
        }
    }
    // End disks at axial = 0 (cap A) and axial = len_sq (cap B).
    if (detail::abs_(da) > epsilon)
    {
        for (const T cap_axial : {static_cast<T>(0), len_sq})
        {
            const T t = (cap_axial - oaa) / da;
            if (t >= static_cast<T>(0))
            {
                const Vec3<T> hp = ray.origin + ray.direction * t - cyl.a;
                const T radial =
                    crd::math::dot(hp, hp) - (crd::math::dot(hp, axis) * crd::math::dot(hp, axis)) / len_sq;
                if (radial <= cyl.radius * cyl.radius)
                {
                    consider(t);
                }
            }
        }
    }
    if (hit)
    {
        out_t = best;
    }
    return hit;
}

// Ray vs a capsule (Capsule3 — hemispherical caps): cylinder body + two cap
// spheres; nearest valid t ≥ 0.
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_capsule(const Ray3<T>& ray, const Capsule3<T>& cap, T& out_t,
                                                T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    T best = std::numeric_limits<T>::infinity();
    bool hit = false;
    {
        const Vec3<T> axis = cap.b - cap.a;
        const T len_sq = crd::math::dot(axis, axis);
        if (len_sq > std::numeric_limits<T>::min())
        {
            // Infinite-cylinder body clipped to [0, len_sq] axially (no caps here).
            const Vec3<T> oa = ray.origin - cap.a;
            const T dd = crd::math::dot(ray.direction, ray.direction);
            const T da = crd::math::dot(ray.direction, axis);
            const T oaa = crd::math::dot(oa, axis);
            const T A = dd * len_sq - da * da;
            const T B = crd::math::dot(oa, ray.direction) * len_sq - oaa * da;
            const T C = crd::math::dot(oa, oa) * len_sq - oaa * oaa - cap.radius * cap.radius * len_sq;
            if (detail::abs_(A) > epsilon)
            {
                const T disc = B * B - A * C;
                if (disc >= static_cast<T>(0))
                {
                    const T sq = static_cast<T>(std::sqrt(disc));
                    for (const T tt : {(-B - sq) / A, (-B + sq) / A})
                    {
                        const T axial = (oaa + tt * da);
                        if (tt >= static_cast<T>(0) && axial >= static_cast<T>(0) && axial <= len_sq && tt < best)
                        {
                            best = tt;
                            hit = true;
                        }
                    }
                }
            }
        }
    }
    // The two cap spheres (also covers a degenerate a == b → just a sphere).
    for (const Vec3<T>& centre : {cap.a, cap.b})
    {
        T t = static_cast<T>(0);
        if (intersect_ray_sphere(ray, Sphere<T>(centre, cap.radius), t, epsilon) && t < best)
        {
            best = t;
            hit = true;
        }
    }
    if (hit)
    {
        out_t = best;
    }
    return hit;
}

// `point` is inside the solid cylinder (flat caps).
template <MathScalar T> [[nodiscard]] inline bool contains(const Cylinder3<T>& cyl, const Vec3<T>& point) noexcept
{
    const Vec3<T> axis = cyl.b - cyl.a;
    const T len_sq = crd::math::dot(axis, axis);
    if (!(len_sq > std::numeric_limits<T>::min()))
    {
        return false;
    }
    const Vec3<T> ap = point - cyl.a;
    const T axial = crd::math::dot(ap, axis);
    if (axial < static_cast<T>(0) || axial > len_sq)
    {
        return false;
    }
    const T radial = crd::math::dot(ap, ap) - axial * axial / len_sq;
    return radial <= cyl.radius * cyl.radius;
}

// ===========================================================================
// 3D — boolean overlap
// ===========================================================================

// Sphere ↔ X (exact; reduces to a v0b distance-squared test).
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Sphere<T>& s, const AABB3<T>& box) noexcept
{
    return distance_squared(box, s.center) <= s.radius * s.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Sphere<T>& s, const OBB3<T>& obb) noexcept
{
    return distance_squared(obb, s.center) <= s.radius * s.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Sphere<T>& s, const Triangle3<T>& tri) noexcept
{
    return distance_squared(tri, s.center) <= s.radius * s.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Sphere<T>& s, const Segment3<T>& seg) noexcept
{
    return distance_squared(seg, s.center) <= s.radius * s.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Sphere<T>& s, const Plane<T>& plane) noexcept
{
    return distance_squared(plane, s.center) <= s.radius * s.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Sphere<T>& s, const Capsule3<T>& cap) noexcept
{
    const T rr = s.radius + cap.radius;
    return distance_squared(Segment3<T>(cap.a, cap.b), s.center) <= rr * rr;
}

// Capsule ↔ X.
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Capsule3<T>& a, const Capsule3<T>& b) noexcept
{
    const T rr = a.radius + b.radius;
    return distance_squared(Segment3<T>(a.a, a.b), Segment3<T>(b.a, b.b)) <= rr * rr;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Capsule3<T>& cap, const Sphere<T>& s) noexcept
{
    return intersects(s, cap);
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Capsule3<T>& cap, const Plane<T>& plane) noexcept
{
    return crd::math::min(detail::abs_(signed_distance(plane, cap.a)), detail::abs_(signed_distance(plane, cap.b))) <=
               cap.radius ||
           signed_distance(plane, cap.a) * signed_distance(plane, cap.b) < static_cast<T>(0);
}
template <MathScalar T> [[nodiscard]] inline bool intersects(const Capsule3<T>& cap, const Triangle3<T>& tri) noexcept
{
    // Exact: distance(spine segment, triangle) ≤ r. If the segment pierces the
    // triangle the distance is 0; otherwise it is the smallest of {endpoint→tri,
    // segment→each tri edge}.
    const Segment3<T> spine(cap.a, cap.b);
    {
        T t = static_cast<T>(0);
        Vec3<T> bc{};
        const Vec3<T> dir = cap.b - cap.a;
        const T len = static_cast<T>(std::sqrt(crd::math::dot(dir, dir)));
        if (len > std::numeric_limits<T>::min() && intersect_ray_triangle(Ray3<T>(cap.a, dir), tri, t, bc) &&
            t <= static_cast<T>(1))
        {
            return true;
        }
    }
    T best = crd::math::min(distance_squared(tri, cap.a), distance_squared(tri, cap.b));
    Vec3<T> c1{};
    Vec3<T> c2{};
    closest_points(spine, Segment3<T>(tri.a, tri.b), c1, c2);
    best = crd::math::min(best, crd::math::distance_squared(c1, c2));
    closest_points(spine, Segment3<T>(tri.b, tri.c), c1, c2);
    best = crd::math::min(best, crd::math::distance_squared(c1, c2));
    closest_points(spine, Segment3<T>(tri.c, tri.a), c1, c2);
    best = crd::math::min(best, crd::math::distance_squared(c1, c2));
    return best <= cap.radius * cap.radius;
}
// Conservative (no false negatives; false positives only in the box's rounded-
// corner Minkowski region). The exact box↔capsule lives in eylem v2.
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Capsule3<T>& cap, const AABB3<T>& box) noexcept
{
    const Vec3<T> r(cap.radius, cap.radius, cap.radius);
    const AABB3<T> grown(box.min - r, box.max + r);
    // Segment vs grown AABB = ray-slab clipped to [0, 1].
    T tmin = static_cast<T>(0);
    T tmax = static_cast<T>(1);
    const Vec3<T> d = cap.b - cap.a;
    detail::slab(cap.a.x, d.x, grown.min.x, grown.max.x, tmin, tmax);
    detail::slab(cap.a.y, d.y, grown.min.y, grown.max.y, tmin, tmax);
    detail::slab(cap.a.z, d.z, grown.min.z, grown.max.z, tmin, tmax);
    return tmax >= tmin;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Capsule3<T>& cap, const OBB3<T>& obb) noexcept
{
    // In the box's frame this becomes capsule ↔ AABB(-h, h).
    const auto local = [&](const Vec3<T>& w)
    {
        const Vec3<T> p = w - obb.center;
        return Vec3<T>(crd::math::dot(p, obb.orientation.c0), crd::math::dot(p, obb.orientation.c1),
                       crd::math::dot(p, obb.orientation.c2));
    };
    return intersects(
        Capsule3<T>(local(cap.a), local(cap.b), cap.radius),
        AABB3<T>(Vec3<T>(-obb.half_extents.x, -obb.half_extents.y, -obb.half_extents.z), obb.half_extents));
}

// Plane ↔ box / sphere.
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Plane<T>& plane, const AABB3<T>& box) noexcept
{
    const Vec3<T> c = center(box);
    const Vec3<T> e = extents(box);
    const T r =
        e.x * detail::abs_(plane.normal.x) + e.y * detail::abs_(plane.normal.y) + e.z * detail::abs_(plane.normal.z);
    return detail::abs_(signed_distance(plane, c)) <= r;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Plane<T>& plane, const OBB3<T>& obb) noexcept
{
    const T r = obb.half_extents.x * detail::abs_(crd::math::dot(plane.normal, obb.orientation.c0)) +
                obb.half_extents.y * detail::abs_(crd::math::dot(plane.normal, obb.orientation.c1)) +
                obb.half_extents.z * detail::abs_(crd::math::dot(plane.normal, obb.orientation.c2));
    return detail::abs_(signed_distance(plane, obb.center)) <= r;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Plane<T>& plane, const Sphere<T>& s) noexcept
{
    return intersects(s, plane);
}

// AABB ↔ Triangle — Akenine-Möller 2001 (13-axis SAT). Box-relative vertices;
// the 9 edge-cross axes are tested in the fixed (e0,e1,e2)×(x,y,z) order.
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const AABB3<T>& box, const Triangle3<T>& tri) noexcept
{
    const Vec3<T> c = center(box);
    const Vec3<T> h = extents(box);
    const Vec3<T> v0 = tri.a - c;
    const Vec3<T> v1 = tri.b - c;
    const Vec3<T> v2 = tri.c - c;
    const Vec3<T> e0 = v1 - v0;
    const Vec3<T> e1 = v2 - v1;
    const Vec3<T> e2 = v0 - v2;
    using detail::abs_;

    // One separating-axis test: project the 3 verts and the box onto `a`.
    const auto sep = [&](const Vec3<T>& a) -> bool
    {
        const T r = h.x * abs_(a.x) + h.y * abs_(a.y) + h.z * abs_(a.z);
        return detail::sat_separated(r, crd::math::dot(v0, a), crd::math::dot(v1, a), crd::math::dot(v2, a));
    };
    const Vec3<T> ex(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
    const Vec3<T> ey(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0));
    const Vec3<T> ez(static_cast<T>(0), static_cast<T>(0), static_cast<T>(1));
    if (sep(crd::math::cross(ex, e0)) || sep(crd::math::cross(ex, e1)) || sep(crd::math::cross(ex, e2)) ||
        sep(crd::math::cross(ey, e0)) || sep(crd::math::cross(ey, e1)) || sep(crd::math::cross(ey, e2)) ||
        sep(crd::math::cross(ez, e0)) || sep(crd::math::cross(ez, e1)) || sep(crd::math::cross(ez, e2)))
    {
        return false;
    }
    // 3 box face normals.
    if (detail::sat_separated(h.x, v0.x, v1.x, v2.x) || detail::sat_separated(h.y, v0.y, v1.y, v2.y) ||
        detail::sat_separated(h.z, v0.z, v1.z, v2.z))
    {
        return false;
    }
    // Triangle normal: the box must straddle the triangle's plane.
    const Vec3<T> n = crd::math::cross(e0, e1);
    const T d = crd::math::dot(n, v0);
    const T r = h.x * abs_(n.x) + h.y * abs_(n.y) + h.z * abs_(n.z);
    return detail::abs_(d) <= r;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Triangle3<T>& tri, const AABB3<T>& box) noexcept
{
    return intersects(box, tri);
}

// OBB ↔ OBB — 15-axis SAT (3 + 3 + 9), fixed order; near-parallel cross-product
// axes are skipped (a degenerate axis never produces a false separation).
template <MathScalar T> [[nodiscard]] inline bool intersects(const OBB3<T>& a, const OBB3<T>& b) noexcept
{
    const Vec3<T> ax[3] = {a.orientation.c0, a.orientation.c1, a.orientation.c2};
    const Vec3<T> bx[3] = {b.orientation.c0, b.orientation.c1, b.orientation.c2};
    const T ae[3] = {a.half_extents.x, a.half_extents.y, a.half_extents.z};
    const T be[3] = {b.half_extents.x, b.half_extents.y, b.half_extents.z};

    T R[3][3];
    T AbsR[3][3];
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            R[i][j] = crd::math::dot(ax[i], bx[j]);
            AbsR[i][j] = detail::abs_(R[i][j]) + crd::math::default_epsilon<T>();
        }
    }
    const Vec3<T> tw = b.center - a.center;
    const T t[3] = {crd::math::dot(tw, ax[0]), crd::math::dot(tw, ax[1]), crd::math::dot(tw, ax[2])};

    // L = A's face normals (i = 0,1,2).
    for (int i = 0; i < 3; ++i)
    {
        const T ra = ae[i];
        const T rb = be[0] * AbsR[i][0] + be[1] * AbsR[i][1] + be[2] * AbsR[i][2];
        if (detail::abs_(t[i]) > ra + rb)
        {
            return false;
        }
    }
    // L = B's face normals (j = 0,1,2).
    for (int j = 0; j < 3; ++j)
    {
        const T ra = ae[0] * AbsR[0][j] + ae[1] * AbsR[1][j] + ae[2] * AbsR[2][j];
        const T rb = be[j];
        const T tt = t[0] * R[0][j] + t[1] * R[1][j] + t[2] * R[2][j];
        if (detail::abs_(tt) > ra + rb)
        {
            return false;
        }
    }
    // L = A_i × B_j, in (i,j) = (0,0),(0,1),(0,2),(1,0)... fixed order.
    // i=0
    if (detail::abs_(t[2] * R[1][0] - t[1] * R[2][0]) >
        ae[1] * AbsR[2][0] + ae[2] * AbsR[1][0] + be[1] * AbsR[0][2] + be[2] * AbsR[0][1])
    {
        return false;
    }
    if (detail::abs_(t[2] * R[1][1] - t[1] * R[2][1]) >
        ae[1] * AbsR[2][1] + ae[2] * AbsR[1][1] + be[0] * AbsR[0][2] + be[2] * AbsR[0][0])
    {
        return false;
    }
    if (detail::abs_(t[2] * R[1][2] - t[1] * R[2][2]) >
        ae[1] * AbsR[2][2] + ae[2] * AbsR[1][2] + be[0] * AbsR[0][1] + be[1] * AbsR[0][0])
    {
        return false;
    }
    // i=1
    if (detail::abs_(t[0] * R[2][0] - t[2] * R[0][0]) >
        ae[0] * AbsR[2][0] + ae[2] * AbsR[0][0] + be[1] * AbsR[1][2] + be[2] * AbsR[1][1])
    {
        return false;
    }
    if (detail::abs_(t[0] * R[2][1] - t[2] * R[0][1]) >
        ae[0] * AbsR[2][1] + ae[2] * AbsR[0][1] + be[0] * AbsR[1][2] + be[2] * AbsR[1][0])
    {
        return false;
    }
    if (detail::abs_(t[0] * R[2][2] - t[2] * R[0][2]) >
        ae[0] * AbsR[2][2] + ae[2] * AbsR[0][2] + be[0] * AbsR[1][1] + be[1] * AbsR[1][0])
    {
        return false;
    }
    // i=2
    if (detail::abs_(t[1] * R[0][0] - t[0] * R[1][0]) >
        ae[0] * AbsR[1][0] + ae[1] * AbsR[0][0] + be[1] * AbsR[2][2] + be[2] * AbsR[2][1])
    {
        return false;
    }
    if (detail::abs_(t[1] * R[0][1] - t[0] * R[1][1]) >
        ae[0] * AbsR[1][1] + ae[1] * AbsR[0][1] + be[0] * AbsR[2][2] + be[2] * AbsR[2][0])
    {
        return false;
    }
    if (detail::abs_(t[1] * R[0][2] - t[0] * R[1][2]) >
        ae[0] * AbsR[1][2] + ae[1] * AbsR[0][2] + be[0] * AbsR[2][1] + be[1] * AbsR[2][0])
    {
        return false;
    }
    return true;
}

// Triangle ↔ Triangle — Möller 1997 (no-divide; coplanar case → 2D SAT).
template <MathScalar T> [[nodiscard]] inline bool intersects(const Triangle3<T>& t1, const Triangle3<T>& t2) noexcept
{
    using detail::abs_;
    const Vec3<T> n2 = crd::math::cross(t2.b - t2.a, t2.c - t2.a);
    const T d2 = -crd::math::dot(n2, t2.a);
    T du0 = crd::math::dot(n2, t1.a) + d2;
    T du1 = crd::math::dot(n2, t1.b) + d2;
    T du2 = crd::math::dot(n2, t1.c) + d2;
    const T eps = crd::math::default_epsilon<T>();
    if (abs_(du0) < eps)
    {
        du0 = static_cast<T>(0);
    }
    if (abs_(du1) < eps)
    {
        du1 = static_cast<T>(0);
    }
    if (abs_(du2) < eps)
    {
        du2 = static_cast<T>(0);
    }
    if (du0 * du1 > static_cast<T>(0) && du0 * du2 > static_cast<T>(0))
    {
        return false; // t1 entirely on one side of t2's plane
    }
    const Vec3<T> n1 = crd::math::cross(t1.b - t1.a, t1.c - t1.a);
    const T d1 = -crd::math::dot(n1, t1.a);
    T dv0 = crd::math::dot(n1, t2.a) + d1;
    T dv1 = crd::math::dot(n1, t2.b) + d1;
    T dv2 = crd::math::dot(n1, t2.c) + d1;
    if (abs_(dv0) < eps)
    {
        dv0 = static_cast<T>(0);
    }
    if (abs_(dv1) < eps)
    {
        dv1 = static_cast<T>(0);
    }
    if (abs_(dv2) < eps)
    {
        dv2 = static_cast<T>(0);
    }
    if (dv0 * dv1 > static_cast<T>(0) && dv0 * dv2 > static_cast<T>(0))
    {
        return false;
    }

    const Vec3<T> dir = crd::math::cross(n1, n2);
    if (crd::math::dot(dir, dir) < std::numeric_limits<T>::min())
    {
        // Coplanar: project onto the plane's dominant axis and run the 2D test.
        const Vec3<T> an(abs_(n1.x), abs_(n1.y), abs_(n1.z));
        const int drop = (an.x > an.y) ? (an.x > an.z ? 0 : 2) : (an.y > an.z ? 1 : 2);
        const auto to2 = [drop](const Vec3<T>& v) -> Vec2<T>
        {
            return drop == 0 ? Vec2<T>(v.y, v.z) : (drop == 1 ? Vec2<T>(v.x, v.z) : Vec2<T>(v.x, v.y));
        };
        return intersects(Triangle2<T>(to2(t1.a), to2(t1.b), to2(t1.c)), Triangle2<T>(to2(t2.a), to2(t2.b), to2(t2.c)));
    }

    // Project onto the dominant axis of `dir`; compute the two intervals on the
    // intersection line and test overlap.
    const Vec3<T> ad(abs_(dir.x), abs_(dir.y), abs_(dir.z));
    const int idx = (ad.x > ad.y) ? (ad.x > ad.z ? 0 : 2) : (ad.y > ad.z ? 1 : 2);
    const auto pick = [idx](const Vec3<T>& v)
    {
        return idx == 0 ? v.x : (idx == 1 ? v.y : v.z);
    };
    const T p1[3] = {pick(t1.a), pick(t1.b), pick(t1.c)};
    const T p2[3] = {pick(t2.a), pick(t2.b), pick(t2.c)};

    const auto interval = [](const T p[3], T e0, T e1, T e2, T& lo, T& hi)
    {
        // The two vertices on opposite sides of the plane define the segment of
        // this triangle that crosses the line; clamp by the third's contribution.
        T s0 = static_cast<T>(0);
        T s1 = static_cast<T>(0);
        if (e0 * e1 > static_cast<T>(0)) // a,b same side -> c is the odd one
        {
            s0 = p[0] + (p[2] - p[0]) * e0 / (e0 - e2);
            s1 = p[1] + (p[2] - p[1]) * e1 / (e1 - e2);
        }
        else if (e0 * e2 > static_cast<T>(0))
        {
            s0 = p[0] + (p[1] - p[0]) * e0 / (e0 - e1);
            s1 = p[2] + (p[1] - p[2]) * e2 / (e2 - e1);
        }
        else
        {
            s0 = p[1] + (p[0] - p[1]) * e1 / (e1 - e0);
            s1 = p[2] + (p[0] - p[2]) * e2 / (e2 - e0);
        }
        lo = crd::math::min(s0, s1);
        hi = crd::math::max(s0, s1);
    };
    T lo1 = static_cast<T>(0);
    T hi1 = static_cast<T>(0);
    T lo2 = static_cast<T>(0);
    T hi2 = static_cast<T>(0);
    interval(p1, du0, du1, du2, lo1, hi1);
    interval(p2, dv0, dv1, dv2, lo2, hi2);
    return !(hi1 < lo2 || hi2 < lo1);
}

// Frustum ↔ OBB — positive-vertex test against each (inward) plane, in the OBB's
// frame (the plane's normal projected onto the box axes gives the support).
template <MathScalar T> [[nodiscard]] inline bool intersects(const Frustum<T>& frustum, const OBB3<T>& obb) noexcept
{
    for (const Plane<T>& plane : frustum.planes)
    {
        const T r = obb.half_extents.x * detail::abs_(crd::math::dot(plane.normal, obb.orientation.c0)) +
                    obb.half_extents.y * detail::abs_(crd::math::dot(plane.normal, obb.orientation.c1)) +
                    obb.half_extents.z * detail::abs_(crd::math::dot(plane.normal, obb.orientation.c2));
        if (signed_distance(plane, obb.center) < -r)
        {
            return false; // box entirely behind this plane
        }
    }
    return true;
}

// ===========================================================================
// 2D
// ===========================================================================

// Ray ↔ AABB2 / OBB2 (slab).
template <MathScalar T>
[[nodiscard]] constexpr bool intersect_ray2_aabb(const Ray2<T>& ray, const AABB2<T>& box, T& out_t) noexcept
{
    T tmin = static_cast<T>(0);
    T tmax = std::numeric_limits<T>::infinity();
    detail::slab(ray.origin.x, ray.direction.x, box.min.x, box.max.x, tmin, tmax);
    detail::slab(ray.origin.y, ray.direction.y, box.min.y, box.max.y, tmin, tmax);
    if (tmax < tmin)
    {
        return false;
    }
    out_t = tmin;
    return true;
}
template <MathScalar T>
[[nodiscard]] constexpr bool intersect_ray2_obb(const Ray2<T>& ray, const OBB2<T>& obb, T& out_t) noexcept
{
    const Vec2<T> p = ray.origin - obb.center;
    const Vec2<T> o(crd::math::dot(p, obb.orientation.c0), crd::math::dot(p, obb.orientation.c1));
    const Vec2<T> d(crd::math::dot(ray.direction, obb.orientation.c0),
                    crd::math::dot(ray.direction, obb.orientation.c1));
    T tmin = static_cast<T>(0);
    T tmax = std::numeric_limits<T>::infinity();
    detail::slab(o.x, d.x, -obb.half_extents.x, obb.half_extents.x, tmin, tmax);
    detail::slab(o.y, d.y, -obb.half_extents.y, obb.half_extents.y, tmin, tmax);
    if (tmax < tmin)
    {
        return false;
    }
    out_t = tmin;
    return true;
}

// Ray ↔ Line2 / Segment2 (single hit on the t-parameter of the ray).
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray2_line(const Ray2<T>& ray, const Line2<T>& line, T& out_t,
                                              T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const T denom = crd::math::cross(ray.direction, line.direction);
    if (detail::abs_(denom) < epsilon)
    {
        return false; // parallel
    }
    const Vec2<T> rel = line.point - ray.origin;
    const T t = crd::math::cross(rel, line.direction) / denom;
    if (t < static_cast<T>(0))
    {
        return false;
    }
    out_t = t;
    return true;
}
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray2_segment(const Ray2<T>& ray, const Segment2<T>& seg, T& out_t,
                                                 T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const Vec2<T> sd = seg.b - seg.a;
    const T denom = crd::math::cross(ray.direction, sd);
    if (detail::abs_(denom) < epsilon)
    {
        return false;
    }
    const Vec2<T> rel = seg.a - ray.origin;
    const T t = crd::math::cross(rel, sd) / denom;
    const T u = crd::math::cross(rel, ray.direction) / denom;
    if (t < static_cast<T>(0) || u < static_cast<T>(0) || u > static_cast<T>(1))
    {
        return false;
    }
    out_t = t;
    return true;
}

// Ray ↔ Circle / Capsule2.
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray2_circle(const Ray2<T>& ray, const Circle<T>& circle, T& out_t,
                                                T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const Vec2<T> oc = ray.origin - circle.center;
    const T a = crd::math::dot(ray.direction, ray.direction);
    if (!(a > epsilon))
    {
        return false;
    }
    const T b = static_cast<T>(2) * crd::math::dot(oc, ray.direction);
    const T c = crd::math::dot(oc, oc) - circle.radius * circle.radius;
    const T disc = b * b - static_cast<T>(4) * a * c;
    if (disc < static_cast<T>(0))
    {
        return false;
    }
    const T sq = static_cast<T>(std::sqrt(disc));
    const T inv2a = static_cast<T>(0.5) / a;
    for (const T t : {(-b - sq) * inv2a, (-b + sq) * inv2a})
    {
        if (t >= static_cast<T>(0))
        {
            out_t = t;
            return true;
        }
    }
    return false;
}
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray2_capsule(const Ray2<T>& ray, const Capsule2<T>& cap, T& out_t,
                                                 T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    // A 2D capsule's boundary is two semicircles + two parallel segments offset
    // by ±r from the spine. Cheap robust route: march the ray and test the swept
    // distance — but for v0c the simplest exact-enough form is: the two cap
    // circles + the two flank segments (perp-offset spine).
    T best = std::numeric_limits<T>::infinity();
    bool hit = false;
    for (const Vec2<T>& centre : {cap.a, cap.b})
    {
        T t = static_cast<T>(0);
        if (intersect_ray2_circle(ray, Circle<T>(centre, cap.radius), t, epsilon) && t < best)
        {
            best = t;
            hit = true;
        }
    }
    const Vec2<T> axis = cap.b - cap.a;
    const T len = static_cast<T>(std::sqrt(crd::math::dot(axis, axis)));
    if (len > std::numeric_limits<T>::min())
    {
        const Vec2<T> n = perp(axis) * (cap.radius / len);
        for (const Vec2<T>& off : {n, Vec2<T>(-n.x, -n.y)})
        {
            T t = static_cast<T>(0);
            if (intersect_ray2_segment(ray, Segment2<T>(cap.a + off, cap.b + off), t, epsilon) && t < best)
            {
                best = t;
                hit = true;
            }
        }
    }
    if (hit)
    {
        out_t = best;
    }
    return hit;
}
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray2_cylinder(const Ray2<T>& ray, const Cylinder2<T>& cyl, T& out_t) noexcept
{
    // A 2D cylinder is the rectangle of half-width r along a→b — i.e. an OBB2.
    const Vec2<T> axis = cyl.b - cyl.a;
    const T len = static_cast<T>(std::sqrt(crd::math::dot(axis, axis)));
    if (!(len > std::numeric_limits<T>::min()))
    {
        return false;
    }
    const Vec2<T> u = axis * (static_cast<T>(1) / len);
    const Vec2<T> v = perp(u);
    const OBB2<T> obb((cyl.a + cyl.b) * static_cast<T>(0.5), Vec2<T>(len * static_cast<T>(0.5), cyl.radius),
                      crd::math::Mat2<T>(u, v));
    return intersect_ray2_obb(ray, obb, out_t);
}

// `point` inside the 2D cylinder (= the rectangle).
template <MathScalar T> [[nodiscard]] inline bool contains(const Cylinder2<T>& cyl, const Vec2<T>& point) noexcept
{
    const Vec2<T> axis = cyl.b - cyl.a;
    const T len_sq = crd::math::dot(axis, axis);
    if (!(len_sq > std::numeric_limits<T>::min()))
    {
        return false;
    }
    const Vec2<T> ap = point - cyl.a;
    const T axial = crd::math::dot(ap, axis);
    if (axial < static_cast<T>(0) || axial > len_sq)
    {
        return false;
    }
    const T radial = crd::math::dot(ap, ap) - axial * axial / len_sq;
    return radial <= cyl.radius * cyl.radius;
}

// Segment ↔ Segment (2D): do the two closed segments share a point?
template <MathScalar T>
[[nodiscard]] constexpr bool segments_intersect(const Segment2<T>& s1, const Segment2<T>& s2) noexcept
{
    const T o1 = detail::orient2d(s1.a, s1.b, s2.a);
    const T o2 = detail::orient2d(s1.a, s1.b, s2.b);
    const T o3 = detail::orient2d(s2.a, s2.b, s1.a);
    const T o4 = detail::orient2d(s2.a, s2.b, s1.b);
    if (o1 * o2 < static_cast<T>(0) && o3 * o4 < static_cast<T>(0))
    {
        return true; // proper crossing
    }
    // Collinear / endpoint-touching cases.
    const auto on_seg = [](const Vec2<T>& a, const Vec2<T>& b, const Vec2<T>& p)
    {
        return crd::math::min(a.x, b.x) <= p.x && p.x <= crd::math::max(a.x, b.x) && crd::math::min(a.y, b.y) <= p.y &&
               p.y <= crd::math::max(a.y, b.y);
    };
    if (o1 == static_cast<T>(0) && on_seg(s1.a, s1.b, s2.a))
    {
        return true;
    }
    if (o2 == static_cast<T>(0) && on_seg(s1.a, s1.b, s2.b))
    {
        return true;
    }
    if (o3 == static_cast<T>(0) && on_seg(s2.a, s2.b, s1.a))
    {
        return true;
    }
    if (o4 == static_cast<T>(0) && on_seg(s2.a, s2.b, s1.b))
    {
        return true;
    }
    return false;
}
// Variant that also returns the intersection point when the crossing is proper.
template <MathScalar T>
[[nodiscard]] inline bool segments_intersect(const Segment2<T>& s1, const Segment2<T>& s2, Vec2<T>& out_point) noexcept
{
    const Vec2<T> d1 = s1.b - s1.a;
    const Vec2<T> d2 = s2.b - s2.a;
    const T denom = crd::math::cross(d1, d2);
    if (detail::abs_(denom) < std::numeric_limits<T>::min())
    {
        return segments_intersect(s1, s2); // parallel / collinear — no single point
    }
    const Vec2<T> r = s2.a - s1.a;
    const T t = crd::math::cross(r, d2) / denom;
    const T u = crd::math::cross(r, d1) / denom;
    if (t < static_cast<T>(0) || t > static_cast<T>(1) || u < static_cast<T>(0) || u > static_cast<T>(1))
    {
        return false;
    }
    out_point = s1.a + d1 * t;
    return true;
}

// OBB2 ↔ OBB2 — 4-axis SAT (2 normals per box).
template <MathScalar T> [[nodiscard]] inline bool intersects(const OBB2<T>& a, const OBB2<T>& b) noexcept
{
    const Vec2<T> axes[4] = {a.orientation.c0, a.orientation.c1, b.orientation.c0, b.orientation.c1};
    const Vec2<T> tw = b.center - a.center;
    for (const Vec2<T>& L : axes)
    {
        const T ra = a.half_extents.x * detail::abs_(crd::math::dot(L, a.orientation.c0)) +
                     a.half_extents.y * detail::abs_(crd::math::dot(L, a.orientation.c1));
        const T rb = b.half_extents.x * detail::abs_(crd::math::dot(L, b.orientation.c0)) +
                     b.half_extents.y * detail::abs_(crd::math::dot(L, b.orientation.c1));
        if (detail::abs_(crd::math::dot(tw, L)) > ra + rb)
        {
            return false;
        }
    }
    return true;
}

// Triangle2 ↔ Triangle2 — SAT over the 6 edge normals.
template <MathScalar T> [[nodiscard]] inline bool intersects(const Triangle2<T>& a, const Triangle2<T>& b) noexcept
{
    const Vec2<T> av[3] = {a.a, a.b, a.c};
    const Vec2<T> bv[3] = {b.a, b.b, b.c};
    const auto test_axes = [&](const Vec2<T> tri[3]) -> bool
    {
        for (int i = 0; i < 3; ++i)
        {
            const Vec2<T> edge = tri[(i + 1) % 3] - tri[i];
            const Vec2<T> n = perp(edge);
            T amn = std::numeric_limits<T>::infinity();
            T amx = -std::numeric_limits<T>::infinity();
            T bmn = std::numeric_limits<T>::infinity();
            T bmx = -std::numeric_limits<T>::infinity();
            for (int k = 0; k < 3; ++k)
            {
                const T pa = crd::math::dot(n, av[k]);
                const T pb = crd::math::dot(n, bv[k]);
                amn = crd::math::min(amn, pa);
                amx = crd::math::max(amx, pa);
                bmn = crd::math::min(bmn, pb);
                bmx = crd::math::max(bmx, pb);
            }
            if (amx < bmn || bmx < amn)
            {
                return true; // separated on this axis
            }
        }
        return false;
    };
    return !test_axes(av) && !test_axes(bv);
}

// Circle ↔ X (exact; reduces to v0b distance-squared).
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Circle<T>& c, const AABB2<T>& box) noexcept
{
    return distance_squared(box, c.center) <= c.radius * c.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Circle<T>& c, const OBB2<T>& obb) noexcept
{
    return distance_squared(obb, c.center) <= c.radius * c.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Circle<T>& c, const Triangle2<T>& tri) noexcept
{
    return distance_squared(tri, c.center) <= c.radius * c.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Circle<T>& c, const Segment2<T>& seg) noexcept
{
    return distance_squared(seg, c.center) <= c.radius * c.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Circle<T>& c, const Line2<T>& line) noexcept
{
    return distance_squared(line, c.center) <= c.radius * c.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Circle<T>& c, const Capsule2<T>& cap) noexcept
{
    const T rr = c.radius + cap.radius;
    return distance_squared(Segment2<T>(cap.a, cap.b), c.center) <= rr * rr;
}

// Triangle2 ↔ AABB2 / Segment2 / Circle (the swapped Circle form).
template <MathScalar T> [[nodiscard]] inline bool intersects(const Triangle2<T>& tri, const AABB2<T>& box) noexcept
{
    if (contains(box, tri.a) || contains(box, tri.b) || contains(box, tri.c))
    {
        return true;
    }
    if (contains(tri, box.min) || contains(tri, box.max) || contains(tri, Vec2<T>(box.min.x, box.max.y)) ||
        contains(tri, Vec2<T>(box.max.x, box.min.y)))
    {
        return true;
    }
    const Segment2<T> te[3] = {Segment2<T>(tri.a, tri.b), Segment2<T>(tri.b, tri.c), Segment2<T>(tri.c, tri.a)};
    const Vec2<T> bc[4] = {box.min, Vec2<T>(box.max.x, box.min.y), box.max, Vec2<T>(box.min.x, box.max.y)};
    for (const Segment2<T>& e : te)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (segments_intersect(e, Segment2<T>(bc[i], bc[(i + 1) % 4])))
            {
                return true;
            }
        }
    }
    return false;
}
template <MathScalar T> [[nodiscard]] inline bool intersects(const Triangle2<T>& tri, const Segment2<T>& seg) noexcept
{
    if (contains(tri, seg.a) || contains(tri, seg.b))
    {
        return true;
    }
    return segments_intersect(seg, Segment2<T>(tri.a, tri.b)) || segments_intersect(seg, Segment2<T>(tri.b, tri.c)) ||
           segments_intersect(seg, Segment2<T>(tri.c, tri.a));
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Triangle2<T>& tri, const Circle<T>& c) noexcept
{
    return intersects(c, tri);
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const AABB2<T>& box, const Triangle2<T>& tri) noexcept
{
    return intersects(tri, box);
}

// Capsule2 ↔ Capsule2 / AABB2 / OBB2.
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Capsule2<T>& a, const Capsule2<T>& b) noexcept
{
    const T rr = a.radius + b.radius;
    return distance_squared(Segment2<T>(a.a, a.b), Segment2<T>(b.a, b.b)) <= rr * rr;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Capsule2<T>& cap, const Circle<T>& c) noexcept
{
    return intersects(c, cap);
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Capsule2<T>& cap, const AABB2<T>& box) noexcept
{
    const Vec2<T> r(cap.radius, cap.radius);
    const AABB2<T> grown(box.min - r, box.max + r);
    T tmin = static_cast<T>(0);
    T tmax = static_cast<T>(1);
    const Vec2<T> d = cap.b - cap.a;
    detail::slab(cap.a.x, d.x, grown.min.x, grown.max.x, tmin, tmax);
    detail::slab(cap.a.y, d.y, grown.min.y, grown.max.y, tmin, tmax);
    return tmax >= tmin;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Capsule2<T>& cap, const OBB2<T>& obb) noexcept
{
    const auto local = [&](const Vec2<T>& w)
    {
        const Vec2<T> p = w - obb.center;
        return Vec2<T>(crd::math::dot(p, obb.orientation.c0), crd::math::dot(p, obb.orientation.c1));
    };
    return intersects(Capsule2<T>(local(cap.a), local(cap.b), cap.radius),
                      AABB2<T>(Vec2<T>(-obb.half_extents.x, -obb.half_extents.y), obb.half_extents));
}

// Line2 ↔ AABB2 / OBB2 — does the infinite line cut the box?
template <MathScalar T> [[nodiscard]] inline bool intersects(const Line2<T>& line, const AABB2<T>& box) noexcept
{
    // The box straddles the line iff its projection onto the line's left-normal
    // spans the line's offset.
    const Vec2<T> n = perp(line.direction);
    const Vec2<T> c = center(box);
    const Vec2<T> e = extents(box);
    const T r = e.x * detail::abs_(n.x) + e.y * detail::abs_(n.y);
    return detail::abs_(crd::math::dot(n, c - line.point)) <= r;
}
template <MathScalar T> [[nodiscard]] inline bool intersects(const Line2<T>& line, const OBB2<T>& obb) noexcept
{
    const Vec2<T> n = perp(line.direction);
    const T r = obb.half_extents.x * detail::abs_(crd::math::dot(n, obb.orientation.c0)) +
                obb.half_extents.y * detail::abs_(crd::math::dot(n, obb.orientation.c1));
    return detail::abs_(crd::math::dot(n, obb.center - line.point)) <= r;
}

} // namespace crd::geometry::primitives
