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

} // namespace crd::geometry::primitives
