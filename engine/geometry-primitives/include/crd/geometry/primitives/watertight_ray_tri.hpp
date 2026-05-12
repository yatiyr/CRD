#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — watertight & precomputed ray↔triangle (v0f).
//
// Two production-renderer ray-triangle tests, complementing v0c's scalar
// Möller-Trumbore (which stays as the cross-check reference):
//
//   * Woop / Benthin / Wald 2013, "Watertight Ray/Triangle Intersection"
//     (JCGT 2(1)) — a per-*ray* shear+scale that maps `dir` onto +Z, then 2D
//     edge functions. *Watertight*: a ray that grazes an edge shared by two
//     triangles hits exactly one of them (or both consistently) — never falls
//     through the crack. Per-ray precompute (`RayTriShear`) amortises across a
//     whole BVH subtree of triangles. The default ray-tri for `crd-geometry-
//     mesh` v4d BVH leaves and `crd-sdf` v2 mesh-bake.
//       Determinism (ADR-0076 §4 #12): the dominant `dir` axis is picked
//       max-then-X-then-Y-then-Z; an edge function that lands exactly on 0 is
//       recomputed in `double` (when T is float) and accepted if still 0
//       ("closed on-edge").
//
//   * Baldwin-Weber 2016, "Fast Ray-Triangle Intersections by Coordinate
//     Transformation" (JCGT 5(3)) — a per-*triangle* affine transform to the
//     unit triangle {(0,0,0),(1,0,0),(0,1,0)} (we build it as the inverse of
//     [edge1 | edge2 | normal], which is robust by construction — det = ‖n‖²).
//     Branchless per-ray test. The opt-in default for cooked *static* meshes
//     (12 floats / 48 B per triangle). `crd-geometry-mesh` v4d picks per
//     context: Woop for dynamic / BVH-leaf, Baldwin-Weber for baked statics.
//
// No transcendental libm calls; `std::abs` is IEEE-exact and allowed.
// ---------------------------------------------------------------------------

#include <crd/geometry/primitives/primitives.hpp>

#include <cmath>
#include <limits>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::Vec3;

// ===========================================================================
// Woop / Benthin / Wald 2013 — watertight, per-ray precompute.
// ===========================================================================

// Per-ray shear: permuted axes (kz = dominant `dir` axis) + the shear that maps
// `dir` to the +Z direction. Cheap (3 div); reuse across many triangles.
template <MathScalar T> struct RayTriShear
{
    crd::usize kx{0};
    crd::usize ky{1};
    crd::usize kz{2};
    T Sx{0};
    T Sy{0};
    T Sz{0};
};

template <MathScalar T> [[nodiscard]] inline RayTriShear<T> precompute_ray_tri(const Ray3<T>& ray) noexcept
{
    const T ax = ray.direction.x < static_cast<T>(0) ? -ray.direction.x : ray.direction.x;
    const T ay = ray.direction.y < static_cast<T>(0) ? -ray.direction.y : ray.direction.y;
    const T az = ray.direction.z < static_cast<T>(0) ? -ray.direction.z : ray.direction.z;
    RayTriShear<T> s;
    if (ax >= ay && ax >= az)
    {
        s.kz = 0;
    }
    else if (ay >= az)
    {
        s.kz = 1;
    }
    else
    {
        s.kz = 2;
    }
    s.kx = s.kz + 1U == 3U ? 0U : s.kz + 1U;
    s.ky = s.kx + 1U == 3U ? 0U : s.kx + 1U;
    // Preserve triangle winding: swap kx/ky when dir along kz is negative.
    if (ray.direction[s.kz] < static_cast<T>(0))
    {
        const crd::usize t = s.kx;
        s.kx = s.ky;
        s.ky = t;
    }
    const T dz = ray.direction[s.kz]; // non-zero unless the ray direction is the zero vector
    s.Sx = ray.direction[s.kx] / dz;
    s.Sy = ray.direction[s.ky] / dz;
    s.Sz = static_cast<T>(1) / dz;
    return s;
}

namespace detail
{
// The Woop edge-sign trinity for verts A, B, C already in the sheared frame.
template <MathScalar T> struct WoopEdges
{
    T u, v, w;
};
template <MathScalar T> [[nodiscard]] constexpr WoopEdges<T> woop_edges(T Ax, T Ay, T Bx, T By, T Cx, T Cy) noexcept
{
    return WoopEdges<T>{Cx * By - Cy * Bx, Ax * Cy - Ay * Cx, Bx * Ay - By * Ax};
}
} // namespace detail

// Watertight ray↔triangle (Woop 2013). `cull_back` ⇒ ignore back-facing hits.
// Returns the nearest hit with `t ≥ tnear`; fills `out_t` and `out_bary` (the
// triangle barycentric `(1−u−v, u, v)` matching vertex order a, b, c).
template <MathScalar T>
[[nodiscard]] inline bool
intersect_ray_triangle_watertight(const Ray3<T>& ray, const RayTriShear<T>& s, const Triangle3<T>& tri, T& out_t,
                                  Vec3<T>& out_bary, bool cull_back = false, T tnear = static_cast<T>(0)) noexcept
{
    const Vec3<T> A = tri.a - ray.origin;
    const Vec3<T> B = tri.b - ray.origin;
    const Vec3<T> C = tri.c - ray.origin;
    const T Ax = A[s.kx] - s.Sx * A[s.kz];
    const T Ay = A[s.ky] - s.Sy * A[s.kz];
    const T Bx = B[s.kx] - s.Sx * B[s.kz];
    const T By = B[s.ky] - s.Sy * B[s.kz];
    const T Cx = C[s.kx] - s.Sx * C[s.kz];
    const T Cy = C[s.ky] - s.Sy * C[s.kz];

    auto e = detail::woop_edges<T>(Ax, Ay, Bx, By, Cx, Cy);
    // Closed on-edge: an exact zero is recomputed in higher precision; if still
    // exactly 0 the ray passes through that edge → keep it (watertight).
    if (e.u == static_cast<T>(0) || e.v == static_cast<T>(0) || e.w == static_cast<T>(0))
    {
        if constexpr (std::is_same_v<T, float>)
        {
            const double AxD = static_cast<double>(Ax);
            const double AyD = static_cast<double>(Ay);
            const double BxD = static_cast<double>(Bx);
            const double ByD = static_cast<double>(By);
            const double CxD = static_cast<double>(Cx);
            const double CyD = static_cast<double>(Cy);
            e.u = static_cast<float>(CxD * ByD - CyD * BxD);
            e.v = static_cast<float>(AxD * CyD - AyD * CxD);
            e.w = static_cast<float>(BxD * AyD - ByD * AxD);
        }
    }
    const bool any_neg = e.u < static_cast<T>(0) || e.v < static_cast<T>(0) || e.w < static_cast<T>(0);
    const bool any_pos = e.u > static_cast<T>(0) || e.v > static_cast<T>(0) || e.w > static_cast<T>(0);
    if (any_neg && any_pos)
    {
        return false; // mixed signs — the ray's line misses the triangle
    }
    const T det = e.u + e.v + e.w;
    if (det == static_cast<T>(0))
    {
        return false; // edge-on / degenerate
    }
    if (cull_back && det < static_cast<T>(0))
    {
        return false;
    }
    const T Az = s.Sz * A[s.kz];
    const T Bz = s.Sz * B[s.kz];
    const T Cz = s.Sz * C[s.kz];
    const T t_scaled = e.u * Az + e.v * Bz + e.w * Cz;
    // Hit iff t ≥ tnear, with the comparison done in scaled space (sign of det).
    if (det > static_cast<T>(0))
    {
        if (t_scaled < tnear * det)
        {
            return false;
        }
    }
    else
    {
        if (t_scaled > tnear * det)
        {
            return false;
        }
    }
    const T inv_det = static_cast<T>(1) / det;
    out_t = t_scaled * inv_det;
    const T u = e.v * inv_det; // weight of vertex b
    const T v = e.w * inv_det; // weight of vertex c
    out_bary = Vec3<T>(static_cast<T>(1) - u - v, u, v);
    return true;
}

// Convenience overload: builds the per-ray shear inline (use the precomputed
// form when tracing one ray against many triangles).
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_triangle_watertight(const Ray3<T>& ray, const Triangle3<T>& tri, T& out_t,
                                                            Vec3<T>& out_bary, bool cull_back = false,
                                                            T tnear = static_cast<T>(0)) noexcept
{
    return intersect_ray_triangle_watertight(ray, precompute_ray_tri(ray), tri, out_t, out_bary, cull_back, tnear);
}

// ===========================================================================
// Baldwin-Weber 2016 — precomputed per-triangle affine transform.
// ===========================================================================

// The affine map sending {a, b, c, a+n} → {(0,0,0), (1,0,0), (0,1,0), (0,0,1)},
// stored as the 3 rows of its linear part + the triangle's vertex `a`. 12 T
// (48 B for f32). Built as inverse([edge1 | edge2 | normal]) — robust by
// construction (its determinant is ‖normal‖², non-zero for a real triangle).
template <MathScalar T> struct TriAffine
{
    Vec3<T> row0{}; // u-row
    Vec3<T> row1{}; // v-row
    Vec3<T> row2{}; // (signed) z-row — 0 on the triangle plane
    Vec3<T> a{};
};

template <MathScalar T> [[nodiscard]] inline TriAffine<T> precompute_triangle_affine(const Triangle3<T>& tri) noexcept
{
    const Vec3<T> e1 = tri.b - tri.a;
    const Vec3<T> e2 = tri.c - tri.a;
    const Vec3<T> n = crd::math::cross(e1, e2);
    const T det = crd::math::dot(n, n); // = ‖n‖² ; > 0 for a non-degenerate triangle
    const T inv = static_cast<T>(1) / (det + std::numeric_limits<T>::min());
    TriAffine<T> m;
    m.row0 = crd::math::cross(e2, n) * inv;
    m.row1 = crd::math::cross(n, e1) * inv;
    m.row2 = n * inv;
    m.a = tri.a;
    return m;
}

// Branchless per-ray test against a precomputed triangle. Hit iff the ray
// crosses the triangle plane (z'≠0) forward (t ≥ tnear) inside the unit
// triangle (u ≥ 0, v ≥ 0, u+v ≤ 1). `out_bary` is `(1−u−v, u, v)`.
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_triangle_precomputed(const Ray3<T>& ray, const TriAffine<T>& m, T& out_t,
                                                             Vec3<T>& out_bary, T tnear = static_cast<T>(0),
                                                             T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const Vec3<T> rel = ray.origin - m.a;
    const T oz = crd::math::dot(m.row2, rel);
    const T dz = crd::math::dot(m.row2, ray.direction);
    if (dz < epsilon && dz > -epsilon)
    {
        return false; // ray parallel to the triangle plane
    }
    const T t = -oz / dz;
    if (t < tnear)
    {
        return false;
    }
    const T ox = crd::math::dot(m.row0, rel);
    const T dx = crd::math::dot(m.row0, ray.direction);
    const T oy = crd::math::dot(m.row1, rel);
    const T dy = crd::math::dot(m.row1, ray.direction);
    const T u = ox + t * dx;
    const T v = oy + t * dy;
    if (u < static_cast<T>(0) || v < static_cast<T>(0) || (u + v) > static_cast<T>(1))
    {
        return false;
    }
    out_t = t;
    out_bary = Vec3<T>(static_cast<T>(1) - u - v, u, v);
    return true;
}

} // namespace crd::geometry::primitives
