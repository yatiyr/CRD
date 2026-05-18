// ---------------------------------------------------------------------------
// V-HACD internal helpers — implementation. Phase 3.1.7 v9c-b.
// ---------------------------------------------------------------------------

#include "vhacd_internal.hpp"

#include <crd/containers/span.hpp>

#include <algorithm>
#include <limits>

namespace crd::geometry::decomposition::detail
{

VoxelCluster build_root_cluster(const VoxelGrid& grid, crd::memory::IAllocator* alloc) noexcept
{
    VoxelCluster c(alloc);
    if (grid.is_empty())
    {
        return c;
    }
    crd::u32 min_ix = std::numeric_limits<crd::u32>::max();
    crd::u32 min_iy = std::numeric_limits<crd::u32>::max();
    crd::u32 min_iz = std::numeric_limits<crd::u32>::max();
    crd::u32 max_ix = 0U;
    crd::u32 max_iy = 0U;
    crd::u32 max_iz = 0U;
    bool any = false;

    for (crd::u32 iz = 0U; iz < grid.nz(); ++iz)
    {
        for (crd::u32 iy = 0U; iy < grid.ny(); ++iy)
        {
            for (crd::u32 ix = 0U; ix < grid.nx(); ++ix)
            {
                const VoxelState s = grid.state_at(ix, iy, iz);
                if (s == VoxelState::Surface || s == VoxelState::Inside)
                {
                    c.voxel_indices.push_back(encode_linear(grid, ix, iy, iz));
                    min_ix = std::min(min_ix, ix);
                    min_iy = std::min(min_iy, iy);
                    min_iz = std::min(min_iz, iz);
                    max_ix = std::max(max_ix, ix);
                    max_iy = std::max(max_iy, iy);
                    max_iz = std::max(max_iz, iz);
                    any = true;
                }
            }
        }
    }

    if (!any)
    {
        return c;
    }
    c.min_ix = min_ix;
    c.min_iy = min_iy;
    c.min_iz = min_iz;
    c.max_ix = max_ix + 1U; // exclusive
    c.max_iy = max_iy + 1U;
    c.max_iz = max_iz + 1U;
    return c;
}

void recompute_cluster_aabb(const VoxelGrid& grid, VoxelCluster& cluster) noexcept
{
    if (cluster.voxel_indices.empty())
    {
        cluster.min_ix = cluster.min_iy = cluster.min_iz = 0U;
        cluster.max_ix = cluster.max_iy = cluster.max_iz = 0U;
        return;
    }
    crd::u32 min_ix = std::numeric_limits<crd::u32>::max();
    crd::u32 min_iy = std::numeric_limits<crd::u32>::max();
    crd::u32 min_iz = std::numeric_limits<crd::u32>::max();
    crd::u32 max_ix = 0U;
    crd::u32 max_iy = 0U;
    crd::u32 max_iz = 0U;
    for (const crd::u32 linear : cluster.voxel_indices)
    {
        crd::u32 ix = 0U;
        crd::u32 iy = 0U;
        crd::u32 iz = 0U;
        decode_linear(grid, linear, ix, iy, iz);
        min_ix = std::min(min_ix, ix); max_ix = std::max(max_ix, ix);
        min_iy = std::min(min_iy, iy); max_iy = std::max(max_iy, iy);
        min_iz = std::min(min_iz, iz); max_iz = std::max(max_iz, iz);
    }
    cluster.min_ix = min_ix;
    cluster.min_iy = min_iy;
    cluster.min_iz = min_iz;
    cluster.max_ix = max_ix + 1U;
    cluster.max_iy = max_iy + 1U;
    cluster.max_iz = max_iz + 1U;
}

namespace
{

[[nodiscard]] bool is_in_this_cluster(crd::containers::ConstSpan<crd::u32> sidecar,
                                      crd::u32 linear,
                                      crd::u32 this_cluster_id) noexcept
{
    return linear < sidecar.size() && sidecar[linear] == this_cluster_id;
}

[[nodiscard]] bool any_neighbour_outside_cluster(const VoxelGrid& grid,
                                                 crd::containers::ConstSpan<crd::u32> sidecar,
                                                 crd::u32 this_cluster_id,
                                                 crd::u32 ix, crd::u32 iy, crd::u32 iz) noexcept
{
    // 6-connectivity. A voxel is "boundary" if any axis is at the grid edge
    // OR a neighbour exists but is not in this cluster.
    if (ix == 0U
        || !is_in_this_cluster(sidecar, encode_linear(grid, ix - 1U, iy, iz), this_cluster_id))
    {
        return true;
    }
    if (ix + 1U >= grid.nx()
        || !is_in_this_cluster(sidecar, encode_linear(grid, ix + 1U, iy, iz), this_cluster_id))
    {
        return true;
    }
    if (iy == 0U
        || !is_in_this_cluster(sidecar, encode_linear(grid, ix, iy - 1U, iz), this_cluster_id))
    {
        return true;
    }
    if (iy + 1U >= grid.ny()
        || !is_in_this_cluster(sidecar, encode_linear(grid, ix, iy + 1U, iz), this_cluster_id))
    {
        return true;
    }
    if (iz == 0U
        || !is_in_this_cluster(sidecar, encode_linear(grid, ix, iy, iz - 1U), this_cluster_id))
    {
        return true;
    }
    if (iz + 1U >= grid.nz()
        || !is_in_this_cluster(sidecar, encode_linear(grid, ix, iy, iz + 1U), this_cluster_id))
    {
        return true;
    }
    return false;
}

} // namespace

crd::containers::Array<crd::math::Vec3<crd::f32>>
collect_cluster_surface_centres(
    const VoxelGrid&                                  grid,
    const VoxelCluster&                               cluster,
    crd::containers::ConstSpan<crd::u32>              cluster_id_sidecar,
    crd::u32                                          this_cluster_id,
    const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
    const crd::math::Vec3<crd::f32>&                  voxel_size_world,
    crd::memory::IAllocator*                          alloc) noexcept
{
    crd::containers::Array<crd::math::Vec3<crd::f32>> centres(alloc);
    centres.reserve(cluster.voxel_indices.size());

    for (const crd::u32 linear : cluster.voxel_indices)
    {
        crd::u32 ix = 0U;
        crd::u32 iy = 0U;
        crd::u32 iz = 0U;
        decode_linear(grid, linear, ix, iy, iz);
        const VoxelState s = grid.state_at(ix, iy, iz);
        const bool grid_surface = (s == VoxelState::Surface);
        const bool cluster_boundary =
            any_neighbour_outside_cluster(grid, cluster_id_sidecar, this_cluster_id, ix, iy, iz);
        if (grid_surface || cluster_boundary)
        {
            centres.push_back(voxel_centre_world(grid_aabb, voxel_size_world, ix, iy, iz));
        }
    }
    return centres;
}

crd::geometry::convex::QuickhullResult<crd::f32>
build_cluster_hull(const VoxelGrid&                                  grid,
                   const VoxelCluster&                               cluster,
                   crd::containers::ConstSpan<crd::u32>              cluster_id_sidecar,
                   crd::u32                                          this_cluster_id,
                   const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
                   const crd::math::Vec3<crd::f32>&                  voxel_size_world,
                   crd::memory::IAllocator*                          alloc) noexcept
{
    const auto centres = collect_cluster_surface_centres(
        grid, cluster, cluster_id_sidecar, this_cluster_id, grid_aabb, voxel_size_world, alloc);
    crd::containers::ConstSpan<crd::math::Vec3<crd::f32>> span(centres.data(), centres.size());
    return crd::geometry::convex::quickhull<crd::f32>(span, alloc);
}

ClusterConcavity
cluster_concavity(const VoxelGrid&                                  grid,
                  const VoxelCluster&                               cluster,
                  crd::containers::ConstSpan<crd::u32>              cluster_id_sidecar,
                  crd::u32                                          this_cluster_id,
                  const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
                  const crd::math::Vec3<crd::f32>&                  voxel_size_world,
                  crd::memory::IAllocator*                          alloc) noexcept
{
    ClusterConcavity result(alloc);
    result.hull = build_cluster_hull(grid, cluster, cluster_id_sidecar,
                                       this_cluster_id, grid_aabb, voxel_size_world, alloc);

    // Degenerate hull (cluster too small / coplanar / colinear) ⇒
    // treat as zero concavity = "as convex as it can be". Stops the
    // recursive driver from trying to split unsplittable clusters.
    if (result.hull.empty() || result.hull.faces.empty())
    {
        result.value = 0.0F;
        result.hull_voxel_count = cluster.voxel_count();
        return result;
    }

    // Count voxels in cluster AABB whose centre is inside the hull.
    const auto hull_view = crd::geometry::convex::convex_hull_view_of(result.hull);
    crd::u32 hull_voxels = 0U;
    for (crd::u32 iz = cluster.min_iz; iz < cluster.max_iz; ++iz)
    {
        for (crd::u32 iy = cluster.min_iy; iy < cluster.max_iy; ++iy)
        {
            for (crd::u32 ix = cluster.min_ix; ix < cluster.max_ix; ++ix)
            {
                const auto centre = voxel_centre_world(grid_aabb, voxel_size_world, ix, iy, iz);
                if (crd::geometry::primitives::contains(hull_view, centre))
                {
                    ++hull_voxels;
                }
            }
        }
    }
    result.hull_voxel_count = hull_voxels;

    if (hull_voxels == 0U)
    {
        result.value = 0.0F;
        return result;
    }
    const crd::f32 ratio = static_cast<crd::f32>(cluster.voxel_count())
                          / static_cast<crd::f32>(hull_voxels);
    result.value = std::clamp(1.0F - ratio, 0.0F, 1.0F);
    return result;
}

// --- Plane-search + split --------------------------------------------------

namespace
{

// Coordinate component of a voxel along the given axis.
[[nodiscard]] crd::u32 axis_component(const VoxelGrid& grid, crd::u32 linear, SplitAxis axis) noexcept
{
    crd::u32 ix = 0U;
    crd::u32 iy = 0U;
    crd::u32 iz = 0U;
    decode_linear(grid, linear, ix, iy, iz);
    switch (axis)
    {
        case SplitAxis::X: return ix;
        case SplitAxis::Y: return iy;
        case SplitAxis::Z: return iz;
    }
    return 0U;
}

} // namespace

VoxelCluster
apply_split(const VoxelGrid&                          grid,
            VoxelCluster&                             cluster,
            crd::u32                                  this_cluster_id,
            crd::u32                                  new_cluster_id,
            const SplitPlane&                         plane,
            crd::containers::Array<crd::u32>&         cluster_id_sidecar,
            crd::memory::IAllocator*                  alloc) noexcept
{
    VoxelCluster right(alloc);
    right.voxel_indices.reserve(cluster.voxel_indices.size() / 2U);

    // Partition `cluster.voxel_indices` into left (kept) and right (moved).
    // Walk + swap-remove pattern; preserves the relative order of left
    // voxels which keeps tests deterministic.
    crd::containers::Array<crd::u32> kept_left(alloc);
    kept_left.reserve(cluster.voxel_indices.size());
    for (const crd::u32 linear : cluster.voxel_indices)
    {
        if (axis_component(grid, linear, plane.axis) < plane.position)
        {
            kept_left.push_back(linear);
        }
        else
        {
            right.voxel_indices.push_back(linear);
            cluster_id_sidecar[linear] = new_cluster_id;
        }
    }
    cluster.voxel_indices = std::move(kept_left);
    (void)this_cluster_id;

    recompute_cluster_aabb(grid, cluster);
    recompute_cluster_aabb(grid, right);
    return right;
}

namespace
{

// Concavity of a SUB-cluster defined inline (without yet committing the
// sidecar update). Used by find_best_split which needs to score both
// halves of a candidate split without mutating the grid state.
//
// Sub-cluster membership is derived implicitly: the parent cluster's
// voxels are split by (axis, position) into left/right; everything in
// `parent.voxel_indices` that satisfies the predicate joins the side.
// We avoid building two full VoxelCluster objects + side-mapping
// sidecars (which would dominate the cost of a candidate eval); we use
// a one-pass scan to compute hull + voxel count.
[[nodiscard]] crd::f32
concavity_of_subset(const VoxelGrid&                                  grid,
                    const VoxelCluster&                               parent,
                    crd::containers::ConstSpan<crd::u32>              parent_sidecar,
                    crd::u32                                          parent_id,
                    SplitAxis                                         axis,
                    crd::u32                                          position,
                    bool                                              left_side,
                    const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
                    const crd::math::Vec3<crd::f32>&                  voxel_size_world,
                    crd::memory::IAllocator*                          alloc,
                    crd::u32&                                         out_subset_voxel_count) noexcept
{
    // Build the subset's surface-voxel centres directly. A voxel is on
    // the subset's surface iff (a) the grid says it's Surface, OR
    // (b) any 6-neighbour is NOT in the subset (out-of-grid / different
    // cluster id / OTHER SIDE of the split plane).
    crd::containers::Array<crd::math::Vec3<crd::f32>> centres(alloc);
    centres.reserve(parent.voxel_indices.size() / 2U);

    crd::u32 subset_min_ix = std::numeric_limits<crd::u32>::max();
    crd::u32 subset_min_iy = std::numeric_limits<crd::u32>::max();
    crd::u32 subset_min_iz = std::numeric_limits<crd::u32>::max();
    crd::u32 subset_max_ix = 0U;
    crd::u32 subset_max_iy = 0U;
    crd::u32 subset_max_iz = 0U;
    crd::u32 subset_count  = 0U;

    auto in_subset = [&](crd::u32 linear) noexcept -> bool {
        if (linear >= parent_sidecar.size())            { return false; }
        if (parent_sidecar[linear] != parent_id)         { return false; }
        return left_side
            ? (axis_component(grid, linear, axis) <  position)
            : (axis_component(grid, linear, axis) >= position);
    };

    for (const crd::u32 linear : parent.voxel_indices)
    {
        if (!in_subset(linear)) { continue; }
        ++subset_count;

        crd::u32 ix = 0U;
        crd::u32 iy = 0U;
        crd::u32 iz = 0U;
        decode_linear(grid, linear, ix, iy, iz);
        subset_min_ix = std::min(subset_min_ix, ix); subset_max_ix = std::max(subset_max_ix, ix);
        subset_min_iy = std::min(subset_min_iy, iy); subset_max_iy = std::max(subset_max_iy, iy);
        subset_min_iz = std::min(subset_min_iz, iz); subset_max_iz = std::max(subset_max_iz, iz);

        const VoxelState s = grid.state_at(ix, iy, iz);
        bool surface = (s == VoxelState::Surface);
        if (!surface)
        {
            auto neighbour_outside = [&](crd::u32 nx, crd::u32 ny, crd::u32 nz) noexcept {
                return !in_subset(encode_linear(grid, nx, ny, nz));
            };
            if (ix == 0U || ix + 1U >= grid.nx()
                || iy == 0U || iy + 1U >= grid.ny()
                || iz == 0U || iz + 1U >= grid.nz()
                || neighbour_outside(ix - 1U, iy, iz)
                || neighbour_outside(ix + 1U, iy, iz)
                || neighbour_outside(ix, iy - 1U, iz)
                || neighbour_outside(ix, iy + 1U, iz)
                || neighbour_outside(ix, iy, iz - 1U)
                || neighbour_outside(ix, iy, iz + 1U))
            {
                surface = true;
            }
        }
        if (surface)
        {
            centres.push_back(voxel_centre_world(grid_aabb, voxel_size_world, ix, iy, iz));
        }
    }
    out_subset_voxel_count = subset_count;

    if (subset_count == 0U || centres.size() < 4U)
    {
        return 0.0F; // degenerate subset, no meaningful concavity
    }

    crd::containers::ConstSpan<crd::math::Vec3<crd::f32>> span(centres.data(), centres.size());
    auto hull = crd::geometry::convex::quickhull<crd::f32>(span, alloc);
    if (hull.empty() || hull.faces.empty())
    {
        return 0.0F;
    }
    const auto hull_view = crd::geometry::convex::convex_hull_view_of(hull);

    crd::u32 hull_voxels = 0U;
    for (crd::u32 iz = subset_min_iz; iz <= subset_max_iz; ++iz)
    {
        for (crd::u32 iy = subset_min_iy; iy <= subset_max_iy; ++iy)
        {
            for (crd::u32 ix = subset_min_ix; ix <= subset_max_ix; ++ix)
            {
                const auto centre = voxel_centre_world(grid_aabb, voxel_size_world, ix, iy, iz);
                if (crd::geometry::primitives::contains(hull_view, centre))
                {
                    ++hull_voxels;
                }
            }
        }
    }
    if (hull_voxels == 0U)
    {
        return 0.0F;
    }
    const crd::f32 ratio = static_cast<crd::f32>(subset_count) / static_cast<crd::f32>(hull_voxels);
    return std::clamp(1.0F - ratio, 0.0F, 1.0F);
}

} // namespace

SplitCandidate
find_best_split(const VoxelGrid&                                  grid,
                const VoxelCluster&                               cluster,
                crd::containers::ConstSpan<crd::u32>              cluster_id_sidecar,
                crd::u32                                          this_cluster_id,
                const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
                const crd::math::Vec3<crd::f32>&                  voxel_size_world,
                crd::u32                                          splits_per_axis,
                crd::f32                                          alpha_imbalance,
                crd::f32                                          beta_symmetry,
                crd::memory::IAllocator*                          alloc) noexcept
{
    (void)beta_symmetry; // v9c-b ships with beta_symmetry treated as 0; principal-axis follow-on planned
    SplitCandidate best{};
    if (cluster.voxel_count() < 2U || splits_per_axis == 0U)
    {
        return best;
    }

    const crd::u32 axis_min[3] = { cluster.min_ix, cluster.min_iy, cluster.min_iz };
    const crd::u32 axis_max[3] = { cluster.max_ix, cluster.max_iy, cluster.max_iz };

    for (crd::u32 a = 0U; a < 3U; ++a)
    {
        const crd::u32 lo = axis_min[a];
        const crd::u32 hi = axis_max[a]; // exclusive
        if (hi <= lo + 1U) { continue; } // axis too thin to split

        for (crd::u32 s = 1U; s <= splits_per_axis; ++s)
        {
            // Position = lo + s*(hi-lo)/(splits_per_axis+1). Skips the
            // endpoints (which would produce an empty side).
            const crd::u32 position = lo + (s * (hi - lo)) / (splits_per_axis + 1U);
            if (position <= lo || position >= hi) { continue; }

            SplitPlane plane{};
            plane.axis     = static_cast<SplitAxis>(a);
            plane.position = position;

            crd::u32 left_count = 0U;
            crd::u32 right_count = 0U;
            const crd::f32 conc_l = concavity_of_subset(
                grid, cluster, cluster_id_sidecar, this_cluster_id,
                plane.axis, plane.position, /*left_side*/ true,
                grid_aabb, voxel_size_world, alloc, left_count);
            const crd::f32 conc_r = concavity_of_subset(
                grid, cluster, cluster_id_sidecar, this_cluster_id,
                plane.axis, plane.position, /*left_side*/ false,
                grid_aabb, voxel_size_world, alloc, right_count);

            if (left_count == 0U || right_count == 0U) { continue; }

            const crd::f32 total = static_cast<crd::f32>(left_count + right_count);
            const crd::f32 imbalance =
                static_cast<crd::f32>(left_count > right_count
                                       ? (left_count - right_count)
                                       : (right_count - left_count)) / total;

            const crd::f32 cost = conc_l + conc_r + alpha_imbalance * imbalance;

            if (cost < best.cost)
            {
                best.plane             = plane;
                best.cost              = cost;
                best.left_voxel_count  = left_count;
                best.right_voxel_count = right_count;
                best.valid             = true;
            }
        }
    }

    return best;
}

} // namespace crd::geometry::decomposition::detail
