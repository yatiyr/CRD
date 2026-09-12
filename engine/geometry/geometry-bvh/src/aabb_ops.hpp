#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — internal AABB scratch ops shared by the builder and the
// updaters. NOT a public header (lives under src/); these are the rounding-free
// min/max accumulation primitives the BVH leans on. `crd::geometry::bvh::detail`.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>

#include <limits>

namespace crd::geometry::bvh::detail
{
using crd::f32;
using crd::u32;
using crd::geometry::primitives::AABB3;
using crd::math::Vec3;

// An "empty" AABB ready for accumulation (min = +∞, max = −∞).
[[nodiscard]] inline AABB3<f32> aabb_empty() noexcept
{
    constexpr f32 inf = std::numeric_limits<f32>::infinity();
    return AABB3<f32>(Vec3<f32>(inf, inf, inf), Vec3<f32>(-inf, -inf, -inf));
}

inline void aabb_include_point(AABB3<f32>& a, const Vec3<f32>& p) noexcept
{
    a.min.x = p.x < a.min.x ? p.x : a.min.x;
    a.min.y = p.y < a.min.y ? p.y : a.min.y;
    a.min.z = p.z < a.min.z ? p.z : a.min.z;
    a.max.x = p.x > a.max.x ? p.x : a.max.x;
    a.max.y = p.y > a.max.y ? p.y : a.max.y;
    a.max.z = p.z > a.max.z ? p.z : a.max.z;
}

// Grow `a` to also enclose `b` — the proper AABB union: componentwise min of
// the mins, max of the maxes. (NOT `aabb_include_point(a, b.min); aabb_include_point(a, b.max);`
// — that misbehaves when `b` is the "empty" sentinel `{min=+∞, max=−∞}`,
// because including `+∞` as a point would push `a.max` to `+∞`. Merging the
// empty sentinel must leave `a` unchanged, which the corner form does.)
inline void aabb_merge(AABB3<f32>& a, const AABB3<f32>& b) noexcept
{
    a.min.x = b.min.x < a.min.x ? b.min.x : a.min.x;
    a.min.y = b.min.y < a.min.y ? b.min.y : a.min.y;
    a.min.z = b.min.z < a.min.z ? b.min.z : a.min.z;
    a.max.x = b.max.x > a.max.x ? b.max.x : a.max.x;
    a.max.y = b.max.y > a.max.y ? b.max.y : a.max.y;
    a.max.z = b.max.z > a.max.z ? b.max.z : a.max.z;
}

[[nodiscard]] inline Vec3<f32> aabb_centroid(const AABB3<f32>& a) noexcept
{
    return Vec3<f32>((a.min.x + a.max.x) * 0.5F, (a.min.y + a.max.y) * 0.5F, (a.min.z + a.max.z) * 0.5F);
}

// Half the surface area: e.x·e.y + e.y·e.z + e.z·e.x. Zero for empty/degenerate.
[[nodiscard]] inline f32 aabb_half_area(const AABB3<f32>& a) noexcept
{
    const f32 ex = a.max.x - a.min.x;
    const f32 ey = a.max.y - a.min.y;
    const f32 ez = a.max.z - a.min.z;
    if (ex < 0.0F || ey < 0.0F || ez < 0.0F)
    {
        return 0.0F;
    }
    return ex * ey + ey * ez + ez * ex;
}

[[nodiscard]] inline f32 component(const Vec3<f32>& v, int axis) noexcept
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

// Bounds of `prims[idx[first .. first+count)]`.
[[nodiscard]] inline AABB3<f32> bounds_of_range(const AABB3<f32>* prims, const u32* idx, u32 first, u32 count) noexcept
{
    AABB3<f32> b = aabb_empty();
    for (u32 i = first; i < first + count; ++i)
    {
        aabb_merge(b, prims[idx[i]]);
    }
    return b;
}

} // namespace crd::geometry::bvh::detail
