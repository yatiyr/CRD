// The robust slab ray-AABB test on plain components (crd/geometry/primitives/robust_ray_aabb.hpp), shared by general
// code and the geometry kernel units (SIMD-DISPATCH, ADR-0134 §5). It lives in the level's namespace, so each unit
// compiles its own copy; every copy gives the same bits.

#pragma once

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>

namespace crd::geometry::primitives::detail::inline CRD_MATH_LEVEL_NS
{

// Williams 2005: the ray (origin o, 1/direction inv) enters each axis by the max plane where neg[axis] (a negative
// inverse direction) and by the min plane otherwise; tmax is widened by pad (Ize 2013). On an overlap with [t0, t1],
// out_t is its start.
template <typename T>
[[nodiscard]] inline bool ray_aabb_slab(const T (&o)[3], const T (&inv)[3], const bool (&neg)[3], const T (&lo)[3],
                                        const T (&hi)[3], T pad, T t0, T t1, T& out_t) noexcept
{
    T tmin = ((neg[0] ? hi[0] : lo[0]) - o[0]) * inv[0];
    T tmax = ((neg[0] ? lo[0] : hi[0]) - o[0]) * inv[0];
    const T tymin = ((neg[1] ? hi[1] : lo[1]) - o[1]) * inv[1];
    const T tymax = ((neg[1] ? lo[1] : hi[1]) - o[1]) * inv[1];
    if (tmin > tymax || tymin > tmax)
    {
        return false;
    }
    if (tymin > tmin)
    {
        tmin = tymin;
    }
    if (tymax < tmax)
    {
        tmax = tymax;
    }
    const T tzmin = ((neg[2] ? hi[2] : lo[2]) - o[2]) * inv[2];
    const T tzmax = ((neg[2] ? lo[2] : hi[2]) - o[2]) * inv[2];
    if (tmin > tzmax || tzmin > tmax)
    {
        return false;
    }
    if (tzmin > tmin)
    {
        tmin = tzmin;
    }
    if (tzmax < tmax)
    {
        tmax = tzmax;
    }
    tmax *= pad;
    const T start = tmin > t0 ? tmin : t0;
    const T end = tmax < t1 ? tmax : t1;
    if (start > end)
    {
        return false;
    }
    out_t = start;
    return true;
}

} // namespace crd::geometry::primitives::detail::inline CRD_MATH_LEVEL_NS
