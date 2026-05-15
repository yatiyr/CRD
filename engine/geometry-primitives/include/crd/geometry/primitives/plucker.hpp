#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — Plücker-coordinate line/edge classification (v0f).
//
// A directed line through points p → q has Plücker coordinates `(d, m)` with
// direction `d = q − p` and moment `m = p × q`. The *side* of one Plücker line
// relative to another is the bilinear `permuted dot`
//     side(a, b) = dot(a.d, b.m) + dot(b.d, a.m)
// — sign-only, totally branchless, `Vec8f`-batchable. > 0 / < 0 say which side;
// **= 0 means the two lines are coplanar / one passes through the other**.
//
// Determinism (ADR-0076 §4 #13): the sum order in `side(...)` is fixed
// (`dot(a.d, b.m)` then `+ dot(b.d, a.m)`), and a sign-zero is treated as
// "on the line" — so a ray grazing a shared triangle edge counts as a hit on
// both sides, consistently.
//
// Uses: `intersect_ray_triangle_plucker` — the cheapest ray-triangle *boolean*
// (the Plücker all-same-sign test is the reject; only on a pierce do we compute
// the one plane-`t` to clip the ray to [tnear, tmax]). Shadow rays / occlusion
// queries / culling. And `plucker_side` as a standalone segment-vs-segment
// side test. No transcendentals; `constexpr`-friendly.
// ---------------------------------------------------------------------------

#include <crd/geometry/primitives/primitives.hpp>

#include <limits>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::MathValue;
using crd::math::Vec3;

// Plücker coordinates of a directed line: direction + moment about the origin.
template <MathScalar T> struct PluckerLine
{
    Vec3<T> d{}; // direction (q − p)
    Vec3<T> m{}; // moment (p × q)
};

// From a segment a→b: line through a, b.
template <MathScalar T> [[nodiscard]] constexpr PluckerLine<T> plucker_from(const Segment3<T>& seg) noexcept
{
    return PluckerLine<T>{seg.b - seg.a, crd::math::cross(seg.a, seg.b)};
}
// From a ray: line through `origin` and `origin + direction` (m = origin × dir).
template <MathScalar T> [[nodiscard]] constexpr PluckerLine<T> plucker_from(const Ray3<T>& ray) noexcept
{
    return PluckerLine<T>{ray.direction, crd::math::cross(ray.origin, ray.direction)};
}
// From an infinite line {point + t·direction}: m = point × (point + direction) = point × direction.
template <MathScalar T> [[nodiscard]] constexpr PluckerLine<T> plucker_from(const Line3<T>& line) noexcept
{
    return PluckerLine<T>{line.direction, crd::math::cross(line.point, line.direction)};
}
// From two points p → q directly.
template <MathScalar T> [[nodiscard]] constexpr PluckerLine<T> plucker_from(const Vec3<T>& p, const Vec3<T>& q) noexcept
{
    return PluckerLine<T>{q - p, crd::math::cross(p, q)};
}

// The signed side of line `a` relative to line `b` (fixed sum order). > 0 / < 0
// = the two are skew on one side / the other; == 0 = coplanar (intersecting or
// parallel).
template <MathScalar T>
[[nodiscard]] constexpr T plucker_side(const PluckerLine<T>& a, const PluckerLine<T>& b) noexcept
{
    return crd::math::dot(a.d, b.m) + crd::math::dot(b.d, a.m);
}

// Cheapest ray↔triangle *boolean*. The ray's line pierces the triangle iff it
// is on the same (non-strictly) side of all three directed edges a→b, b→c,
// c→a (a sign-zero on an edge counts — watertight on shared edges). On a
// pierce, the one plane-`t` is computed and clipped to [tnear, tmax].
template <MathScalar T>
[[nodiscard]] inline bool
intersect_ray_triangle_plucker(const Ray3<T>& ray, const Triangle3<T>& tri, T tmax = std::numeric_limits<T>::infinity(),
                               T tnear = static_cast<T>(0), T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const PluckerLine<T> r = plucker_from(ray);
    const T s1 = plucker_side(r, plucker_from(tri.a, tri.b));
    const T s2 = plucker_side(r, plucker_from(tri.b, tri.c));
    const T s3 = plucker_side(r, plucker_from(tri.c, tri.a));
    const bool all_nonneg = s1 >= static_cast<T>(0) && s2 >= static_cast<T>(0) && s3 >= static_cast<T>(0);
    const bool all_nonpos = s1 <= static_cast<T>(0) && s2 <= static_cast<T>(0) && s3 <= static_cast<T>(0);
    if (!(all_nonneg || all_nonpos))
    {
        return false; // the line misses the triangle
    }
    // The line pierces — clip the ray to [tnear, tmax] via the plane hit.
    const Vec3<T> n = crd::math::cross(tri.b - tri.a, tri.c - tri.a);
    const T denom = crd::math::dot(n, ray.direction);
    if (denom < epsilon && denom > -epsilon)
    {
        return false; // ray parallel to the triangle plane (degenerate / grazing)
    }
    const T t = crd::math::dot(n, tri.a - ray.origin) / denom;
    return t >= tnear && t <= tmax;
}

} // namespace crd::geometry::primitives
