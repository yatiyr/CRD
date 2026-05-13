#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — analytic signed-distance functions (Phase 3.1.7
// v1h, ADR-0076 §15).
//
// Inigo Quilez's canonical closed-form SDFs (iquilezles.org/articles/
// distfunctions, /distfunctions2d), translated to C++ over `crd::math::Vec2/3`.
// `sd_*(p, params...)` returns the signed distance from a query point `p` (in
// the shape's local frame — translate / rotate the point into the frame at the
// call site) to the shape's surface: negative inside, zero on the boundary,
// positive outside. For shapes with no interior (a triangle / segment in 3D)
// the result is the unsigned distance — noted per function.
//
// This is the C++ *scalar reference* `crd-geometry-shader-helpers` (v9e) emits
// GLSL/HLSL twins of and `crd-sdf` v0 reuses; `closest_point.hpp` gives the
// (unsigned) distance + closest point on the *primitive structs* in world
// space, this gives the signed value in shape-local space the way SDF pipelines
// expect it. The two agree in magnitude on the surface and outside.
//
// Determinism (ADR-0076 §4): every function here is `sqrt` / `abs` / `min` /
// `max` / `clamp` / `dot` / `length` only — no transcendental libm (the
// `crd-no-std-math-check` guard covers `engine/geometry-primitives`). Constants
// like `sqrt(3)` / the regular-polygon vertex cosines are pre-evaluated
// literals.
// ---------------------------------------------------------------------------

#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <cmath>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::Vec2;
using crd::math::Vec3;

namespace sd_detail
{
// Component-wise vector helpers used by the iq translations (kept local — the
// lean `crd-math` substrate does not carry these).
template <MathScalar T> [[nodiscard]] constexpr T sgn(T x) noexcept
{
    return x > static_cast<T>(0) ? static_cast<T>(1) : (x < static_cast<T>(0) ? static_cast<T>(-1) : static_cast<T>(0));
}
template <MathScalar T> [[nodiscard]] inline Vec2<T> vabs(const Vec2<T>& v) noexcept
{
    return Vec2<T>(std::abs(v.x), std::abs(v.y));
}
template <MathScalar T> [[nodiscard]] inline Vec3<T> vabs(const Vec3<T>& v) noexcept
{
    return Vec3<T>(std::abs(v.x), std::abs(v.y), std::abs(v.z));
}
template <MathScalar T> [[nodiscard]] constexpr Vec2<T> vmax0(const Vec2<T>& v) noexcept
{
    return Vec2<T>(crd::math::max(v.x, static_cast<T>(0)), crd::math::max(v.y, static_cast<T>(0)));
}
template <MathScalar T> [[nodiscard]] constexpr Vec3<T> vmax0(const Vec3<T>& v) noexcept
{
    return Vec3<T>(crd::math::max(v.x, static_cast<T>(0)), crd::math::max(v.y, static_cast<T>(0)),
                   crd::math::max(v.z, static_cast<T>(0)));
}
template <MathScalar T> [[nodiscard]] constexpr T max3(T a, T b, T c) noexcept
{
    return crd::math::max(a, crd::math::max(b, c));
}
} // namespace sd_detail

// ===========================================================================
// 3D
// ===========================================================================

// Sphere of radius `r` centred at the origin.
template <MathScalar T> [[nodiscard]] inline T sd_sphere(const Vec3<T>& p, T r) noexcept
{
    return crd::math::length(p) - r;
}

// Axis-aligned box of half-extents `b`, centred at the origin.
template <MathScalar T> [[nodiscard]] inline T sd_box(const Vec3<T>& p, const Vec3<T>& b) noexcept
{
    using namespace sd_detail;
    const Vec3<T> q = vabs(p) - b;
    return crd::math::length(vmax0(q)) + crd::math::min(max3(q.x, q.y, q.z), static_cast<T>(0));
}

// Box of bounding half-extents `b`, edges rounded with radius `r` (`b` is the
// box's overall half-extent, `r` is subtracted from each face — iq convention).
template <MathScalar T> [[nodiscard]] inline T sd_round_box(const Vec3<T>& p, const Vec3<T>& b, T r) noexcept
{
    using namespace sd_detail;
    const Vec3<T> q = vabs(p) - b + Vec3<T>(r, r, r);
    return crd::math::length(vmax0(q)) + crd::math::min(max3(q.x, q.y, q.z), static_cast<T>(0)) - r;
}

// Wireframe ("box frame") of bounding half-extents `b`, strut thickness `e`.
template <MathScalar T> [[nodiscard]] inline T sd_box_frame(const Vec3<T>& p_in, const Vec3<T>& b, T e) noexcept
{
    using namespace sd_detail;
    const Vec3<T> p = vabs(p_in) - b;
    const Vec3<T> q = vabs(p + Vec3<T>(e, e, e)) - Vec3<T>(e, e, e);
    const T d0 =
        crd::math::length(vmax0(Vec3<T>(p.x, q.y, q.z))) + crd::math::min(max3(p.x, q.y, q.z), static_cast<T>(0));
    const T d1 =
        crd::math::length(vmax0(Vec3<T>(q.x, p.y, q.z))) + crd::math::min(max3(q.x, p.y, q.z), static_cast<T>(0));
    const T d2 =
        crd::math::length(vmax0(Vec3<T>(q.x, q.y, p.z))) + crd::math::min(max3(q.x, q.y, p.z), static_cast<T>(0));
    return crd::math::min(crd::math::min(d0, d1), d2);
}

// Half-space boundary: plane through the origin with unit normal `n` and offset
// `h` (the plane is `dot(x, n) + h = 0`).
template <MathScalar T> [[nodiscard]] inline T sd_plane(const Vec3<T>& p, const Vec3<T>& n, T h) noexcept
{
    return crd::math::dot(p, n) + h;
}

// Capsule: the segment `a`→`b` swept by radius `r` (hemispherical caps).
template <MathScalar T>
[[nodiscard]] inline T sd_capsule(const Vec3<T>& p, const Vec3<T>& a, const Vec3<T>& b, T r) noexcept
{
    const Vec3<T> pa = p - a;
    const Vec3<T> ba = b - a;
    const T denom = crd::math::dot(ba, ba);
    const T h = denom > static_cast<T>(0)
                    ? crd::math::clamp(crd::math::dot(pa, ba) / denom, static_cast<T>(0), static_cast<T>(1))
                    : static_cast<T>(0);
    return crd::math::length(pa - ba * h) - r;
}

// Capped cylinder: the segment `a`→`b` swept by radius `r` with FLAT end caps.
template <MathScalar T>
[[nodiscard]] inline T sd_cylinder(const Vec3<T>& p, const Vec3<T>& a, const Vec3<T>& b, T r) noexcept
{
    using sd_detail::sgn;
    const Vec3<T> ba = b - a;
    const Vec3<T> pa = p - a;
    const T baba = crd::math::dot(ba, ba);
    const T paba = crd::math::dot(pa, ba);
    const T x = crd::math::length(pa * baba - ba * paba) - r * baba;
    const T y = std::abs(paba - baba * static_cast<T>(0.5)) - baba * static_cast<T>(0.5);
    const T x2 = x * x;
    const T y2 = y * y * baba;
    const T d =
        (crd::math::max(x, y) < static_cast<T>(0))
            ? -crd::math::min(x2, y2)
            : ((x > static_cast<T>(0) ? x2 : static_cast<T>(0)) + (y > static_cast<T>(0) ? y2 : static_cast<T>(0)));
    return sgn(d) * std::sqrt(std::abs(d)) / baba;
}

// Cone of height `h` with apex at the origin opening down the +y axis; `c` is
// `(sin θ, cos θ)` of the half-angle θ (iq's capped-cone parameterisation).
template <MathScalar T> [[nodiscard]] inline T sd_cone(const Vec3<T>& p, const Vec2<T>& c, T h) noexcept
{
    using sd_detail::sgn;
    const Vec2<T> q(h * c.x / c.y, -h);
    const Vec2<T> w(std::sqrt(p.x * p.x + p.z * p.z), p.y);
    const T t0 = crd::math::clamp(crd::math::dot(w, q) / crd::math::dot(q, q), static_cast<T>(0), static_cast<T>(1));
    const Vec2<T> a = w - q * t0;
    const Vec2<T> b(w.x - q.x * crd::math::clamp(w.x / q.x, static_cast<T>(0), static_cast<T>(1)), w.y - q.y);
    const T k = sgn(q.y);
    const T d = crd::math::min(crd::math::dot(a, a), crd::math::dot(b, b));
    const T s = crd::math::max(k * (w.x * q.y - w.y * q.x), k * (w.y - q.y));
    return std::sqrt(d) * sgn(s);
}

// Torus in the xz-plane: major radius `t.x`, minor (tube) radius `t.y`.
template <MathScalar T> [[nodiscard]] inline T sd_torus(const Vec3<T>& p, const Vec2<T>& t) noexcept
{
    const T qx = std::sqrt(p.x * p.x + p.z * p.z) - t.x;
    return std::sqrt(qx * qx + p.y * p.y) - t.y;
}

// Triangle (a, b, c) in 3D — UNSIGNED distance (a triangle has no interior;
// iq's `udTriangle`). The signed-by-plane-side form belongs to a mesh / shell
// consumer, not the primitive layer.
template <MathScalar T>
[[nodiscard]] inline T sd_triangle(const Vec3<T>& p, const Vec3<T>& a, const Vec3<T>& b, const Vec3<T>& c) noexcept
{
    using sd_detail::sgn;
    const Vec3<T> ba = b - a;
    const Vec3<T> pa = p - a;
    const Vec3<T> cb = c - b;
    const Vec3<T> pb = p - b;
    const Vec3<T> ac = a - c;
    const Vec3<T> pc = p - c;
    const Vec3<T> nor = crd::math::cross(ba, ac);
    const auto dot2 = [](const Vec3<T>& v) noexcept
    {
        return crd::math::dot(v, v);
    };
    const T sign_sum = sgn(crd::math::dot(crd::math::cross(ba, nor), pa)) +
                       sgn(crd::math::dot(crd::math::cross(cb, nor), pb)) +
                       sgn(crd::math::dot(crd::math::cross(ac, nor), pc));
    if (sign_sum < static_cast<T>(2))
    {
        const T e0 =
            dot2(ba * crd::math::clamp(crd::math::dot(ba, pa) / dot2(ba), static_cast<T>(0), static_cast<T>(1)) - pa);
        const T e1 =
            dot2(cb * crd::math::clamp(crd::math::dot(cb, pb) / dot2(cb), static_cast<T>(0), static_cast<T>(1)) - pb);
        const T e2 =
            dot2(ac * crd::math::clamp(crd::math::dot(ac, pc) / dot2(ac), static_cast<T>(0), static_cast<T>(1)) - pc);
        return std::sqrt(crd::math::min(crd::math::min(e0, e1), e2));
    }
    const T np = crd::math::dot(nor, pa);
    return std::sqrt(np * np / dot2(nor));
}

// Ellipsoid with semi-axes `r` (approximate distance — exact has no closed
// form; iq's well-behaved bound).
template <MathScalar T> [[nodiscard]] inline T sd_ellipsoid(const Vec3<T>& p, const Vec3<T>& r) noexcept
{
    const Vec3<T> pr(p.x / r.x, p.y / r.y, p.z / r.z);
    const Vec3<T> prr(p.x / (r.x * r.x), p.y / (r.y * r.y), p.z / (r.z * r.z));
    const T k0 = crd::math::length(pr);
    const T k1 = crd::math::length(prr);
    if (k1 <= static_cast<T>(0))
    {
        return (k0 - static_cast<T>(1)) * crd::math::min(crd::math::min(r.x, r.y), r.z);
    }
    return k0 * (k0 - static_cast<T>(1)) / k1;
}

// Regular octahedron of "radius" `s` (distance from centre to a vertex along an
// axis), centred at the origin — exact (iq).
template <MathScalar T> [[nodiscard]] inline T sd_octahedron(const Vec3<T>& p_in, T s) noexcept
{
    const Vec3<T> p = sd_detail::vabs(p_in);
    const T m = p.x + p.y + p.z - s;
    Vec3<T> q;
    if (static_cast<T>(3) * p.x < m)
    {
        q = Vec3<T>(p.x, p.y, p.z);
    }
    else if (static_cast<T>(3) * p.y < m)
    {
        q = Vec3<T>(p.y, p.z, p.x);
    }
    else if (static_cast<T>(3) * p.z < m)
    {
        q = Vec3<T>(p.z, p.x, p.y);
    }
    else
    {
        return m * static_cast<T>(0.57735026918962576451); // m / sqrt(3)
    }
    const T k = crd::math::clamp(static_cast<T>(0.5) * (q.z - q.y + s), static_cast<T>(0), s);
    return crd::math::length(Vec3<T>(q.x, q.y - s + k, q.z - k));
}

// ===========================================================================
// 2D
// ===========================================================================

// Circle of radius `r` centred at the origin.
template <MathScalar T> [[nodiscard]] inline T sd_circle(const Vec2<T>& p, T r) noexcept
{
    return crd::math::length(p) - r;
}

// Axis-aligned box of half-extents `b`, centred at the origin (2D).
template <MathScalar T> [[nodiscard]] inline T sd_box_2d(const Vec2<T>& p, const Vec2<T>& b) noexcept
{
    using namespace sd_detail;
    const Vec2<T> d = vabs(p) - b;
    return crd::math::length(vmax0(d)) + crd::math::min(crd::math::max(d.x, d.y), static_cast<T>(0));
}

// Box of bounding half-extents `b` with corners rounded by radius `r` (2D).
template <MathScalar T> [[nodiscard]] inline T sd_round_box_2d(const Vec2<T>& p, const Vec2<T>& b, T r) noexcept
{
    using namespace sd_detail;
    const Vec2<T> q = vabs(p) - b + Vec2<T>(r, r);
    return crd::math::min(crd::math::max(q.x, q.y), static_cast<T>(0)) + crd::math::length(vmax0(q)) - r;
}

// Segment a→b (2D) — UNSIGNED distance (a segment has no interior; iq's
// `sdSegment`).
template <MathScalar T>
[[nodiscard]] inline T sd_segment_2d(const Vec2<T>& p, const Vec2<T>& a, const Vec2<T>& b) noexcept
{
    const Vec2<T> pa = p - a;
    const Vec2<T> ba = b - a;
    const T denom = crd::math::dot(ba, ba);
    const T h = denom > static_cast<T>(0)
                    ? crd::math::clamp(crd::math::dot(pa, ba) / denom, static_cast<T>(0), static_cast<T>(1))
                    : static_cast<T>(0);
    return crd::math::length(pa - ba * h);
}

// Triangle (p0, p1, p2) in 2D — signed (negative inside; iq's `sdTriangle`).
template <MathScalar T>
[[nodiscard]] inline T sd_triangle_2d(const Vec2<T>& p, const Vec2<T>& p0, const Vec2<T>& p1,
                                      const Vec2<T>& p2) noexcept
{
    using sd_detail::sgn;
    const Vec2<T> e0 = p1 - p0;
    const Vec2<T> e1 = p2 - p1;
    const Vec2<T> e2 = p0 - p2;
    const Vec2<T> v0 = p - p0;
    const Vec2<T> v1 = p - p1;
    const Vec2<T> v2 = p - p2;
    const Vec2<T> pq0 = v0 - e0 * crd::math::clamp(crd::math::dot(v0, e0) / crd::math::dot(e0, e0), static_cast<T>(0),
                                                   static_cast<T>(1));
    const Vec2<T> pq1 = v1 - e1 * crd::math::clamp(crd::math::dot(v1, e1) / crd::math::dot(e1, e1), static_cast<T>(0),
                                                   static_cast<T>(1));
    const Vec2<T> pq2 = v2 - e2 * crd::math::clamp(crd::math::dot(v2, e2) / crd::math::dot(e2, e2), static_cast<T>(0),
                                                   static_cast<T>(1));
    const T s = sgn(e0.x * e2.y - e0.y * e2.x);
    const T d0x = crd::math::dot(pq0, pq0);
    const T d0y = s * (v0.x * e0.y - v0.y * e0.x);
    const T d1x = crd::math::dot(pq1, pq1);
    const T d1y = s * (v1.x * e1.y - v1.y * e1.x);
    const T d2x = crd::math::dot(pq2, pq2);
    const T d2y = s * (v2.x * e2.y - v2.y * e2.x);
    // iq's `min(min(vec2,vec2),vec2)` is component-wise: the .x and .y lanes are
    // each min'd independently (it is NOT a lexicographic pair-min).
    const T dx = crd::math::min(crd::math::min(d0x, d1x), d2x);
    const T dy = crd::math::min(crd::math::min(d0y, d1y), d2y);
    return -std::sqrt(dx) * sgn(dy);
}

// Equilateral triangle of "radius" `r` (centre-to-vertex), apex up, centred at
// the origin — signed (iq).
template <MathScalar T> [[nodiscard]] inline T sd_equilateral_triangle_2d(const Vec2<T>& p_in, T r) noexcept
{
    using sd_detail::sgn;
    const T k = static_cast<T>(1.7320508075688772935); // sqrt(3)
    Vec2<T> p(std::abs(p_in.x) - r, p_in.y + r / k);
    if (p.x + k * p.y > static_cast<T>(0))
    {
        p = Vec2<T>(p.x - k * p.y, -k * p.x - p.y) * static_cast<T>(0.5);
    }
    p.x -= crd::math::clamp(p.x, static_cast<T>(-2) * r, static_cast<T>(0));
    return -crd::math::length(p) * sgn(p.y);
}

// Regular pentagon of circumradius `r`, one vertex up, centred at the origin —
// signed (iq).
template <MathScalar T> [[nodiscard]] inline T sd_pentagon_2d(const Vec2<T>& p_in, T r) noexcept
{
    const T kx = static_cast<T>(0.809016994374947);
    const T ky = static_cast<T>(0.587785252292473);
    const T kz = static_cast<T>(0.726542528005361);
    Vec2<T> p(std::abs(p_in.x), p_in.y);
    p = p -
        Vec2<T>(-kx, ky) * (static_cast<T>(2) * crd::math::min(crd::math::dot(Vec2<T>(-kx, ky), p), static_cast<T>(0)));
    p = p -
        Vec2<T>(kx, ky) * (static_cast<T>(2) * crd::math::min(crd::math::dot(Vec2<T>(kx, ky), p), static_cast<T>(0)));
    p = p - Vec2<T>(crd::math::clamp(p.x, -r * kz, r * kz), r);
    return crd::math::length(p) * sd_detail::sgn(p.y);
}

// Regular hexagon of incircle radius `r` (flat side up), centred at the origin
// — signed (iq).
template <MathScalar T> [[nodiscard]] inline T sd_hexagon_2d(const Vec2<T>& p_in, T r) noexcept
{
    const T kx = static_cast<T>(-0.866025403784439);
    const T ky = static_cast<T>(0.5);
    const T kz = static_cast<T>(0.577350269189626);
    Vec2<T> p(std::abs(p_in.x), std::abs(p_in.y));
    p = p -
        Vec2<T>(kx, ky) * (static_cast<T>(2) * crd::math::min(crd::math::dot(Vec2<T>(kx, ky), p), static_cast<T>(0)));
    p = p - Vec2<T>(crd::math::clamp(p.x, -kz * r, kz * r), r);
    return crd::math::length(p) * sd_detail::sgn(p.y);
}

} // namespace crd::geometry::primitives
