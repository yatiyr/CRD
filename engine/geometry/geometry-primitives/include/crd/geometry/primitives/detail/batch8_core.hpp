// Moller-Trumbore on a register of triangles, for the kernel units of geometry-primitives and geometry-mesh
// (SIMD-DISPATCH, ADR-0134 §5): eight on the level's Vec8f, sixteen on Vec16f at avx512. It lives in the level's
// namespace, so each unit compiles its own copy on its level's types.

#pragma once

#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/simd/vec8f.hpp>

namespace crd::geometry::primitives::detail::inline CRD_MATH_LEVEL_NS
{

template <typename V> struct Mt
{
    V det;
    V u; // barycentric weight of b
    V v; // barycentric weight of c
    V t;
};

// det, u, v and t of the ray (o, d) against the triangles (a, b, c) of each lane, two roundings a step: u, v and t are
// their numerators times 1/det, infinite where det is zero (the caller's |det| test rejects those lanes). The lanes
// are independent, so every register width gives each lane the same bits.
template <typename V>
[[nodiscard]] CRD_FORCEINLINE Mt<V> moller_trumbore(const crd::f32 (&o)[3], const crd::f32 (&d)[3], V ax, V ay, V az,
                                                    V bx, V by, V bz, V cx, V cy, V cz) noexcept
{
    const V ox(o[0]);
    const V oy(o[1]);
    const V oz(o[2]);
    const V dx(d[0]);
    const V dy(d[1]);
    const V dz(d[2]);

    const V e1x = bx - ax;
    const V e1y = by - ay;
    const V e1z = bz - az;
    const V e2x = cx - ax;
    const V e2y = cy - ay;
    const V e2z = cz - az;

    // pvec = cross(d, e2)
    const V px = dy * e2z - dz * e2y;
    const V py = dz * e2x - dx * e2z;
    const V pz = dx * e2y - dy * e2x;
    const V det = e1x * px + e1y * py + e1z * pz;
    const V inv_det = V::one() / det;

    const V tvx = ox - ax;
    const V tvy = oy - ay;
    const V tvz = oz - az;
    const V u = (tvx * px + tvy * py + tvz * pz) * inv_det;

    // qvec = cross(tvec, e1)
    const V qx = tvy * e1z - tvz * e1y;
    const V qy = tvz * e1x - tvx * e1z;
    const V qz = tvx * e1y - tvy * e1x;
    const V v = (dx * qx + dy * qy + dz * qz) * inv_det;
    const V t = (e2x * qx + e2y * qy + e2z * qz) * inv_det;
    return Mt<V>{det, u, v, t};
}

} // namespace crd::geometry::primitives::detail::inline CRD_MATH_LEVEL_NS
