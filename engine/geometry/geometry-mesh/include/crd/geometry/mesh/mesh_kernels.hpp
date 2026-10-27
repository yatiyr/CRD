// The table of the geometry-mesh kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
// (engine/geometry/geometry-mesh/kernels/mesh_kernels.cpp), resolved by mesh_kernels() in
// crd/geometry/mesh/mesh_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing from
// it. The raycast is bit-exact across levels (ADR-0063, ADR-0134 §7).

#pragma once

#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitive_kernels.hpp>

namespace crd::geometry::mesh
{

// The bytes of a mesh and its BVH a kernel reads (by memcpy, at these strides and offsets): Vec3<f32> vertices,
// three u32 indices a triangle, and BvhNode (crd/geometry/bvh/bvh_tree.hpp) nodes: the bounds as min then max, the
// first child or primitive, and the primitive count (0 for an interior node, whose children are left_first and
// left_first + 1).
inline constexpr crd::usize kVertexBytes = 12;
inline constexpr crd::usize kBvhNodeBytes = 32;
inline constexpr crd::usize kBvhNodeLeftFirst = 24;
inline constexpr crd::usize kBvhNodePrimCount = 28;
inline constexpr crd::usize kBvhStackDepth = 64; // bvh::k_max_bvh_depth

struct MeshBvhView
{
    const unsigned char* vertices;
    const crd::u32* indices;
    const unsigned char* nodes;
    const crd::u32* prim_indices;
    crd::u32 root;
};

struct MeshRayHitData
{
    crd::f32 t;
    crd::u32 triangle;
    crd::f32 bary[3]; // weights of the triangle's vertices 0, 1, 2
};

struct MeshKernels
{
    // The nearest hit in [0, tmax] of the ray against the mesh (nearer child first, Moller-Trumbore on eight leaf
    // triangles a step, the lowest triangle index on an equal t); false when there is none.
    bool (*raycast_mt8)(const MeshBvhView& mesh, const crd::geometry::primitives::RaySlab& ray, crd::f32 tmax,
                        bool cull_back, MeshRayHitData& hit) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const MeshKernels kMeshKernelsPortable;
extern const MeshKernels kMeshKernelsSse2;
extern const MeshKernels kMeshKernelsAvx2;

} // namespace crd::geometry::mesh
