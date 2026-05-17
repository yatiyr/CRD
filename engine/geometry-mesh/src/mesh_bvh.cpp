// crd-geometry-mesh — TriangleMeshBvh builder (v4a).

#include <crd/geometry/mesh/mesh_bvh.hpp>

#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh_build.hpp>

namespace crd::geometry::mesh
{

using crd::geometry::primitives::AABB3;
using crd::math::Vec3;

TriangleMeshBvh build_triangle_mesh_bvh(const TriangleMeshViewf& view,
                                          crd::memory::IAllocator*  alloc)
{
    CRD_ASSERT(alloc != nullptr);
    CRD_ASSERT(view.indices.size() % 3U == 0U);

    TriangleMeshBvh out{alloc};
    const crd::u32 tri_count = view.triangle_count();
    if (tri_count == 0U)
    {
        return out;
    }

    out.triangle_aabbs.resize(static_cast<crd::usize>(tri_count));
    for (crd::u32 ti = 0U; ti < tri_count; ++ti)
    {
        const crd::u32 i0 = view.indices[ti * 3U + 0U];
        const crd::u32 i1 = view.indices[ti * 3U + 1U];
        const crd::u32 i2 = view.indices[ti * 3U + 2U];
        CRD_ASSERT(i0 < view.vertices.size());
        CRD_ASSERT(i1 < view.vertices.size());
        CRD_ASSERT(i2 < view.vertices.size());
        const Vec3<crd::f32>& v0 = view.vertices[i0];
        const Vec3<crd::f32>& v1 = view.vertices[i1];
        const Vec3<crd::f32>& v2 = view.vertices[i2];

        Vec3<crd::f32> bmin{std::min({v0.x, v1.x, v2.x}),
                            std::min({v0.y, v1.y, v2.y}),
                            std::min({v0.z, v1.z, v2.z})};
        Vec3<crd::f32> bmax{std::max({v0.x, v1.x, v2.x}),
                            std::max({v0.y, v1.y, v2.y}),
                            std::max({v0.z, v1.z, v2.z})};
        out.triangle_aabbs[ti] = AABB3<crd::f32>{bmin, bmax};
    }

    out.tree = crd::geometry::bvh::bvh_build(
        crd::containers::ConstSpan<AABB3<crd::f32>>{
            out.triangle_aabbs.data(), out.triangle_aabbs.size()},
        alloc);

    return out;
}

} // namespace crd::geometry::mesh
