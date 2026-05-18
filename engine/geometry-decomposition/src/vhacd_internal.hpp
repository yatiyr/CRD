#pragma once

// ---------------------------------------------------------------------------
// V-HACD internal helpers. Phase 3.1.7 v9c-b. Not a public header.
//
// `VoxelCluster` — a subset of voxels from a VoxelGrid, identified by
// linear indices into the grid. Carries a cached AABB (in voxel space)
// so plane-search doesn't re-scan to find min/max each iteration.
//
// Surface voxels of a cluster = those whose voxel state is Surface OR
// any 6-neighbor inside the grid is not in the cluster (i.e. exposed to
// air or to a different cluster after a split). These centres are the
// input to Quickhull when computing the cluster's convex hull.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/convex/quickhull.hpp>
#include <crd/geometry/decomposition/voxel.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::decomposition::detail
{

// A cluster of voxels from a VoxelGrid. `voxel_indices` holds linear
// indices into the grid (computed via `ix + nx*(iy + ny*iz)`). Cached
// AABB is in voxel-index space (inclusive min, exclusive max — same as
// the slice convention).
struct VoxelCluster
{
    crd::containers::Array<crd::u32> voxel_indices;
    // Voxel-index-space AABB. min ≤ index_components < max.
    crd::u32 min_ix = 0;
    crd::u32 min_iy = 0;
    crd::u32 min_iz = 0;
    crd::u32 max_ix = 0; // exclusive
    crd::u32 max_iy = 0; // exclusive
    crd::u32 max_iz = 0; // exclusive

    explicit VoxelCluster(crd::memory::IAllocator* alloc) noexcept : voxel_indices(alloc) {}
    VoxelCluster(const VoxelCluster&)            = delete;
    VoxelCluster& operator=(const VoxelCluster&) = delete;
    VoxelCluster(VoxelCluster&&) noexcept       = default;
    VoxelCluster& operator=(VoxelCluster&&) noexcept = default;
    ~VoxelCluster()                              = default;

    [[nodiscard]] crd::u32 voxel_count() const noexcept
    {
        return static_cast<crd::u32>(voxel_indices.size());
    }

    [[nodiscard]] crd::u32 aabb_voxel_count() const noexcept
    {
        const crd::u32 ex = max_ix > min_ix ? (max_ix - min_ix) : 0U;
        const crd::u32 ey = max_iy > min_iy ? (max_iy - min_iy) : 0U;
        const crd::u32 ez = max_iz > min_iz ? (max_iz - min_iz) : 0U;
        return ex * ey * ez;
    }
};

// Decode a linear voxel index to (ix, iy, iz) for the given grid.
inline void decode_linear(const VoxelGrid& grid, crd::u32 linear,
                          crd::u32& out_ix, crd::u32& out_iy, crd::u32& out_iz) noexcept
{
    out_ix = linear % grid.nx();
    const crd::u32 r = linear / grid.nx();
    out_iy = r % grid.ny();
    out_iz = r / grid.ny();
}

// Encode (ix, iy, iz) to linear.
inline crd::u32 encode_linear(const VoxelGrid& grid,
                              crd::u32 ix, crd::u32 iy, crd::u32 iz) noexcept
{
    return ix + grid.nx() * (iy + grid.ny() * iz);
}

// Build the initial root cluster — all Surface OR Inside voxels in the
// grid. Caller owns the returned VoxelCluster.
[[nodiscard]] VoxelCluster
build_root_cluster(const VoxelGrid& grid, crd::memory::IAllocator* alloc) noexcept;

// Recompute the cached AABB of a cluster from its voxel_indices. Cheap
// (O(N) scan); called after a split.
void recompute_cluster_aabb(const VoxelGrid& grid, VoxelCluster& cluster) noexcept;

// Convert voxel-index-space centre to world-space coordinate.
[[nodiscard]] inline crd::math::Vec3<crd::f32>
voxel_centre_world(const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
                   const crd::math::Vec3<crd::f32>& voxel_size_world,
                   crd::u32 ix, crd::u32 iy, crd::u32 iz) noexcept
{
    return {
        grid_aabb.min.x + (static_cast<crd::f32>(ix) + 0.5F) * voxel_size_world.x,
        grid_aabb.min.y + (static_cast<crd::f32>(iy) + 0.5F) * voxel_size_world.y,
        grid_aabb.min.z + (static_cast<crd::f32>(iz) + 0.5F) * voxel_size_world.z,
    };
}

// Membership predicate using a flat per-voxel cluster_id sidecar (faster
// than scanning the index array). The caller allocates the sidecar once
// (size = grid.voxel_count()), initialises Surface/Inside voxels to a
// valid cluster id (root starts as 0), Outside/Unknown voxels to U32_MAX.
// Splits update the sidecar in-place.
constexpr crd::u32 kClusterIdNone = ~crd::u32{0};

// Collect surface voxels of the cluster as world-space centres for hull
// input. A voxel is "cluster surface" iff:
//   * its state is `Surface` in the source grid, OR
//   * at least one 6-neighbour is OUT of the cluster (= different cluster
//     id, or out of grid bounds).
[[nodiscard]] crd::containers::Array<crd::math::Vec3<crd::f32>>
collect_cluster_surface_centres(
    const VoxelGrid&                                  grid,
    const VoxelCluster&                               cluster,
    crd::containers::ConstSpan<crd::u32>              cluster_id_sidecar,
    crd::u32                                          this_cluster_id,
    const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
    const crd::math::Vec3<crd::f32>&                  voxel_size_world,
    crd::memory::IAllocator*                          alloc) noexcept;

// Build the convex hull of a cluster (Quickhull over its surface voxel
// centres). Returns a fully-owned `QuickhullResult<f32>`; check
// `result.empty()` for degenerate clusters that didn't yield a hull.
[[nodiscard]] crd::geometry::convex::QuickhullResult<crd::f32>
build_cluster_hull(const VoxelGrid&                                  grid,
                   const VoxelCluster&                               cluster,
                   crd::containers::ConstSpan<crd::u32>              cluster_id_sidecar,
                   crd::u32                                          this_cluster_id,
                   const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
                   const crd::math::Vec3<crd::f32>&                  voxel_size_world,
                   crd::memory::IAllocator*                          alloc) noexcept;

// Voxel-fraction concavity of a cluster (D129):
//     concavity = 1 - |C_voxels| / |hull_voxels(C)|
// where `hull_voxels(C)` counts voxels in the cluster's AABB whose
// centre is inside the cluster's convex hull. Clamped to [0, 1].
// A perfectly convex cluster (e.g. cube, sphere of voxels) has
// concavity = 0 within float epsilon.
//
// Returns the cluster's hull (so callers planning to KEEP the hull
// don't recompute it — important for the recursive driver). Empty hull
// (degenerate cluster) ⇒ concavity = 0 (treated as "no further split
// possible" rather than "fully concave" — safer termination).
struct ClusterConcavity
{
    crd::f32                                                  value = 0.0F;
    crd::geometry::convex::QuickhullResult<crd::f32>          hull;
    crd::u32                                                  hull_voxel_count = 0;
    explicit ClusterConcavity(crd::memory::IAllocator* alloc) noexcept : hull(alloc) {}
};

[[nodiscard]] ClusterConcavity
cluster_concavity(const VoxelGrid&                                  grid,
                  const VoxelCluster&                               cluster,
                  crd::containers::ConstSpan<crd::u32>              cluster_id_sidecar,
                  crd::u32                                          this_cluster_id,
                  const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
                  const crd::math::Vec3<crd::f32>&                  voxel_size_world,
                  crd::memory::IAllocator*                          alloc) noexcept;

// --- Plane-search + split --------------------------------------------------

enum class SplitAxis : crd::u8 { X = 0, Y = 1, Z = 2 };

// A candidate split plane: axis-aligned at voxel index `position` along
// `axis`. Voxels with index_component < position go LEFT; >= position go
// RIGHT. `position` is in voxel-index space (not world).
struct SplitPlane
{
    SplitAxis axis     = SplitAxis::X;
    crd::u32  position = 0;
};

// Evaluation of a candidate split plane. `valid` is false when the plane
// produces an empty side (e.g. position at cluster boundary).
struct SplitCandidate
{
    SplitPlane plane{};
    crd::f32   cost            = std::numeric_limits<crd::f32>::infinity();
    crd::u32   left_voxel_count  = 0;
    crd::u32   right_voxel_count = 0;
    bool       valid            = false;
};

// Search for the minimum-cost split plane for `cluster`. Tries
// `opts.splits_per_axis` evenly-spaced positions along each of the 3
// axes (3 · splits_per_axis candidates). Returns the best valid
// candidate; check `result.valid` before using.
//
// Cost function (D130 — Mamou §3.3 form):
//   cost = concavity(L) + concavity(R)
//        + alpha · |size_L - size_R| / (size_L + size_R)
//        + beta  · symmetry_penalty(plane)
//
// v9c-b ships with beta = 0 by default — true symmetry-axis detection
// adds principal-axis machinery; deferred to v9c-b-symmetry follow-on.
[[nodiscard]] SplitCandidate
find_best_split(const VoxelGrid&                                  grid,
                const VoxelCluster&                               cluster,
                crd::containers::ConstSpan<crd::u32>              cluster_id_sidecar,
                crd::u32                                          this_cluster_id,
                const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
                const crd::math::Vec3<crd::f32>&                  voxel_size_world,
                crd::u32                                          splits_per_axis,
                crd::f32                                          alpha_imbalance,
                crd::f32                                          beta_symmetry,
                crd::memory::IAllocator*                          alloc) noexcept;

// Apply a split plane: partition `cluster.voxel_indices` into a LEFT
// half (kept in `cluster`) and a RIGHT half (returned as a new cluster
// with id `new_cluster_id`). Updates the sidecar in place. Recomputes
// AABBs for both halves.
[[nodiscard]] VoxelCluster
apply_split(const VoxelGrid&                          grid,
            VoxelCluster&                             cluster,
            crd::u32                                  this_cluster_id,
            crd::u32                                  new_cluster_id,
            const SplitPlane&                         plane,
            crd::containers::Array<crd::u32>&         cluster_id_sidecar,
            crd::memory::IAllocator*                  alloc) noexcept;

} // namespace crd::geometry::decomposition::detail
