// ---------------------------------------------------------------------------
// V-HACD recursive decomposition — driver. Phase 3.1.7 v9c-b.
//
// Algorithm (Mamou §3.2-3.4): build root cluster from grid; until every
// cluster has concavity ≤ min_concavity or max_parts is hit, pick the
// worst (highest-concavity) cluster and split it along the best axis-
// aligned plane (minimum-cost per Mamou).
// ---------------------------------------------------------------------------

#include <crd/geometry/decomposition/vhacd.hpp>

#include "vhacd_internal.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>

#include <algorithm>
#include <limits>

namespace crd::geometry::decomposition
{

namespace
{

[[nodiscard]] VhacdStatus
validate_options(const VhacdOptions& opts) noexcept
{
    if (opts.max_parts == 0U)               { return VhacdStatus::InvalidOptions; }
    if (opts.max_depth == 0U)               { return VhacdStatus::InvalidOptions; }
    if (opts.splits_per_axis == 0U)         { return VhacdStatus::InvalidOptions; }
    if (!(opts.min_concavity >= 0.0F)
        || !(opts.min_concavity <= 1.0F))   { return VhacdStatus::InvalidOptions; }
    if (!(opts.alpha_imbalance >= 0.0F))    { return VhacdStatus::InvalidOptions; }
    if (!(opts.beta_symmetry   >= 0.0F))    { return VhacdStatus::InvalidOptions; }
    return VhacdStatus::Ok;
}

} // namespace

VhacdResult
vhacd_decompose(const VoxelGrid&                                  grid,
                const crd::geometry::primitives::AABB3<crd::f32>& grid_aabb,
                const crd::math::Vec3<crd::f32>&                  voxel_size_world,
                const VhacdOptions&                               opts,
                crd::memory::IAllocator*                          alloc) noexcept
{
    VhacdResult result(alloc);
    result.status = validate_options(opts);
    if (result.status != VhacdStatus::Ok)
    {
        return result;
    }
    if (grid.is_empty())
    {
        result.status = VhacdStatus::EmptyGrid;
        return result;
    }

    // Build the root cluster: every Surface ∪ Inside voxel.
    detail::VoxelCluster root = detail::build_root_cluster(grid, alloc);
    if (root.voxel_count() == 0U)
    {
        result.status = VhacdStatus::EmptyGrid;
        return result;
    }
    result.total_input_voxels = root.voxel_count();

    // Per-voxel cluster_id sidecar. kClusterIdNone for non-Surface/Inside cells.
    crd::containers::Array<crd::u32> sidecar(alloc);
    sidecar.resize(static_cast<crd::usize>(grid.voxel_count()), detail::kClusterIdNone);
    for (const crd::u32 linear : root.voxel_indices)
    {
        sidecar[linear] = 0U;
    }

    // Cluster pool — index = cluster id. Move-only; we never iterate over
    // pointers to elements during push_back so re-allocations are safe.
    crd::containers::Array<detail::VoxelCluster> clusters(alloc);
    clusters.reserve(opts.max_parts);
    clusters.push_back(std::move(root));

    // ---- Recursive plane-search loop ----
    // Per iteration: find worst-concavity cluster + find best split plane
    // for it + apply the split. Halts when every cluster has concavity
    // <= min_concavity OR num_parts == max_parts OR no usable split found.
    //
    // Concavity is computed fresh each iteration (correct + simple). A
    // cache-and-only-recompute-children optimisation is a v9c-b-perf
    // follow-on once a real consumer hits the perf wall.
    for (crd::u32 iter = 0U; iter < opts.max_parts; ++iter)
    {
        if (clusters.size() >= opts.max_parts) { break; }

        crd::containers::ConstSpan<crd::u32> sidecar_span(sidecar.data(), sidecar.size());

        // Pick worst cluster.
        crd::u32 worst_id    = 0U;
        crd::f32 worst_value = -1.0F;
        for (crd::u32 i = 0U; i < static_cast<crd::u32>(clusters.size()); ++i)
        {
            const auto cc = detail::cluster_concavity(grid, clusters[i], sidecar_span, i,
                                                       grid_aabb, voxel_size_world, alloc);
            if (cc.value > worst_value)
            {
                worst_value = cc.value;
                worst_id    = i;
            }
        }
        result.max_part_concavity = worst_value;

        // Termination: every cluster is "convex enough".
        if (worst_value <= opts.min_concavity) { break; }

        // Search the best split plane for the worst cluster.
        const auto split = detail::find_best_split(
            grid, clusters[worst_id], sidecar_span, worst_id,
            grid_aabb, voxel_size_world,
            opts.splits_per_axis, opts.alpha_imbalance, opts.beta_symmetry, alloc);
        if (!split.valid)
        {
            // No usable split (degenerate cluster). Accept this cluster
            // as a leaf even though it's concave — better than infinite-
            // looping.
            break;
        }

        // Apply the split. RIGHT half gets a new cluster id; LEFT half
        // keeps `worst_id`.
        const crd::u32 new_id = static_cast<crd::u32>(clusters.size());
        detail::VoxelCluster right = detail::apply_split(
            grid, clusters[worst_id], worst_id, new_id, split.plane, sidecar, alloc);
        clusters.push_back(std::move(right));
    }

    // Build hulls for the final clusters.
    crd::containers::ConstSpan<crd::u32> sidecar_span(sidecar.data(), sidecar.size());
    result.parts.reserve(clusters.size());
    for (crd::u32 i = 0U; i < static_cast<crd::u32>(clusters.size()); ++i)
    {
        auto hull = detail::build_cluster_hull(grid, clusters[i], sidecar_span, i,
                                                grid_aabb, voxel_size_world, alloc);
        if (hull.empty())
        {
            result.status = VhacdStatus::HullBuildFailed;
            return result;
        }
        result.parts.push_back(std::move(hull));
    }

    result.status = VhacdStatus::Ok;
    return result;
}

} // namespace crd::geometry::decomposition
