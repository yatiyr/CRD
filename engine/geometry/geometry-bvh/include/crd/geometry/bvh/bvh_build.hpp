#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — binned-SAH builder (v1a).
//
// `bvh_build` takes a span of primitive AABBs + an allocator and returns a
// `BvhTree` (functional form per ADR-0076 §11 — not a `BvhTree::build` member).
// The builder is the Wald 2007 "binned SAH": for each node, bucket primitive
// centroids into `sah_bins` bins per axis, sweep the bin boundaries to find the
// minimum-cost split, recurse.
//
// Determinism (ADR-0076 §5.2 — pinned): axes are evaluated X → Y → Z, and
// within an axis the lower bin index wins a cost tie (the implementation only
// replaces the running best on a *strictly* lower cost, so the first split
// found at the minimum cost is kept — which is exactly X-then-Y-then-Z,
// lower-bin-first). The leaf-order permutation is produced by a stable
// partition into scratch (no `std::sort`, no thread-order-dependent reduction —
// v1a's build is single-threaded; the jobs-parallel build with a deterministic
// per-thread reduction tree is a later `-bvh` slice).
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh/bvh_tree.hpp>

namespace crd::geometry::bvh
{
struct BvhBuildOptions
{
    crd::u32 sah_bins{16};      // bin count for the SAH sweep; clamped to [2, 64]
    crd::u16 max_leaf_prims{4}; // a node with ≤ this many prims becomes a leaf without splitting
};

// Build a BVH over `prims`. An empty span yields an empty tree
// (`tree.is_empty()`). The returned tree references primitives by index into
// `prims`; the caller keeps `prims` alive and passes it back to the query
// helpers.
[[nodiscard]] BvhTree bvh_build(crd::containers::ConstSpan<AABB3<crd::f32>> prims, crd::memory::IAllocator* alloc,
                                const BvhBuildOptions& opts = {});

// Surface-area-heuristic quality metric for a built tree: Σ over leaves of
// (prim_count · halfArea(leaf.bounds)), divided by halfArea(root.bounds). Lower
// is better; ~`prim_count` for a single-leaf tree, much less for a good split.
// (`0` for an empty tree.) Used by the v1a tests to assert the binned-SAH build
// is not catastrophically worse than a naive median split.
[[nodiscard]] crd::f32 bvh_sah_cost(const BvhTree& tree) noexcept;

} // namespace crd::geometry::bvh
