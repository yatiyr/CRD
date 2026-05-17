#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — circumcentre computation for 2D + 3D triangles.
//
// **Robustness pattern (D95)**: always lifted to `f64` regardless of the
// caller's `T`, then cast back. Circumcentre formulas have terms of order
// `coord^2 / coord` which can overflow f32 for input coords above ~10^3.
// The f64 lift is the same pattern as `crd-geometry-spatial`'s LooseOctree
// raycast precompute (f32 surface, f64 internals).
//
// **References**:
//   - https://mathworld.wolfram.com/Circumcircle.html
//   - Shewchuk's `predicates.c` derives circumcentres in the same lifted
//     form as the basis for cocircularity detection.
//
// Consumers:
//   - `crd-geometry-delaunay` v8d-2d / v8d-3d Voronoi extraction
//     (Voronoi vertex = Delaunay simplex circumcentre).
//   - `crd-geometry-delaunay` v8g Ruppert refinement (Steiner points at
//     circumcentres of low-quality triangles).
//   - Future tet meshers (`crd-fea` Phase 3.1.12) for tet quality metrics.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::primitives
{

// 2D triangle circumcentre. Returns the point equidistant from a, b, c.
// Computed in f64 then cast to T for robustness on large coords.
template <crd::math::MathScalar T>
[[nodiscard]] inline crd::math::Vec2<T>
circumcenter_2d(const crd::math::Vec2<T>& a,
                 const crd::math::Vec2<T>& b,
                 const crd::math::Vec2<T>& c) noexcept
{
    const crd::f64 ax = static_cast<crd::f64>(a.x);
    const crd::f64 ay = static_cast<crd::f64>(a.y);
    const crd::f64 bx = static_cast<crd::f64>(b.x);
    const crd::f64 by = static_cast<crd::f64>(b.y);
    const crd::f64 cx = static_cast<crd::f64>(c.x);
    const crd::f64 cy = static_cast<crd::f64>(c.y);

    // d = 2·((b-a) × (c-a))  (twice the signed area).
    const crd::f64 d = 2.0 * ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax));
    if (d == 0.0)
    {
        // Degenerate (collinear) — caller's responsibility to guard. Return
        // centroid as a finite fallback rather than NaN.
        const crd::f64 ux = (ax + bx + cx) / 3.0;
        const crd::f64 uy = (ay + by + cy) / 3.0;
        return crd::math::Vec2<T>{static_cast<T>(ux), static_cast<T>(uy)};
    }

    const crd::f64 a_sq = ax * ax + ay * ay;
    const crd::f64 b_sq = bx * bx + by * by;
    const crd::f64 c_sq = cx * cx + cy * cy;

    const crd::f64 ux = (a_sq * (by - cy) + b_sq * (cy - ay) + c_sq * (ay - by)) / d;
    const crd::f64 uy = (a_sq * (cx - bx) + b_sq * (ax - cx) + c_sq * (bx - ax)) / d;

    return crd::math::Vec2<T>{static_cast<T>(ux), static_cast<T>(uy)};
}

// 3D tetrahedron circumcentre. Returns the point equidistant from a, b, c, d.
// Computed in f64 then cast to T for robustness on large coords (D95-3D).
//
// **Derivation**: translate origin to a (so a'=0). Solve the 3x3 linear system
//   [b - a]·O' = |b - a|² / 2
//   [c - a]·O' = |c - a|² / 2
//   [d - a]·O' = |d - a|² / 2
// for O' (3 unknowns); circumcentre = a + O'. Closed-form via Cramer's rule.
//
// Returns the centroid as a finite fallback on degenerate (coplanar/zero-vol)
// input — caller's responsibility to guard via `orient3d != 0`.
template <crd::math::MathScalar T>
[[nodiscard]] inline crd::math::Vec3<T>
circumcenter_3d(const crd::math::Vec3<T>& a,
                 const crd::math::Vec3<T>& b,
                 const crd::math::Vec3<T>& c,
                 const crd::math::Vec3<T>& d) noexcept
{
    const crd::f64 ax = static_cast<crd::f64>(a.x);
    const crd::f64 ay = static_cast<crd::f64>(a.y);
    const crd::f64 az = static_cast<crd::f64>(a.z);
    const crd::f64 bx = static_cast<crd::f64>(b.x) - ax;
    const crd::f64 by = static_cast<crd::f64>(b.y) - ay;
    const crd::f64 bz = static_cast<crd::f64>(b.z) - az;
    const crd::f64 cx = static_cast<crd::f64>(c.x) - ax;
    const crd::f64 cy = static_cast<crd::f64>(c.y) - ay;
    const crd::f64 cz = static_cast<crd::f64>(c.z) - az;
    const crd::f64 dx = static_cast<crd::f64>(d.x) - ax;
    const crd::f64 dy = static_cast<crd::f64>(d.y) - ay;
    const crd::f64 dz = static_cast<crd::f64>(d.z) - az;

    // 6·signed volume of tet = det([b-a; c-a; d-a]).
    const crd::f64 det6 = bx * (cy * dz - cz * dy)
                        - by * (cx * dz - cz * dx)
                        + bz * (cx * dy - cy * dx);
    if (det6 == 0.0)
    {
        // Coplanar / degenerate tet — centroid fallback.
        const crd::f64 ux = (ax + (ax + bx) + (ax + cx) + (ax + dx)) / 4.0;
        const crd::f64 uy = (ay + (ay + by) + (ay + cy) + (ay + dy)) / 4.0;
        const crd::f64 uz = (az + (az + bz) + (az + cz) + (az + dz)) / 4.0;
        return crd::math::Vec3<T>{static_cast<T>(ux), static_cast<T>(uy), static_cast<T>(uz)};
    }

    // Right-hand sides: |b-a|² / 2, |c-a|² / 2, |d-a|² / 2.
    const crd::f64 rhs_b = (bx * bx + by * by + bz * bz) * 0.5;
    const crd::f64 rhs_c = (cx * cx + cy * cy + cz * cz) * 0.5;
    const crd::f64 rhs_d = (dx * dx + dy * dy + dz * dz) * 0.5;

    // Cramer's rule on the 3x3 system [b'; c'; d'] · O' = [rhs_b; rhs_c; rhs_d].
    const crd::f64 inv_det = 1.0 / det6;
    const crd::f64 ox = (rhs_b * (cy * dz - cz * dy)
                       - by * (rhs_c * dz - cz * rhs_d)
                       + bz * (rhs_c * dy - cy * rhs_d)) * inv_det;
    const crd::f64 oy = (bx * (rhs_c * dz - cz * rhs_d)
                       - rhs_b * (cx * dz - cz * dx)
                       + bz * (cx * rhs_d - rhs_c * dx)) * inv_det;
    const crd::f64 oz = (bx * (cy * rhs_d - rhs_c * dy)
                       - by * (cx * rhs_d - rhs_c * dx)
                       + rhs_b * (cx * dy - cy * dx)) * inv_det;

    return crd::math::Vec3<T>{static_cast<T>(ax + ox), static_cast<T>(ay + oy), static_cast<T>(az + oz)};
}

} // namespace crd::geometry::primitives
