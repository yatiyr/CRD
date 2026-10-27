// Moller-Trumbore on eight triangles, for the kernel units of geometry-primitives and geometry-mesh (SIMD-DISPATCH,
// ADR-0134 §5). It lives in the level's namespace, so each unit compiles its own copy on its level's Vec8f.

#pragma once

#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/simd/vec8f.hpp>

namespace crd::geometry::primitives::detail::inline CRD_MATH_LEVEL_NS
{

struct Mt8
{
    crd::math::simd::Vec8f det;
    crd::math::simd::Vec8f u; // barycentric weight of b
    crd::math::simd::Vec8f v; // barycentric weight of c
    crd::math::simd::Vec8f t;
};

// det, u, v and t of the ray (o, d) against the eight triangles (a, b, c), two roundings a step: u, v and t are
// their numerators times 1/det, infinite where det is zero (the caller's |det| test rejects those lanes).
[[nodiscard]] CRD_FORCEINLINE Mt8 moller_trumbore8(const crd::f32 (&o)[3], const crd::f32 (&d)[3],
                                                   crd::math::simd::Vec8f ax, crd::math::simd::Vec8f ay,
                                                   crd::math::simd::Vec8f az, crd::math::simd::Vec8f bx,
                                                   crd::math::simd::Vec8f by, crd::math::simd::Vec8f bz,
                                                   crd::math::simd::Vec8f cx, crd::math::simd::Vec8f cy,
                                                   crd::math::simd::Vec8f cz) noexcept
{
    using crd::math::simd::Vec8f;
    const Vec8f ox(o[0]);
    const Vec8f oy(o[1]);
    const Vec8f oz(o[2]);
    const Vec8f dx(d[0]);
    const Vec8f dy(d[1]);
    const Vec8f dz(d[2]);

    const Vec8f e1x = bx - ax;
    const Vec8f e1y = by - ay;
    const Vec8f e1z = bz - az;
    const Vec8f e2x = cx - ax;
    const Vec8f e2y = cy - ay;
    const Vec8f e2z = cz - az;

    // pvec = cross(d, e2)
    const Vec8f px = dy * e2z - dz * e2y;
    const Vec8f py = dz * e2x - dx * e2z;
    const Vec8f pz = dx * e2y - dy * e2x;
    const Vec8f det = e1x * px + e1y * py + e1z * pz;
    const Vec8f inv_det = Vec8f::one() / det;

    const Vec8f tvx = ox - ax;
    const Vec8f tvy = oy - ay;
    const Vec8f tvz = oz - az;
    const Vec8f u = (tvx * px + tvy * py + tvz * pz) * inv_det;

    // qvec = cross(tvec, e1)
    const Vec8f qx = tvy * e1z - tvz * e1y;
    const Vec8f qy = tvz * e1x - tvx * e1z;
    const Vec8f qz = tvx * e1y - tvy * e1x;
    const Vec8f v = (dx * qx + dy * qy + dz * qz) * inv_det;
    const Vec8f t = (e2x * qx + e2y * qy + e2z * qz) * inv_det;
    return Mt8{det, u, v, t};
}

} // namespace crd::geometry::primitives::detail::inline CRD_MATH_LEVEL_NS
