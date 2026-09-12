#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — in-place tree updates (v1b: refit; v1c: insert/erase).
//
//   * `bvh_refit(tree, prims)` — O(n) bottom-up AABB recomputation. The
//     topology (node array, prim-index permutation, `split_axis`) is left
//     untouched; only each node's `bounds` is rewritten from the current
//     `prims`. For dynamic scenes where the set of primitives is fixed but
//     their boxes move every frame: refit is far cheaper than a rebuild, and
//     query *correctness* is unaffected by how far things moved (only query
//     *efficiency* degrades — eventually a rebuild pays off; that's the
//     caller's call). Catto GDC 2019, *Dynamic Bounding Volume Hierarchies*.
//
//     The caller must pass `prims` with the same length as the build's input
//     (the leaf-order indices stay valid); changing the count is undefined —
//     rebuild instead.
//
//   * incremental insert / erase is a *different structure* (the static
//     `BvhTree` packed array can't support it without a rebuild) — see
//     `crd/geometry/bvh/dynamic_bvh.hpp` (`DynamicBvh`, v1c).
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh/bvh_tree.hpp>

namespace crd::geometry::bvh
{
// Recompute every node's bounds bottom-up from the current `prims`. No-op on an
// empty tree. Relies on the build invariant that a node's children have a higher
// array index than the node itself (so a single reverse pass is bottom-up) —
// `bvh_build` guarantees this; a debug assert checks it.
void bvh_refit(BvhTree& tree, crd::containers::ConstSpan<AABB3<crd::f32>> prims);

} // namespace crd::geometry::bvh
