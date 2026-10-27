// crd-geometry-primitives — the batch intersection kernels (v0f): one call of the level's kernel per span
// (crd/geometry/primitives/simd_batch.hpp).

#include <crd/core/assert.hpp>
#include <crd/geometry/primitives/primitive_dispatch.hpp>
#include <crd/geometry/primitives/simd_batch.hpp>
#include <crd/math/scalar.hpp>

#include <limits>
#include <type_traits>

namespace crd::geometry::primitives
{

// The packet kernel reads AABB3<f32> spans as Box3 (min, then max, three components each).
static_assert(std::is_standard_layout_v<AABB3<crd::f32>> && sizeof(AABB3<crd::f32>) == sizeof(Box3),
              "AABB3<f32> must lay out as Box3");

namespace
{

Box3 box3(const AABB3<crd::f32>& box) noexcept
{
    return Box3{{box.min.x, box.min.y, box.min.z}, {box.max.x, box.max.y, box.max.z}};
}

} // namespace

RayPacket8 precompute_ray_packet8(const Ray3<crd::f32> (&rays)[kBatchLanes]) noexcept
{
    RayPacket8 p{};
    for (crd::usize l = 0; l < kBatchLanes; ++l)
    {
        p.origin_x[l] = rays[l].origin.x;
        p.origin_y[l] = rays[l].origin.y;
        p.origin_z[l] = rays[l].origin.z;
        p.inv_dir_x[l] = 1.0F / rays[l].direction.x;
        p.inv_dir_y[l] = 1.0F / rays[l].direction.y;
        p.inv_dir_z[l] = 1.0F / rays[l].direction.z;
    }
    return p;
}

void ray_vs_8_aabb(const Ray3<crd::f32>& ray, const RayAABBPrecompute<crd::f32>& pre,
                   crd::containers::ConstSpan<Aabb8> boxes, crd::f32 t0, crd::f32 t1,
                   crd::containers::Span<RayAabb8Result> out) noexcept
{
    CRD_ASSERT(out.size() == boxes.size());
    primitive_kernels().table->ray_vs_aabb8(ray_slab(ray, pre), boxes.data(), boxes.size(), t0, t1, out.data());
}

void ray_packet8_vs_aabb(const RayPacket8& packet, crd::containers::ConstSpan<AABB3<crd::f32>> boxes, crd::f32 t0,
                         crd::f32 t1, crd::containers::Span<RayAabb8Result> out) noexcept
{
    CRD_ASSERT(out.size() == boxes.size());
    primitive_kernels().table->packet8_vs_aabb(packet, reinterpret_cast<const Box3*>(boxes.data()), boxes.size(),
                                               ray_aabb_robust_pad<crd::f32>(), t0, t1, out.data());
}

void ray_vs_8_triangle(const Ray3<crd::f32>& ray, crd::containers::ConstSpan<Triangle38> tris,
                       crd::containers::Span<RayTri8Result> out, bool cull_back, crd::f32 tnear) noexcept
{
    CRD_ASSERT(out.size() == tris.size());
    // The triangle kernel reads the ray's origin and direction only, so it takes no slab precompute.
    primitive_kernels().table->ray_vs_triangle8(ray_slab(ray, {}), tris.data(), tris.size(), cull_back, tnear,
                                                crd::math::default_epsilon<crd::f32>(), out.data());
}

void aabb8_vs_aabb(crd::containers::ConstSpan<Aabb8> boxes, const AABB3<crd::f32>& box,
                   crd::containers::Span<Mask8> out) noexcept
{
    CRD_ASSERT(out.size() == boxes.size());
    primitive_kernels().table->aabb8_vs_aabb(boxes.data(), boxes.size(), box3(box), out.data());
}

void sphere8_vs_sphere(crd::containers::ConstSpan<Sphere8> spheres, const Sphere<crd::f32>& sphere,
                       crd::containers::Span<Mask8> out) noexcept
{
    CRD_ASSERT(out.size() == spheres.size());
    const crd::f32 center[3] = {sphere.center.x, sphere.center.y, sphere.center.z};
    primitive_kernels().table->sphere8_vs_sphere(spheres.data(), spheres.size(), center, sphere.radius, out.data());
}

void segment8_vs_segment_distsq(crd::containers::ConstSpan<Segment38Pair> pairs,
                                crd::containers::Span<Lanes8> out) noexcept
{
    CRD_ASSERT(out.size() == pairs.size());
    // The smallest normal: the "truly zero" threshold of squared lengths and determinants, and the pad that keeps a
    // divide finite where its denominator is 0 (the surrounding select then discards the lane).
    primitive_kernels().table->segment8_distsq(pairs.data(), pairs.size(), std::numeric_limits<crd::f32>::min(),
                                               out.data());
}

} // namespace crd::geometry::primitives
