#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — jobs-parallel binned-SAH builder (v1f).
//
// `bvh_build_parallel` builds the same tree `bvh_build` does — bit-for-bit —
// but fans the two O(count) reductions per node (centroid bounds; the 3-axis
// bin histograms) out over `crd::jobs::parallel_reduce` for nodes with at least
// `parallel_threshold` primitives. The per-chunk partials are min/max + integer
// adds (exact, commutative) and `parallel_reduce` folds them in fixed job
// order, so the SAH evaluation is identical to the serial path regardless of
// `num_jobs` → the tree is identical. (The stable partition stays serial in
// v1f; the per-leaf node-array layout and the §5.2 SAH split tiebreak are
// unchanged.)
//
// Requires the job system to be initialised (`crd::jobs::init()`) — it calls
// `jobs::parallel_reduce`, which uses `jobs::frame_alloc`. If `num_jobs` (or
// the resolved worker count) is ≤ 1, or the input has fewer than
// `parallel_threshold` primitives, it just calls the serial `bvh_build`.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh/bvh_build.hpp>

namespace crd::geometry::bvh
{
// Build a BVH over `prims`, fanning the large-node reductions over `num_jobs`
// worker jobs (0 ⇒ use `crd::jobs::num_workers()`). Nodes with fewer than
// `parallel_threshold` primitives take the serial path. Produces a tree
// byte-for-byte equal to `bvh_build(prims, alloc, opts)`.
[[nodiscard]] BvhTree bvh_build_parallel(crd::containers::ConstSpan<AABB3<crd::f32>> prims,
                                         crd::memory::IAllocator* alloc, const BvhBuildOptions& opts = {},
                                         crd::u32 num_jobs = 0, crd::u32 parallel_threshold = 8192);

} // namespace crd::geometry::bvh
