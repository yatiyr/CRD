#pragma once

#include <crd/hesap/ordering/adjacency_graph.hpp>
#include <crd/hesap/ordering/permutation.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ordering
{
// -----------------------------------------------------------------------
// Reverse Cuthill-McKee — bandwidth/profile-reducing ordering.
//
// Cuthill-McKee: BFS from a pseudo-peripheral start node; within each BFS
// level, visit newly-reached neighbours in ascending (degree, vertex-index)
// order (D(ord)-1 tie-break). Reverse the resulting order → RCM (Reverse CM
// reduces the profile/envelope further than plain CM for the same bandwidth).
//
// Disconnected graphs: components are processed in ascending lowest-unvisited-
// vertex order, each seeded by its own pseudo-peripheral node. Determinism:
// pure integer, fixed tie-breaks (D(ord)-1), structure-derived start seed
// (D(ord)-3) — bit-identical permutation across runs/platforms.
// -----------------------------------------------------------------------
[[nodiscard]] Permutation rcm_order(const AdjacencyGraph& graph, crd::memory::IAllocator* alloc);

// Convenience: build adjacency from a pattern then RCM.
[[nodiscard]] Permutation rcm_order(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc);

} // namespace crd::hesap::ordering
