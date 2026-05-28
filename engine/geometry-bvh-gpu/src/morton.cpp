// ---------------------------------------------------------------------------
// CPU Morton-code reference implementation. THE ALGORITHM DEFINITION.
// The GPU kernel (engine/geometry-bvh-gpu/shaders/compute_morton_codes.comp) is
// a mechanical translation of the per-element loop body below.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh_gpu/morton.hpp>

#include <algorithm>
#include <limits>

namespace crd::geometry::bvh_gpu
{

crd::geometry::primitives::AABB3<crd::f32>
union_aabb_of(crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs) noexcept
{
    using Vec3 = crd::math::Vec3<crd::f32>;
    constexpr crd::f32 k_inf = std::numeric_limits<crd::f32>::infinity();
    if (aabbs.empty())
    {
        return {Vec3{ k_inf,  k_inf,  k_inf}, Vec3{-k_inf, -k_inf, -k_inf}};
    }
    Vec3 lo{ k_inf,  k_inf,  k_inf};
    Vec3 hi{-k_inf, -k_inf, -k_inf};
    for (const auto& b : aabbs)
    {
        lo.x = std::min(lo.x, b.min.x); hi.x = std::max(hi.x, b.max.x);
        lo.y = std::min(lo.y, b.min.y); hi.y = std::max(hi.y, b.max.y);
        lo.z = std::min(lo.z, b.min.z); hi.z = std::max(hi.z, b.max.z);
    }
    return {lo, hi};
}

namespace
{

[[nodiscard]] crd::math::Vec3<crd::f32>
inv_extent_of(const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb) noexcept
{
    // Reciprocal of (max - min) per axis. A zero / near-zero extent
    // ⇒ inv = 0, which makes `quantize_to_morton_grid` map every
    // centroid to bin 0 along that axis (correct for a degenerate flat
    // scene). Avoids producing inf-or-NaN that would propagate.
    crd::math::Vec3<crd::f32> inv{};
    const crd::f32 ex = scene_aabb.max.x - scene_aabb.min.x;
    const crd::f32 ey = scene_aabb.max.y - scene_aabb.min.y;
    const crd::f32 ez = scene_aabb.max.z - scene_aabb.min.z;
    inv.x = ex > 0.0F ? (1.0F / ex) : 0.0F;
    inv.y = ey > 0.0F ? (1.0F / ey) : 0.0F;
    inv.z = ez > 0.0F ? (1.0F / ez) : 0.0F;
    return inv;
}

} // namespace

crd::containers::Array<crd::u32>
compute_morton_codes_cpu(crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
                          const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
                          crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::Array<crd::u32> out(alloc);
    if (aabbs.empty())
    {
        return out;
    }
    out.resize(aabbs.size(), 0U);

    const auto inv_extent = inv_extent_of(scene_aabb);
    for (crd::usize i = 0U; i < aabbs.size(); ++i)
    {
        const auto& b = aabbs[i];
        const crd::math::Vec3<crd::f32> centroid{
            0.5F * (b.min.x + b.max.x),
            0.5F * (b.min.y + b.max.y),
            0.5F * (b.min.z + b.max.z),
        };
        out[i] = morton3_30bit_for_centroid(centroid, scene_aabb.min, inv_extent);
    }
    return out;
}

crd::containers::Array<crd::u32>
compute_morton_codes_cpu(crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
                          crd::memory::IAllocator* alloc) noexcept
{
    return compute_morton_codes_cpu(aabbs, union_aabb_of(aabbs), alloc);
}

} // namespace crd::geometry::bvh_gpu
