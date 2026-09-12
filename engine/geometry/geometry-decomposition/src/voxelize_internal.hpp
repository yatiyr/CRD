#pragma once

// ---------------------------------------------------------------------------
// Akenine-Möller 2001 triangle/AABB Separating Axis Theorem overlap test.
// Internal to crd-geometry-decomposition — NOT a public header. Lives in
// `src/` so future SAT consumers in this module (v9c-b cluster splitting
// might want it too) can include it without leaking it as a public API.
//
// The test is exact: returns true iff the closed triangle and the closed
// AABB share at least one point. 13 separating axes total:
//   3  AABB face normals (the canonical x/y/z axes)
//   1  triangle face normal
//   9  cross products of each triangle edge × each AABB edge
//
// Reference: Akenine-Möller, T. (2001), "Fast 3D Triangle-Box Overlap
// Testing", Journal of Graphics Tools 6(1):29-33. Also documented in
// Ericson 2005 §5.2.9.
//
// Inputs are translated so the AABB centre is the origin; the function
// operates entirely in centre-relative coordinates which simplifies the
// edge-cross axis tests to comparing |p_i| against the box's projected
// radius along that axis.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <algorithm>

namespace crd::geometry::decomposition::detail
{

// 9-axis edge-cross helper. `e_axis` is the AABB edge in canonical form
// (one of {x, y, z}); `tri_edge` is a triangle edge vector. Returns true
// if the axis SEPARATES (i.e. no overlap along it). When this returns
// true the SAT test fails fast.
//
// The signature mirrors the literature: rather than constructing the
// cross product as a Vec3, we inline its 9 specialised forms (one per
// {tri_edge, AABB_axis} pair) into the caller because the AABB axis is a
// canonical basis vector and 6 of 9 components are zero.
template <typename T>
[[nodiscard]] inline bool axis_test_x01(T a, T b, T fa, T fb,
                                        const crd::math::Vec3<T>& v0,
                                        const crd::math::Vec3<T>& v2,
                                        const crd::math::Vec3<T>& half_extents) noexcept
{
    // Axis = (1, 0, 0) × tri_edge = (0, -tri_edge.z, tri_edge.y) form.
    const T p0 = a * v0.y - b * v0.z;
    const T p2 = a * v2.y - b * v2.z;
    const T pmin = std::min(p0, p2);
    const T pmax = std::max(p0, p2);
    const T rad  = fa * half_extents.y + fb * half_extents.z;
    return pmin > rad || pmax < -rad;
}

template <typename T>
[[nodiscard]] inline bool axis_test_y02(T a, T b, T fa, T fb,
                                        const crd::math::Vec3<T>& v0,
                                        const crd::math::Vec3<T>& v2,
                                        const crd::math::Vec3<T>& half_extents) noexcept
{
    // Axis = (0, 1, 0) × tri_edge = (tri_edge.z, 0, -tri_edge.x) form.
    const T p0 = -a * v0.x + b * v0.z;
    const T p2 = -a * v2.x + b * v2.z;
    const T pmin = std::min(p0, p2);
    const T pmax = std::max(p0, p2);
    const T rad  = fa * half_extents.x + fb * half_extents.z;
    return pmin > rad || pmax < -rad;
}

template <typename T>
[[nodiscard]] inline bool axis_test_z12(T a, T b, T fa, T fb,
                                        const crd::math::Vec3<T>& v1,
                                        const crd::math::Vec3<T>& v2,
                                        const crd::math::Vec3<T>& half_extents) noexcept
{
    // Axis = (0, 0, 1) × tri_edge = (-tri_edge.y, tri_edge.x, 0) form.
    const T p1 = a * v1.x - b * v1.y;
    const T p2 = a * v2.x - b * v2.y;
    const T pmin = std::min(p1, p2);
    const T pmax = std::max(p1, p2);
    const T rad  = fa * half_extents.x + fb * half_extents.y;
    return pmin > rad || pmax < -rad;
}

template <typename T>
[[nodiscard]] inline bool plane_box_overlap(const crd::math::Vec3<T>& normal,
                                            const crd::math::Vec3<T>& vert,
                                            const crd::math::Vec3<T>& half_extents) noexcept
{
    // Returns true iff the AABB centred at origin with extents `half_extents`
    // straddles the plane through `vert` with normal `normal`.
    crd::math::Vec3<T> vmin{};
    crd::math::Vec3<T> vmax{};
    if (normal.x > T{0})
    {
        vmin.x = -half_extents.x - vert.x;
        vmax.x =  half_extents.x - vert.x;
    }
    else
    {
        vmin.x =  half_extents.x - vert.x;
        vmax.x = -half_extents.x - vert.x;
    }
    if (normal.y > T{0})
    {
        vmin.y = -half_extents.y - vert.y;
        vmax.y =  half_extents.y - vert.y;
    }
    else
    {
        vmin.y =  half_extents.y - vert.y;
        vmax.y = -half_extents.y - vert.y;
    }
    if (normal.z > T{0})
    {
        vmin.z = -half_extents.z - vert.z;
        vmax.z =  half_extents.z - vert.z;
    }
    else
    {
        vmin.z =  half_extents.z - vert.z;
        vmax.z = -half_extents.z - vert.z;
    }
    if (normal.x * vmin.x + normal.y * vmin.y + normal.z * vmin.z > T{0})
    {
        return false;
    }
    if (normal.x * vmax.x + normal.y * vmax.y + normal.z * vmax.z >= T{0})
    {
        return true;
    }
    return false;
}

// Exact triangle/AABB overlap test (Akenine-Möller 2001).
// `box_centre` is the AABB centre in world space. `half_extents` are the
// per-axis half sizes (always positive). `a`, `b`, `c` are the triangle
// vertices in world space.
template <typename T>
[[nodiscard]] inline bool tri_box_overlap_sat(const crd::math::Vec3<T>& box_centre,
                                              const crd::math::Vec3<T>& half_extents,
                                              const crd::math::Vec3<T>& a,
                                              const crd::math::Vec3<T>& b,
                                              const crd::math::Vec3<T>& c) noexcept
{
    using crd::math::Vec3;

    // Translate triangle so AABB centre is the origin.
    const Vec3<T> v0 { a.x - box_centre.x, a.y - box_centre.y, a.z - box_centre.z };
    const Vec3<T> v1 { b.x - box_centre.x, b.y - box_centre.y, b.z - box_centre.z };
    const Vec3<T> v2 { c.x - box_centre.x, c.y - box_centre.y, c.z - box_centre.z };

    // Three AABB face-normal axes — degenerate-triangle-tolerant via min/max.
    {
        const T xmin = std::min({v0.x, v1.x, v2.x});
        const T xmax = std::max({v0.x, v1.x, v2.x});
        if (xmin > half_extents.x || xmax < -half_extents.x) { return false; }
    }
    {
        const T ymin = std::min({v0.y, v1.y, v2.y});
        const T ymax = std::max({v0.y, v1.y, v2.y});
        if (ymin > half_extents.y || ymax < -half_extents.y) { return false; }
    }
    {
        const T zmin = std::min({v0.z, v1.z, v2.z});
        const T zmax = std::max({v0.z, v1.z, v2.z});
        if (zmin > half_extents.z || zmax < -half_extents.z) { return false; }
    }

    // Triangle edges.
    const Vec3<T> e0 { v1.x - v0.x, v1.y - v0.y, v1.z - v0.z };
    const Vec3<T> e1 { v2.x - v1.x, v2.y - v1.y, v2.z - v1.z };
    const Vec3<T> e2 { v0.x - v2.x, v0.y - v2.y, v0.z - v2.z };

    // 9 edge-cross axes — three per triangle edge.
    {
        const T fex = crd::math::abs(e0.x);
        const T fey = crd::math::abs(e0.y);
        const T fez = crd::math::abs(e0.z);
        if (axis_test_x01<T>( e0.z, e0.y, fez, fey, v0, v2, half_extents)) { return false; }
        if (axis_test_y02<T>( e0.z, e0.x, fez, fex, v0, v2, half_extents)) { return false; }
        if (axis_test_z12<T>( e0.y, e0.x, fey, fex, v1, v2, half_extents)) { return false; }
    }
    {
        const T fex = crd::math::abs(e1.x);
        const T fey = crd::math::abs(e1.y);
        const T fez = crd::math::abs(e1.z);
        if (axis_test_x01<T>( e1.z, e1.y, fez, fey, v0, v2, half_extents)) { return false; }
        if (axis_test_y02<T>( e1.z, e1.x, fez, fex, v0, v2, half_extents)) { return false; }
        // Note: AKM uses (v0, v1) for this axis (skipping v2); see paper.
        const T p0 = e1.y * v0.x - e1.x * v0.y;
        const T p1 = e1.y * v1.x - e1.x * v1.y;
        const T pmin = std::min(p0, p1);
        const T pmax = std::max(p0, p1);
        const T rad  = fey * half_extents.x + fex * half_extents.y;
        if (pmin > rad || pmax < -rad) { return false; }
    }
    {
        const T fex = crd::math::abs(e2.x);
        const T fey = crd::math::abs(e2.y);
        const T fez = crd::math::abs(e2.z);
        if (axis_test_x01<T>( e2.z, e2.y, fez, fey, v0, v1, half_extents)) { return false; }
        if (axis_test_y02<T>( e2.z, e2.x, fez, fex, v0, v1, half_extents)) { return false; }
        if (axis_test_z12<T>( e2.y, e2.x, fey, fex, v1, v2, half_extents)) { return false; }
    }

    // Triangle face-normal axis.
    const Vec3<T> normal {
        e0.y * e1.z - e0.z * e1.y,
        e0.z * e1.x - e0.x * e1.z,
        e0.x * e1.y - e0.y * e1.x,
    };
    if (!plane_box_overlap<T>(normal, v0, half_extents)) { return false; }

    return true;
}

} // namespace crd::geometry::decomposition::detail
