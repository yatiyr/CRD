#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh — TriangleMeshBvh: per-triangle AABB + BvhTree (v4a).
//
// Bundles a built BVH with the per-triangle AABB array it indexes. The mesh
// queries (`closest_point`, `raycast`, `winding_number`) consume one of these
// alongside the source `TriangleMeshView`. Built once per mesh; reused across
// thousands of queries.
//
// API:
//   * `build_triangle_mesh_bvh(view, alloc)` → `TriangleMeshBvh`.
//   * `TriangleMeshBvh::triangle_aabbs()` → the per-triangle AABBs (the BVH's
//     leaf prims).
//   * `TriangleMeshBvh::tree()` → the `BvhTree` indexing those AABBs.
//
// Determinism: builder is the v1f-serial `bvh_build` (binned-SAH, lex-tie
// split per ADR-0076 §5.2). Per-triangle AABBs use raw min/max over the
// three vertex positions — bit-exact across compilers / OSes.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh/bvh_build.hpp>
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh
{

struct TriangleMeshBvh
{
    crd::containers::Array<crd::geometry::primitives::AABB3<crd::f32>> triangle_aabbs;
    crd::geometry::bvh::BvhTree                                         tree;

    explicit TriangleMeshBvh(crd::memory::IAllocator* alloc) noexcept
        : triangle_aabbs(alloc), tree(alloc)
    {
    }

    [[nodiscard]] bool is_empty() const noexcept { return triangle_aabbs.empty(); }
};

// Build a per-triangle AABB array + a binned-SAH BVH over it. The result's
// storage binds to `alloc`. Returns an empty `TriangleMeshBvh` for an empty
// `view`.
[[nodiscard]] TriangleMeshBvh build_triangle_mesh_bvh(const TriangleMeshViewf& view,
                                                       crd::memory::IAllocator*  alloc);

} // namespace crd::geometry::mesh
