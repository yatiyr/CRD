// ---------------------------------------------------------------------------
// 60-bit Morton CPU reference. Mirror of morton.cpp 30-bit path; lifted
// to u64 + 20 bits per axis. THE ALGORITHM DEFINITION for the GPU 60-bit
// kernel (v9a-60bit-gpu) — that shader is a mechanical translation of
// the per-element loop body below.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh_gpu/morton.hpp>          // for `union_aabb_of`
#include <crd/geometry/bvh_gpu/morton_60bit.hpp>

#include <algorithm>
#include <limits>

namespace crd::geometry::bvh_gpu
{

namespace
{

[[nodiscard]] crd::math::Vec3<crd::f32>
inv_extent_of_for_60(const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb) noexcept
{
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

crd::containers::Array<std::uint64_t>
compute_morton_codes_cpu_60bit(
    crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
    const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
    crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::Array<std::uint64_t> out(alloc);
    if (aabbs.empty())
    {
        return out;
    }
    out.resize(aabbs.size(), std::uint64_t{0});

    const auto inv_extent = inv_extent_of_for_60(scene_aabb);
    for (crd::usize i = 0U; i < aabbs.size(); ++i)
    {
        const auto& b = aabbs[i];
        const crd::math::Vec3<crd::f32> centroid{
            0.5F * (b.min.x + b.max.x),
            0.5F * (b.min.y + b.max.y),
            0.5F * (b.min.z + b.max.z),
        };
        out[i] = morton3_60bit_for_centroid(centroid, scene_aabb.min, inv_extent);
    }
    return out;
}

crd::containers::Array<std::uint64_t>
compute_morton_codes_cpu_60bit(
    crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
    crd::memory::IAllocator* alloc) noexcept
{
    return compute_morton_codes_cpu_60bit(aabbs, union_aabb_of(aabbs), alloc);
}

} // namespace crd::geometry::bvh_gpu
