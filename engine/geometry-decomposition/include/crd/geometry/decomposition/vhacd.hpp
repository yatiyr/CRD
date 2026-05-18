#pragma once

// ---------------------------------------------------------------------------
// crd::geometry::decomposition::vhacd_decompose — V-HACD (Mamou 2014)
// recursive approximate convex decomposition. Phase 3.1.7 v9c-b.
//
// Consumes a `VoxelGrid` (from v9c-a `voxelize_mesh`) and produces an
// array of convex hulls — one per leaf cluster of the recursive split.
// Cooker-only — not runtime; primary consumer is eylem v1c convex
// collider conditioning.
//
// Algorithm (Mamou §3.2-3.4) — recursive plane-search:
//
//   1. Start with one cluster = all Surface ∪ Inside voxels.
//   2. While (any cluster has concavity > min_concavity) AND (num_parts < max_parts):
//      a. Pick the worst (highest-concavity) cluster.
//      b. Search axis-aligned planes (3 axes × N positions per cluster
//         AABB) for the minimum-cost split via Mamou cost function:
//             cost(plane) = concavity_left + concavity_right
//                         + alpha · |size_left - size_right| / size_total
//                         + beta  · symmetry_penalty(plane)
//      c. Split the worst cluster along the best plane.
//   3. For each leaf cluster, run Quickhull on its surface voxel centres.
//   4. Output `Array<QuickhullResult<f32>>` — one per leaf.
//
// Concavity (D129) is the **voxel-fraction** form:
//     concavity(C) = 1 - |C_voxels| / |hull_voxels(C)|
// where hull_voxels(C) = voxels inside convex_hull(C)'s AABB that pass
// the convex-hull contains-point test. Clamped to [0, 1]; a convex
// cluster (e.g. cube, sphere) has concavity = 0. This DIVERGES from
// Mamou's original Hausdorff distance metric — modern V-HACD
// implementations universally use the voxel-fraction form for cooker-
// budget speed. Pinned for ADR-0076 §24 amendment at v9c-close.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/convex/quickhull.hpp>
#include <crd/geometry/decomposition/voxel.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::decomposition
{

enum class VhacdStatus : crd::u8
{
    Ok = 0,
    EmptyGrid,         // input grid has zero voxels OR zero Surface/Inside voxels
    InvalidOptions,    // max_parts == 0, min_concavity outside [0,1], ...
    OutOfMemory,
    HullBuildFailed,   // Quickhull rejected the cluster's voxel cloud
    InternalInvariant, // bug — file a debt entry
};

[[nodiscard]] constexpr const char* to_string(VhacdStatus s) noexcept
{
    switch (s)
    {
        case VhacdStatus::Ok:                return "Ok";
        case VhacdStatus::EmptyGrid:         return "EmptyGrid";
        case VhacdStatus::InvalidOptions:    return "InvalidOptions";
        case VhacdStatus::OutOfMemory:       return "OutOfMemory";
        case VhacdStatus::HullBuildFailed:   return "HullBuildFailed";
        case VhacdStatus::InternalInvariant: return "InternalInvariant";
    }
    return "?";
}

struct VhacdOptions
{
    // Stop splitting when every cluster has concavity below this. Default
    // 0.05 (a 5%-deviation-from-convex-hull is "close enough to convex").
    crd::f32 min_concavity = 0.05F;

    // Hard cap on the number of output parts. Default 32 (matches the
    // V-HACD reference default; covers most game/eylem collider cases).
    crd::u32 max_parts = 32U;

    // Maximum recursion depth. Acts as a sanity cap; budget overflow trips
    // a graceful exit (not InternalInvariant). Default 16.
    crd::u32 max_depth = 16U;

    // Number of candidate split positions per axis. Higher = better splits
    // but more search cost. Default 16 ⇒ 3 axes × 16 = 48 candidates per
    // cluster. Mamou's reference uses 16-32 depending on quality tier.
    crd::u32 splits_per_axis = 16U;

    // Mamou cost-function weights. Defaults from the V-HACD reference:
    // alpha = 0.05 (imbalance penalty), beta = 0.05 (symmetry penalty).
    crd::f32 alpha_imbalance = 0.05F;
    crd::f32 beta_symmetry   = 0.05F;
};

struct VhacdResult
{
    // One convex hull per leaf cluster. Each is a complete owned-arrays
    // QuickhullResult; build a non-owning ConvexHullView via the existing
    // `crd::geometry::convex::convex_hull_view_of(parts[i])` helper.
    crd::containers::Array<crd::geometry::convex::QuickhullResult<crd::f32>> parts;

    // Telemetry.
    crd::u32    total_input_voxels       = 0;
    crd::u32    max_recursion_depth_seen = 0;
    crd::f32    max_part_concavity       = 0.0F;
    VhacdStatus status                    = VhacdStatus::Ok;

    explicit VhacdResult(crd::memory::IAllocator* alloc) noexcept : parts(alloc) {}
    VhacdResult(const VhacdResult&)            = delete;
    VhacdResult& operator=(const VhacdResult&) = delete;
    VhacdResult(VhacdResult&&)                 = default;
    VhacdResult& operator=(VhacdResult&&)      = default;
    ~VhacdResult()                              = default;
};

// Decompose `grid` (output of v9c-a `voxelize_mesh`) into approximately
// convex parts per V-HACD (Mamou 2014). Reads cells with state Surface
// or Inside as the "solid" volume; Outside / Unknown are ignored.
//
// `grid_aabb` and `voxel_size_world` come from the VoxelizationResult.
// All cluster voxel coordinates are emitted in world space using these.
[[nodiscard]] VhacdResult
vhacd_decompose(const VoxelGrid& grid,
                const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
                const crd::math::Vec3<crd::f32>&                  voxel_size_world,
                const VhacdOptions&                               opts,
                crd::memory::IAllocator*                          alloc) noexcept;

} // namespace crd::geometry::decomposition
