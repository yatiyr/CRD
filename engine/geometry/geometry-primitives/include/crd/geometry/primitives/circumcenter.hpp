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
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>

#include <limits>

namespace crd::geometry::primitives
{

// The smallest |determinant| a circumcentre solves with: below the smallest normal f64 (zero, or so small that its
// reciprocal overflows) the simplex is degenerate and the centroid is returned.
inline constexpr crd::f64 kCircumcenterMinAbsDet = std::numeric_limits<crd::f64>::min();

// 2D triangle circumcentre. Returns the point equidistant from a, b, c.
// Computed in f64 then cast to T for robustness on large coords.
//
// **Derivation**: translate the origin to a (a' = 0). The centre O' is
// equidistant from 0, b' and c', so
//   (b - a)·O' = |b - a|² / 2
//   (c - a)·O' = |c - a|² / 2
// a 2x2 system crd-math solves by Cramer's rule; circumcentre = a + O'.
//
// Returns the centroid as a finite fallback on degenerate (collinear)
// input, |det| < kCircumcenterMinAbsDet -- caller's responsibility to guard
// via `orient2d != 0`.
template <crd::math::MathScalar T>
[[nodiscard]] inline crd::math::Vec2<T>
circumcenter_2d(const crd::math::Vec2<T>& a,
                 const crd::math::Vec2<T>& b,
                 const crd::math::Vec2<T>& c) noexcept
{
    using V = crd::math::Vec2<crd::f64>;
    const V a64{static_cast<crd::f64>(a.x), static_cast<crd::f64>(a.y)};
    const V b64 = V{static_cast<crd::f64>(b.x), static_cast<crd::f64>(b.y)} - a64;
    const V c64 = V{static_cast<crd::f64>(c.x), static_cast<crd::f64>(c.y)} - a64;

    // The system's rows are b - a and c - a.
    const crd::math::Mat2<crd::f64> rows = crd::math::transpose(crd::math::Mat2<crd::f64>{b64, c64});
    const V rhs = V{crd::math::dot(b64, b64), crd::math::dot(c64, c64)} * 0.5;
    V o_rel{};
    if (!crd::math::try_solve(rows, rhs, o_rel, kCircumcenterMinAbsDet))
    {
        o_rel = (b64 + c64) / 3.0; // the centroid
    }
    const V o = a64 + o_rel;
    return crd::math::Vec2<T>{static_cast<T>(o.x), static_cast<T>(o.y)};
}

// 3D tetrahedron circumcentre. Returns the point equidistant from a, b, c, d.
// Computed in f64 then cast to T for robustness on large coords (D95-3D).
//
// **Derivation**: translate origin to a (so a'=0). Solve the 3x3 linear system
//   [b - a]·O' = |b - a|² / 2
//   [c - a]·O' = |c - a|² / 2
//   [d - a]·O' = |d - a|² / 2
// for O' (3 unknowns) by crd-math's Cramer's rule; circumcentre = a + O'.
//
// Returns the centroid as a finite fallback on degenerate (coplanar/zero-vol)
// input, |det| < kCircumcenterMinAbsDet -- caller's responsibility to guard
// via `orient3d != 0`.
template <crd::math::MathScalar T>
[[nodiscard]] inline crd::math::Vec3<T>
circumcenter_3d(const crd::math::Vec3<T>& a,
                 const crd::math::Vec3<T>& b,
                 const crd::math::Vec3<T>& c,
                 const crd::math::Vec3<T>& d) noexcept
{
    using V = crd::math::Vec3<crd::f64>;
    const V a64{static_cast<crd::f64>(a.x), static_cast<crd::f64>(a.y), static_cast<crd::f64>(a.z)};
    const V b64 = V{static_cast<crd::f64>(b.x), static_cast<crd::f64>(b.y), static_cast<crd::f64>(b.z)} - a64;
    const V c64 = V{static_cast<crd::f64>(c.x), static_cast<crd::f64>(c.y), static_cast<crd::f64>(c.z)} - a64;
    const V d64 = V{static_cast<crd::f64>(d.x), static_cast<crd::f64>(d.y), static_cast<crd::f64>(d.z)} - a64;

    // The system's rows are b - a, c - a and d - a.
    const crd::math::Mat3<crd::f64> rows = crd::math::transpose(crd::math::Mat3<crd::f64>{b64, c64, d64});
    const V rhs = V{crd::math::dot(b64, b64), crd::math::dot(c64, c64), crd::math::dot(d64, d64)} * 0.5;
    V o_rel{};
    if (!crd::math::try_solve(rows, rhs, o_rel, kCircumcenterMinAbsDet))
    {
        o_rel = (b64 + c64 + d64) / 4.0; // the centroid
    }
    const V o = a64 + o_rel;
    return crd::math::Vec3<T>{static_cast<T>(o.x), static_cast<T>(o.y), static_cast<T>(o.z)};
}

} // namespace crd::geometry::primitives
