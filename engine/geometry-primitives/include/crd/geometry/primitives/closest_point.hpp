#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — closest-point catalogue (Phase 3.1.7 v0b).
//
// "Given a point, find the nearest point ON shape X" — and the obvious
// `distance` / `distance_squared` companions — for every 2D and 3D primitive in
// `primitives.hpp`. Plus segment-vs-segment (the two mutually-closest points),
// which capsule-vs-capsule, character-controller sweeps, and the GJK fallback
// path all lean on.
//
// Algorithm notes (these get exercised by physics, robotics, mechanics, PCB
// sims, animation, games — so the exact, branch-light forms, not the lazy ones):
//   * Triangle: Christer Ericson, "Real-Time Collision Detection" §5.1.5 — the
//     7-Voronoi-region dot-product cascade (3 vertex / 3 edge / 1 face), not
//     plane-project-then-clamp-barycentrics. Specialises cleanly to 2D.
//   * Segment↔segment: Ericson §5.1.9 — robust on parallel and on degenerate
//     (zero-length) segments.
//   * AABB / OBB: clamp the query into the box's frame (Ericson §5.1.3/5.1.4).
//   * Sphere / Circle / Capsule: closest point on the *surface* — `center +
//     radius · dir(p)` (so it's well-defined for `p` inside, too). A degenerate
//     direction (`p` exactly on the center / spine) resolves to a fixed axis
//     direction so the result is deterministic.
//
// Degenerate-input contract: a zero-length segment / zero-direction line·ray
// collapses to its anchor point; a flat (collinear-vertex) triangle falls back
// to the nearest of its three edges; the squared-length guards use
// `numeric_limits<T>::min()` so working at sub-millimetre scales (PCB) is not
// silently rounded to "this is a point".
//
// `closest_point(Plane,·)` / `closest_point(AABB3,·)` already live in
// `primitives.hpp` (they were migrated there in v0a); this header only adds
// their `distance` / `distance_squared` companions, never redefines them.
//
// The dimension-agnostic cores (triangle cascade, segment-segment, the linear
// projection) are written once over a vector concept and reused by both the 2D
// and 3D public overloads — 2D/3D parity by construction.
// ---------------------------------------------------------------------------

#include <crd/geometry/primitives/primitives.hpp>

#include <cmath>
#include <limits>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::Vec2;
using crd::math::Vec3;

namespace detail
{
// Squared-length below which a direction / segment is "truly" degenerate.
template <MathScalar T> [[nodiscard]] constexpr T cp_zero_sq() noexcept
{
    return std::numeric_limits<T>::min();
}

// Parameter t of the closest point of `p` on the line { o + t·d }, clamped to
// [lo, hi]. lo=-inf,hi=+inf → infinite line; lo=0,hi=+inf → ray; lo=0,hi=1 →
// segment a→b (with o=a, d=b-a). A degenerate direction returns 0 (so the
// caller's `o + 0·d = o` — the line/ray/segment collapses to its anchor).
template <typename V, MathScalar T>
[[nodiscard]] constexpr T cp_line_param(const V& o, const V& d, const V& p, T lo, T hi) noexcept
{
    const T dd = crd::math::dot(d, d);
    if (!(dd > cp_zero_sq<T>()))
    {
        return static_cast<T>(0);
    }
    return crd::math::clamp(crd::math::dot(p - o, d) / dd, lo, hi);
}

// Ericson §5.1.5 ClosestPtPointTriangle — Voronoi-region cascade. Works for any
// vector type supporting dot / +/- / *T (so: Vec2 and Vec3).
template <typename V, MathScalar T>
[[nodiscard]] constexpr V cp_triangle(const V& a, const V& b, const V& c, const V& p) noexcept
{
    const V ab = b - a;
    const V ac = c - a;
    const V ap = p - a;
    const T d1 = crd::math::dot(ab, ap);
    const T d2 = crd::math::dot(ac, ap);
    if (d1 <= static_cast<T>(0) && d2 <= static_cast<T>(0))
    {
        return a; // vertex region A
    }
    const V bp = p - b;
    const T d3 = crd::math::dot(ab, bp);
    const T d4 = crd::math::dot(ac, bp);
    if (d3 >= static_cast<T>(0) && d4 <= d3)
    {
        return b; // vertex region B
    }
    const T vc = d1 * d4 - d3 * d2;
    if (vc <= static_cast<T>(0) && d1 >= static_cast<T>(0) && d3 <= static_cast<T>(0))
    {
        const T v = d1 / (d1 - d3);
        return a + ab * v; // edge AB
    }
    const V cp = p - c;
    const T d5 = crd::math::dot(ab, cp);
    const T d6 = crd::math::dot(ac, cp);
    if (d6 >= static_cast<T>(0) && d5 <= d6)
    {
        return c; // vertex region C
    }
    const T vb = d5 * d2 - d1 * d6;
    if (vb <= static_cast<T>(0) && d2 >= static_cast<T>(0) && d6 <= static_cast<T>(0))
    {
        const T w = d2 / (d2 - d6);
        return a + ac * w; // edge AC
    }
    const T va = d3 * d6 - d5 * d4;
    if (va <= static_cast<T>(0) && (d4 - d3) >= static_cast<T>(0) && (d5 - d6) >= static_cast<T>(0))
    {
        const T w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + (c - b) * w; // edge BC
    }
    const T sum = va + vb + vc;
    // Degenerate (sliver / collinear) triangle: the face barycentric blow up.
    // Fall back to the nearest of the three edge segments.
    const T scale = crd::math::dot(ab, ab) + crd::math::dot(ac, ac) + crd::math::dot(c - b, c - b);
    if (std::abs(sum) <= crd::math::default_epsilon<T>() * (scale + std::numeric_limits<T>::min()))
    {
        const V e0 = a + ab * crd::math::clamp(d1 / (crd::math::dot(ab, ab) + std::numeric_limits<T>::min()),
                                               static_cast<T>(0), static_cast<T>(1));
        const V e1 = a + ac * crd::math::clamp(d2 / (crd::math::dot(ac, ac) + std::numeric_limits<T>::min()),
                                               static_cast<T>(0), static_cast<T>(1));
        const V bc = c - b;
        const V e2 = b + bc * crd::math::clamp(crd::math::dot(bc, p - b) /
                                                   (crd::math::dot(bc, bc) + std::numeric_limits<T>::min()),
                                               static_cast<T>(0), static_cast<T>(1));
        const T q0 = crd::math::dot(p - e0, p - e0);
        const T q1 = crd::math::dot(p - e1, p - e1);
        const T q2 = crd::math::dot(p - e2, p - e2);
        if (q0 <= q1 && q0 <= q2)
        {
            return e0;
        }
        return q1 <= q2 ? e1 : e2;
    }
    const T denom = static_cast<T>(1) / sum;
    return a + ab * (vb * denom) + ac * (vc * denom); // interior of the face
}

// Ericson §5.1.9 ClosestPtSegmentSegment — robust on parallel / degenerate.
template <typename V, MathScalar T>
constexpr void cp_segment_segment(const V& p1, const V& q1, const V& p2, const V& q2, V& out_c1, V& out_c2) noexcept
{
    const V d1 = q1 - p1; // direction of segment 1
    const V d2 = q2 - p2; // direction of segment 2
    const V r = p1 - p2;
    const T a = crd::math::dot(d1, d1); // squared length of segment 1
    const T e = crd::math::dot(d2, d2); // squared length of segment 2
    const T f = crd::math::dot(d2, r);
    const T eps = cp_zero_sq<T>();

    T s = static_cast<T>(0);
    T t = static_cast<T>(0);
    if (a <= eps && e <= eps)
    {
        // Both segments are points.
    }
    else if (a <= eps)
    {
        // Segment 1 is a point.
        t = crd::math::clamp(f / e, static_cast<T>(0), static_cast<T>(1));
    }
    else
    {
        const T c = crd::math::dot(d1, r);
        if (e <= eps)
        {
            // Segment 2 is a point.
            s = crd::math::clamp(-c / a, static_cast<T>(0), static_cast<T>(1));
        }
        else
        {
            const T b = crd::math::dot(d1, d2);
            const T denom = a * e - b * b; // always >= 0
            if (denom > eps)
            {
                s = crd::math::clamp((b * f - c * e) / denom, static_cast<T>(0), static_cast<T>(1));
            }
            // else: parallel — keep s = 0 (deterministic tiebreak), pick t below.
            t = (b * s + f) / e;
            if (t < static_cast<T>(0))
            {
                t = static_cast<T>(0);
                s = crd::math::clamp(-c / a, static_cast<T>(0), static_cast<T>(1));
            }
            else if (t > static_cast<T>(1))
            {
                t = static_cast<T>(1);
                s = crd::math::clamp((b - c) / a, static_cast<T>(0), static_cast<T>(1));
            }
        }
    }
    out_c1 = p1 + d1 * s;
    out_c2 = p2 + d2 * t;
}
} // namespace detail

// ===========================================================================
// 3D
// ===========================================================================

// ---- Plane (closest_point lives in primitives.hpp; companions only) --------

template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Plane<T>& plane, const Vec3<T>& p) noexcept
{
    const T sd = signed_distance(plane, p);
    return sd * sd;
}
template <MathScalar T> [[nodiscard]] constexpr T distance(const Plane<T>& plane, const Vec3<T>& p) noexcept
{
    return std::abs(signed_distance(plane, p));
}

// ---- Line3 ----------------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> closest_point(const Line3<T>& line, const Vec3<T>& p) noexcept
{
    const T tt = detail::cp_line_param<Vec3<T>, T>(line.point, line.direction, p, -std::numeric_limits<T>::infinity(),
                                                   std::numeric_limits<T>::infinity());
    return line.point + line.direction * tt;
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Line3<T>& line, const Vec3<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(line, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Line3<T>& line, const Vec3<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(line, p)));
}

// ---- Segment3 -------------------------------------------------------------

// Parameter t ∈ [0,1] of the closest point on a→b: closest point = a + t·(b−a).
template <MathScalar T> [[nodiscard]] constexpr T closest_param(const Segment3<T>& seg, const Vec3<T>& p) noexcept
{
    return detail::cp_line_param<Vec3<T>, T>(seg.a, seg.b - seg.a, p, static_cast<T>(0), static_cast<T>(1));
}
template <MathScalar T> [[nodiscard]] constexpr Vec3<T> closest_point(const Segment3<T>& seg, const Vec3<T>& p) noexcept
{
    return seg.a + (seg.b - seg.a) * closest_param(seg, p);
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Segment3<T>& seg, const Vec3<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(seg, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Segment3<T>& seg, const Vec3<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(seg, p)));
}

// ---- Ray3 -----------------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> closest_point(const Ray3<T>& ray, const Vec3<T>& p) noexcept
{
    const T tt = detail::cp_line_param<Vec3<T>, T>(ray.origin, ray.direction, p, static_cast<T>(0),
                                                   std::numeric_limits<T>::infinity());
    return ray.origin + ray.direction * tt;
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Ray3<T>& ray, const Vec3<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(ray, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Ray3<T>& ray, const Vec3<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(ray, p)));
}

// ---- Triangle3 (Ericson Voronoi-region) -----------------------------------

template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> closest_point(const Triangle3<T>& tri, const Vec3<T>& p) noexcept
{
    return detail::cp_triangle<Vec3<T>, T>(tri.a, tri.b, tri.c, p);
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Triangle3<T>& tri, const Vec3<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(tri, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Triangle3<T>& tri, const Vec3<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(tri, p)));
}

// ---- AABB3 (closest_point lives in primitives.hpp; companions only) -------

template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const AABB3<T>& box, const Vec3<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(box, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const AABB3<T>& box, const Vec3<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(box, p)));
}

// ---- OBB3 -----------------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> closest_point(const OBB3<T>& obb, const Vec3<T>& p) noexcept
{
    const Vec3<T> d = p - obb.center;
    Vec3<T> result = obb.center;
    const T t0 = crd::math::clamp(crd::math::dot(d, obb.orientation.c0), -obb.half_extents.x, obb.half_extents.x);
    result = result + obb.orientation.c0 * t0;
    const T t1 = crd::math::clamp(crd::math::dot(d, obb.orientation.c1), -obb.half_extents.y, obb.half_extents.y);
    result = result + obb.orientation.c1 * t1;
    const T t2 = crd::math::clamp(crd::math::dot(d, obb.orientation.c2), -obb.half_extents.z, obb.half_extents.z);
    result = result + obb.orientation.c2 * t2;
    return result;
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const OBB3<T>& obb, const Vec3<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(obb, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const OBB3<T>& obb, const Vec3<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(obb, p)));
}

// ---- Sphere (closest point on the *surface*) ------------------------------

template <MathScalar T> [[nodiscard]] inline Vec3<T> closest_point(const Sphere<T>& sphere, const Vec3<T>& p) noexcept
{
    const Vec3<T> v = p - sphere.center;
    const T len_sq = crd::math::dot(v, v);
    if (!(len_sq > std::numeric_limits<T>::min()))
    {
        return sphere.center + Vec3<T>(sphere.radius, static_cast<T>(0), static_cast<T>(0)); // deterministic tiebreak
    }
    return sphere.center + v * (sphere.radius / static_cast<T>(std::sqrt(len_sq)));
}
template <MathScalar T> [[nodiscard]] inline T distance_squared(const Sphere<T>& sphere, const Vec3<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(sphere, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Sphere<T>& sphere, const Vec3<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(sphere, p)));
}

// ---- Capsule3 (closest point on the *surface*) ----------------------------

template <MathScalar T> [[nodiscard]] inline Vec3<T> closest_point(const Capsule3<T>& cap, const Vec3<T>& p) noexcept
{
    const Vec3<T> s = closest_point(Segment3<T>(cap.a, cap.b), p);
    const Vec3<T> v = p - s;
    const T len_sq = crd::math::dot(v, v);
    if (!(len_sq > std::numeric_limits<T>::min()))
    {
        return s + Vec3<T>(cap.radius, static_cast<T>(0), static_cast<T>(0)); // deterministic tiebreak
    }
    return s + v * (cap.radius / static_cast<T>(std::sqrt(len_sq)));
}
template <MathScalar T> [[nodiscard]] inline T distance_squared(const Capsule3<T>& cap, const Vec3<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(cap, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Capsule3<T>& cap, const Vec3<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(cap, p)));
}

// ---- Segment3 ↔ Segment3 (mutually-closest point pair) --------------------

template <MathScalar T>
constexpr void closest_points(const Segment3<T>& seg1, const Segment3<T>& seg2, Vec3<T>& out_c1,
                              Vec3<T>& out_c2) noexcept
{
    detail::cp_segment_segment<Vec3<T>, T>(seg1.a, seg1.b, seg2.a, seg2.b, out_c1, out_c2);
}
template <MathScalar T>
[[nodiscard]] constexpr T distance_squared(const Segment3<T>& seg1, const Segment3<T>& seg2) noexcept
{
    Vec3<T> c1{};
    Vec3<T> c2{};
    closest_points(seg1, seg2, c1, c2);
    return crd::math::distance_squared(c1, c2);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Segment3<T>& seg1, const Segment3<T>& seg2) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(seg1, seg2)));
}

// ===========================================================================
// 2D
// ===========================================================================

// Left-perpendicular of a 2D vector: rotate +90°. perp((x,y)) = (-y, x).
template <MathScalar T> [[nodiscard]] constexpr Vec2<T> perp(const Vec2<T>& v) noexcept
{
    return Vec2<T>(-v.y, v.x);
}

// ---- Line2 ----------------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> closest_point(const Line2<T>& line, const Vec2<T>& p) noexcept
{
    const T tt = detail::cp_line_param<Vec2<T>, T>(line.point, line.direction, p, -std::numeric_limits<T>::infinity(),
                                                   std::numeric_limits<T>::infinity());
    return line.point + line.direction * tt;
}
// Signed perpendicular distance, sign = which side (positive on the side the
// left-normal `perp(direction)` points to). This is `Line2`'s `Plane`-analog.
template <MathScalar T> [[nodiscard]] inline T signed_distance(const Line2<T>& line, const Vec2<T>& p) noexcept
{
    const T len = static_cast<T>(std::sqrt(crd::math::dot(line.direction, line.direction)));
    if (!(len > std::numeric_limits<T>::min()))
    {
        return static_cast<T>(0);
    }
    return crd::math::dot(perp(line.direction), p - line.point) / len;
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Line2<T>& line, const Vec2<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(line, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Line2<T>& line, const Vec2<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(line, p)));
}

// ---- Segment2 -------------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr T closest_param(const Segment2<T>& seg, const Vec2<T>& p) noexcept
{
    return detail::cp_line_param<Vec2<T>, T>(seg.a, seg.b - seg.a, p, static_cast<T>(0), static_cast<T>(1));
}
template <MathScalar T> [[nodiscard]] constexpr Vec2<T> closest_point(const Segment2<T>& seg, const Vec2<T>& p) noexcept
{
    return seg.a + (seg.b - seg.a) * closest_param(seg, p);
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Segment2<T>& seg, const Vec2<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(seg, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Segment2<T>& seg, const Vec2<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(seg, p)));
}

// ---- Ray2 -----------------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> closest_point(const Ray2<T>& ray, const Vec2<T>& p) noexcept
{
    const T tt = detail::cp_line_param<Vec2<T>, T>(ray.origin, ray.direction, p, static_cast<T>(0),
                                                   std::numeric_limits<T>::infinity());
    return ray.origin + ray.direction * tt;
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Ray2<T>& ray, const Vec2<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(ray, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Ray2<T>& ray, const Vec2<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(ray, p)));
}

// ---- Triangle2 (Ericson Voronoi-region, planar) ---------------------------

template <MathScalar T>
[[nodiscard]] constexpr Vec2<T> closest_point(const Triangle2<T>& tri, const Vec2<T>& p) noexcept
{
    return detail::cp_triangle<Vec2<T>, T>(tri.a, tri.b, tri.c, p);
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Triangle2<T>& tri, const Vec2<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(tri, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Triangle2<T>& tri, const Vec2<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(tri, p)));
}

// ---- AABB2 ----------------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> closest_point(const AABB2<T>& box, const Vec2<T>& p) noexcept
{
    return Vec2<T>(crd::math::clamp(p.x, box.min.x, box.max.x), crd::math::clamp(p.y, box.min.y, box.max.y));
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const AABB2<T>& box, const Vec2<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(box, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const AABB2<T>& box, const Vec2<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(box, p)));
}

// ---- OBB2 -----------------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> closest_point(const OBB2<T>& obb, const Vec2<T>& p) noexcept
{
    const Vec2<T> d = p - obb.center;
    Vec2<T> result = obb.center;
    const T t0 = crd::math::clamp(crd::math::dot(d, obb.orientation.c0), -obb.half_extents.x, obb.half_extents.x);
    result = result + obb.orientation.c0 * t0;
    const T t1 = crd::math::clamp(crd::math::dot(d, obb.orientation.c1), -obb.half_extents.y, obb.half_extents.y);
    result = result + obb.orientation.c1 * t1;
    return result;
}
template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const OBB2<T>& obb, const Vec2<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(obb, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const OBB2<T>& obb, const Vec2<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(obb, p)));
}

// ---- Circle (closest point on the *boundary*) -----------------------------

template <MathScalar T> [[nodiscard]] inline Vec2<T> closest_point(const Circle<T>& circle, const Vec2<T>& p) noexcept
{
    const Vec2<T> v = p - circle.center;
    const T len_sq = crd::math::dot(v, v);
    if (!(len_sq > std::numeric_limits<T>::min()))
    {
        return circle.center + Vec2<T>(circle.radius, static_cast<T>(0)); // deterministic tiebreak
    }
    return circle.center + v * (circle.radius / static_cast<T>(std::sqrt(len_sq)));
}
template <MathScalar T> [[nodiscard]] inline T distance_squared(const Circle<T>& circle, const Vec2<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(circle, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Circle<T>& circle, const Vec2<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(circle, p)));
}

// ---- Capsule2 (closest point on the *boundary*) ---------------------------

template <MathScalar T> [[nodiscard]] inline Vec2<T> closest_point(const Capsule2<T>& cap, const Vec2<T>& p) noexcept
{
    const Vec2<T> s = closest_point(Segment2<T>(cap.a, cap.b), p);
    const Vec2<T> v = p - s;
    const T len_sq = crd::math::dot(v, v);
    if (!(len_sq > std::numeric_limits<T>::min()))
    {
        return s + Vec2<T>(cap.radius, static_cast<T>(0)); // deterministic tiebreak
    }
    return s + v * (cap.radius / static_cast<T>(std::sqrt(len_sq)));
}
template <MathScalar T> [[nodiscard]] inline T distance_squared(const Capsule2<T>& cap, const Vec2<T>& p) noexcept
{
    return crd::math::distance_squared(closest_point(cap, p), p);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Capsule2<T>& cap, const Vec2<T>& p) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(cap, p)));
}

// ---- Segment2 ↔ Segment2 (mutually-closest point pair) --------------------

template <MathScalar T>
constexpr void closest_points(const Segment2<T>& seg1, const Segment2<T>& seg2, Vec2<T>& out_c1,
                              Vec2<T>& out_c2) noexcept
{
    detail::cp_segment_segment<Vec2<T>, T>(seg1.a, seg1.b, seg2.a, seg2.b, out_c1, out_c2);
}
template <MathScalar T>
[[nodiscard]] constexpr T distance_squared(const Segment2<T>& seg1, const Segment2<T>& seg2) noexcept
{
    Vec2<T> c1{};
    Vec2<T> c2{};
    closest_points(seg1, seg2, c1, c2);
    return crd::math::distance_squared(c1, c2);
}
template <MathScalar T> [[nodiscard]] inline T distance(const Segment2<T>& seg1, const Segment2<T>& seg2) noexcept
{
    return static_cast<T>(std::sqrt(distance_squared(seg1, seg2)));
}

} // namespace crd::geometry::primitives
