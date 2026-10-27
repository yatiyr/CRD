// crd-geometry-mesh — per-leaf SIMD Moller-Trumbore raycast (v4d): one call of the level's raycast
// (crd/geometry/mesh/mesh_kernels.hpp) per ray.

#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/mesh/mesh_dispatch.hpp>
#include <crd/geometry/mesh/mesh_raycast_simd.hpp>
#include <crd/geometry/primitives/primitive_dispatch.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>

#include <cstddef>
#include <type_traits>

namespace crd::geometry::mesh
{

using crd::geometry::bvh::BvhNode;

// The kernel reads the vertices and nodes as bytes at mesh_kernels.hpp's strides and offsets.
static_assert(sizeof(crd::math::Vec3<crd::f32>) == kVertexBytes, "Vec3<f32> is three packed f32");
static_assert(std::is_standard_layout_v<BvhNode> && sizeof(BvhNode) == kBvhNodeBytes);
static_assert(offsetof(BvhNode, bounds) == 0 && sizeof(BvhNode::bounds) == 6 * sizeof(crd::f32));
static_assert(offsetof(BvhNode, left_first) == kBvhNodeLeftFirst && sizeof(BvhNode::left_first) == 4);
static_assert(offsetof(BvhNode, prim_count) == kBvhNodePrimCount && sizeof(BvhNode::prim_count) == 2);
static_assert(crd::geometry::bvh::k_max_bvh_depth == kBvhStackDepth);

std::optional<MeshRayHit> mesh_raycast_simd(const TriangleMeshViewf& view, const TriangleMeshBvh& bvh,
                                            const crd::geometry::primitives::Ray3<crd::f32>& ray, crd::f32 tmax,
                                            bool cull_back) noexcept
{
    if (view.is_empty() || bvh.is_empty())
    {
        return std::nullopt;
    }
    const MeshBvhView mesh{reinterpret_cast<const unsigned char*>(view.vertices.data()), view.indices.data(),
                           reinterpret_cast<const unsigned char*>(bvh.tree.nodes().data()),
                           bvh.tree.prim_indices().data(), bvh.tree.root()};
    const crd::geometry::primitives::RaySlab slab =
        crd::geometry::primitives::ray_slab(ray, crd::geometry::primitives::precompute_ray_aabb(ray));
    MeshRayHitData hit{};
    if (!mesh_kernels().table->raycast_mt8(mesh, slab, tmax, cull_back, hit))
    {
        return std::nullopt;
    }
    MeshRayHit out{};
    out.t = hit.t;
    out.payload = MeshHitPayload{hit.triangle, crd::math::Vec3<crd::f32>{hit.bary[0], hit.bary[1], hit.bary[2]}};
    return out;
}

} // namespace crd::geometry::mesh
