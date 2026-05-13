#include "bvh_build_internal.hpp"

#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh_build_parallel.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::bvh
{
namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::math::Vec3;
using detail::accumulate_histogram;
using detail::BinHistogram;
using detail::bounds_of_range;
using detail::centroid_bounds_of;
using detail::clear_histogram;
using detail::k_max_bins;
using detail::merge_histogram;
using detail::SplitChoice;
using detail::stable_partition_by_bin;
using detail::sweep_for_split;

struct BuildFrame
{
    u32 node;
    u32 first;
    u32 count;
    u32 depth;
};

// `parallel_for(num_jobs, num_jobs, fn)` calls `fn(i, i+1)` for each i in
// [0, num_jobs) — so `begin` IS the chunk index. Each worker maps its chunk to
// the element sub-range [i·total/nj, (i+1)·total/nj) of the slice (the exact
// chunking the serial accumulate would walk), writes into its own `out[i]` slot
// (disjoint — no race). The fold is serial and in slot order; everything is
// min/max + integer adds (exact, commutative) → the result is bit-identical to
// the serial builder regardless of `num_jobs`. `out` is the calling thread's
// allocator (reused for every big node — never the frame arena), so a big build
// does not grow the frame arena.

struct CbParams
{
    const u32* idx;
    u32 first;
    u32 total;
    u32 num_jobs;
    const Vec3<f32>* cent;
    AABB3<f32>* out;
};

struct HpParams
{
    const u32* idx;
    u32 first;
    u32 total;
    u32 num_jobs;
    const Vec3<f32>* cent;
    const AABB3<f32>* prim;
    u32 bins;
    AABB3<f32> cbounds;
    BinHistogram* out;
};

[[nodiscard]] AABB3<f32> parallel_centroid_bounds(const u32* idx, u32 first, u32 total, u32 num_jobs,
                                                  const Vec3<f32>* cent, AABB3<f32>* partials)
{
    CbParams params{idx, first, total, num_jobs, cent, partials};
    const CbParams* pp = &params;
    crd::jobs::Counter* c = crd::jobs::parallel_for(num_jobs, num_jobs,
                                                    [pp](u32 i, u32)
                                                    {
                                                        const u32 b = i * pp->total / pp->num_jobs;
                                                        const u32 e = (i + 1U) * pp->total / pp->num_jobs;
                                                        pp->out[i] =
                                                            centroid_bounds_of(pp->idx, pp->first + b, e - b, pp->cent);
                                                    });
    crd::jobs::wait(c);
    AABB3<f32> result = detail::aabb_empty();
    for (u32 i = 0; i < num_jobs; ++i)
    {
        detail::aabb_merge(result, partials[i]);
    }
    return result;
}

void parallel_accumulate_histogram(const u32* idx, u32 first, u32 total, u32 num_jobs, const Vec3<f32>* cent,
                                   const AABB3<f32>* prim, u32 bins, const AABB3<f32>& cbounds, BinHistogram* partials,
                                   BinHistogram& result)
{
    HpParams params{idx, first, total, num_jobs, cent, prim, bins, cbounds, partials};
    const HpParams* pp = &params;
    crd::jobs::Counter* c = crd::jobs::parallel_for(num_jobs, num_jobs,
                                                    [pp](u32 i, u32)
                                                    {
                                                        const u32 b = i * pp->total / pp->num_jobs;
                                                        const u32 e = (i + 1U) * pp->total / pp->num_jobs;
                                                        clear_histogram(pp->out[i], pp->bins);
                                                        accumulate_histogram(pp->out[i], pp->idx, pp->first + b, e - b,
                                                                             pp->cent, pp->prim, pp->bins, pp->cbounds);
                                                    });
    crd::jobs::wait(c);
    clear_histogram(result, bins);
    for (u32 i = 0; i < num_jobs; ++i)
    {
        merge_histogram(result, partials[i], bins);
    }
}

} // namespace

BvhTree bvh_build_parallel(crd::containers::ConstSpan<AABB3<f32>> prims, crd::memory::IAllocator* alloc,
                           const BvhBuildOptions& opts, u32 num_jobs, u32 parallel_threshold)
{
    CRD_ASSERT(alloc != nullptr);
    CRD_ASSERT(crd::geometry::primitives::all_finite(prims)); // NaN/Inf contract — ADR-0076 §15
    if (num_jobs == 0U)
    {
        num_jobs = crd::jobs::num_workers();
    }
    // Nothing to fan out — the serial builder produces exactly the same tree.
    if (num_jobs <= 1U || prims.size() < parallel_threshold)
    {
        return bvh_build(prims, alloc, opts);
    }

    BvhTree tree(alloc);
    const usize n = prims.size();
    const u32 bins = opts.sah_bins < 2U ? 2U : (opts.sah_bins > k_max_bins ? k_max_bins : opts.sah_bins);
    const u32 max_leaf = opts.max_leaf_prims < 1U ? 1U : static_cast<u32>(opts.max_leaf_prims);

    crd::containers::Array<Vec3<f32>> centroids(alloc);
    centroids.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        centroids[i] = detail::aabb_centroid(prims[i]);
    }
    crd::containers::Array<u32>& idx = tree.prim_indices_mut();
    idx.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        idx[i] = static_cast<u32>(i);
    }
    crd::containers::Array<u32> partition_scratch(alloc);
    partition_scratch.resize(n);
    crd::containers::Array<BvhNode>& nodes = tree.nodes_mut();
    nodes.reserve(2 * n);

    // Per-job partial buffers, on the tree's allocator, reused for every big node.
    crd::containers::Array<AABB3<f32>> cb_partials(alloc);
    cb_partials.resize(num_jobs);
    crd::containers::Array<BinHistogram> hist_partials(alloc);
    hist_partials.resize(num_jobs);

    const AABB3<f32>* prim_ptr = prims.data();
    const Vec3<f32>* cent_ptr = centroids.data();
    u32* idx_ptr = idx.data();
    u32* scratch_ptr = partition_scratch.data();

    BvhNode root{};
    root.bounds = bounds_of_range(prim_ptr, idx_ptr, 0, static_cast<u32>(n));
    nodes.push_back(root);
    tree.set_root(0);

    crd::containers::Array<BuildFrame> stack(alloc);
    stack.reserve(2 * k_max_bvh_depth);
    stack.push_back(BuildFrame{0, 0, static_cast<u32>(n), 0});

    while (stack.size() > 0)
    {
        const BuildFrame frame = stack[stack.size() - 1];
        stack.resize(stack.size() - 1);

        if (frame.count <= max_leaf) // leaf
        {
            BvhNode& node = nodes[frame.node];
            node.left_first = frame.first;
            node.prim_count = static_cast<crd::u16>(frame.count);
            node.split_axis = 0;
            continue;
        }

        AABB3<f32> cbounds;
        BinHistogram hist;
        if (frame.count >= parallel_threshold) // fan the two O(count) reductions out
        {
            cbounds =
                parallel_centroid_bounds(idx_ptr, frame.first, frame.count, num_jobs, cent_ptr, cb_partials.data());
            parallel_accumulate_histogram(idx_ptr, frame.first, frame.count, num_jobs, cent_ptr, prim_ptr, bins,
                                          cbounds, hist_partials.data(), hist);
        }
        else // small node — serial, identical result
        {
            cbounds = centroid_bounds_of(idx_ptr, frame.first, frame.count, cent_ptr);
            clear_histogram(hist, bins);
            accumulate_histogram(hist, idx_ptr, frame.first, frame.count, cent_ptr, prim_ptr, bins, cbounds);
        }
        SplitChoice best;
        sweep_for_split(hist, bins, best);

        u32 mid = (best.axis < 0) ? (frame.count / 2U)
                                  : stable_partition_by_bin(idx_ptr, scratch_ptr, frame.first, frame.count, cent_ptr,
                                                            best.axis, best.bin, cbounds, bins);
        if (mid == 0 || mid >= frame.count)
        {
            mid = frame.count / 2U;
        }

        CRD_ASSERT(frame.depth + 1U < k_max_bvh_depth);
        const u32 left_idx = static_cast<u32>(nodes.size());
        BvhNode left_child{};
        BvhNode right_child{};
        left_child.bounds = bounds_of_range(prim_ptr, idx_ptr, frame.first, mid);
        right_child.bounds = bounds_of_range(prim_ptr, idx_ptr, frame.first + mid, frame.count - mid);
        nodes.push_back(left_child);
        nodes.push_back(right_child);
        CRD_ASSERT(nodes.size() <= 2U * n);

        BvhNode& parent = nodes[frame.node];
        parent.left_first = left_idx;
        parent.prim_count = 0;
        parent.split_axis = static_cast<crd::u8>(best.axis < 0 ? 0 : best.axis);

        stack.push_back(BuildFrame{left_idx + 1U, frame.first + mid, frame.count - mid, frame.depth + 1U});
        stack.push_back(BuildFrame{left_idx, frame.first, mid, frame.depth + 1U});
    }

    return tree;
}

} // namespace crd::geometry::bvh
