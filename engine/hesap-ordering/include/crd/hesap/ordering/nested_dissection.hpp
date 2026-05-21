#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/ordering/adjacency_graph.hpp>
#include <crd/hesap/ordering/permutation.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ordering
{
// -----------------------------------------------------------------------
// WeightedGraph -- one level of the multilevel coarsening stack. Same CSR
// adjacency shape as AdjacencyGraph (xadj/adjncy, symmetric, ascending-sorted,
// diagonal-free) PLUS edge weights (adjwgt, parallel to adjncy) and vertex
// weights (vwgt). The base level is all-unit; each coarsening level accumulates
// the weights of the fine vertices/edges it merges, so the total vertex weight
// is CONSERVED (Σ vwgt == base n) at every level and edge-cut / balance stay
// meaningful as the graph shrinks. Move-only (owns its Arrays).
// -----------------------------------------------------------------------
struct WeightedGraph
{
    crd::u32 n = 0;
    crd::containers::Array<crd::u32> xadj;   // length n+1
    crd::containers::Array<crd::u32> adjncy; // length xadj[n]; neighbours ascending per vertex
    crd::containers::Array<crd::u32> adjwgt; // length xadj[n]; edge weights, parallel to adjncy
    crd::containers::Array<crd::u32> vwgt;   // length n; vertex weights

    explicit WeightedGraph(crd::memory::IAllocator* alloc) : xadj(alloc), adjncy(alloc), adjwgt(alloc), vwgt(alloc) {}

    [[nodiscard]] crd::u32 degree(crd::u32 v) const noexcept { return xadj[v + 1] - xadj[v]; }
    [[nodiscard]] crd::u64 total_vertex_weight() const noexcept;
};

// Multilevel-ND 2-way partition of the symmetric pattern of a square matrix:
// part[v] ∈ {0, 1}. Pipeline: coarsen (heavy-edge matching) → bisect the coarsest
// graph (re-seeding BFS region-grow) → uncoarsen-project back to the original.
//
// Deterministic (D(ord)-1/-2/-3/-4/-7). FM-refined since v2e (single path — the
// public bipartition IS the refined multilevel one). Contracts: n == 0 → empty;
// n == 1 → {0}. Disconnected graphs handled by the bisection's re-seeding (D(ord)-7).
[[nodiscard]] crd::containers::Array<crd::u8> nd_bipartition(const sparse::SparsePattern& pattern,
                                                             crd::memory::IAllocator* alloc);

// Nested-dissection fill-reducing ordering. Recursive bisection: bipartition (FM-
// refined multilevel) → minimum vertex separator (König) → recurse on the two
// halves → elimination order = [order(A), order(B), separator] (separator LAST,
// so it is eliminated after both halves → minimal fill). Subgraphs of <=
// kAmdThreshold vertices switch to AMD (the METIS hybrid). The returned
// Permutation's `perm[k]` is the k-th eliminated original vertex (same convention
// as `amd_order`). Deterministic (D(ord)-1/-2/-3/-4/-7).
[[nodiscard]] Permutation nd_order(const AdjacencyGraph& graph, crd::memory::IAllocator* alloc);
[[nodiscard]] Permutation nd_order(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc);
} // namespace crd::hesap::ordering

// -----------------------------------------------------------------------
// Multilevel nested-dissection MACHINERY (METIS paradigm; pure-C++ port, not a
// wrap of METIS the library). v2d ships the SCAFFOLD — coarsen + bisect-coarsest
// + uncoarsen-project — proven correct by tests against conservation /
// well-formedness / determinism invariants. v2e layers Fiduccia-Mattheyses
// refinement + vertex-separator extraction + the recursive-bisection nd_order
// driver + the AMD-fill gate on top.
//
// Determinism (integer, no RNG): D(ord)-1 (matching/BFS ties → ascending index),
// D(ord)-2 (coarse adjacency built sorted), D(ord)-3 (lowest-index seed), D(ord)-4
// (adjacency re-sorted ascending), D(ord)-7 (coarse vertices numbered by ascending
// lowest-index constituent fine vertex; bisection re-seeds from the lowest-index
// unassigned vertex). See docs/systems/hesap-ordering.md.
// -----------------------------------------------------------------------
namespace crd::hesap::ordering::detail
{
inline constexpr crd::u32 kCoarsestMax = 100U;  // coarsen until ≤ this — bisect mediocrity is fine here
inline constexpr crd::u32 kMaxLevels = 30U;     // hard cap vs a pathological non-coarsening graph
inline constexpr crd::u32 kAmdThreshold = 100U; // recurse with ND above this; AMD-order at/below (METIS hybrid)

// Minimum vertex separator of a bipartition: the minimum vertex cover of the cut
// edges (König's theorem via bipartite max-matching), returned as ascending
// original vertex indices. Removing it disconnects part 0 from part 1. Optimal
// for the given cut + deterministic (ascending iteration throughout).
[[nodiscard]] crd::containers::Array<crd::u32>
vertex_separator(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u8> part, crd::memory::IAllocator* alloc);

// Constrained AMD (CHOLMOD-style): approximate-minimum-degree elimination on the
// FULL graph, constrained so vertices are eliminated in ascending `cmember` class
// order (within a class, by approximate minimum degree). This is the ND interface
// fix — separator vertices (assigned higher cmember by the separator-tree
// postorder) are provably eliminated after the interior they border, so the
// min-degree within each class sees the full graph and never dumps fill into a
// live separator. A constraint-aware copy of the cs_amd port: dense-node handling
// disabled; supervariable merge + mass elimination gated by cmember equality.
// `perm[k]` = the k-th eliminated vertex (same convention as `amd_order`).
[[nodiscard]] Permutation camd_order(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u32> cmember,
                                     crd::memory::IAllocator* alloc);

// Node-separator refinement, in place on a 3-way labelling loc[v] in
// {0=A, 1=B, 2=separator}. Repeatedly absorbs every separator vertex that has no
// neighbour on one side into that side -- a free |S| reduction with no cascade
// (the separator invariant is preserved because no A-B edge can appear), multi-
// pass until quiescent, balance-constrained (kBalanceTol on |A| vs |B|). König
// gives the minimum cover of the *edge* cut; this shrinks it toward the minimum
// vertex separator, which is what drives Cholesky fill. Ascending visit (D(ord)-1).
void node_fm_refine(const AdjacencyGraph& g, crd::containers::Array<crd::u8>& loc, crd::memory::IAllocator* alloc);

// Compact induced subgraph over `verts` (ascending original ids): a new
// AdjacencyGraph of verts.size() vertices keeping only edges between kept
// vertices, neighbours remapped to new ids (ascending preserved). The new->global
// label map is `verts` itself.
[[nodiscard]] AdjacencyGraph induced_subgraph(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u32> verts,
                                              crd::memory::IAllocator* alloc);

// Base WeightedGraph from an unweighted adjacency: all edge + vertex weights 1.
[[nodiscard]] WeightedGraph to_weighted(const AdjacencyGraph& g, crd::memory::IAllocator* alloc);

// Heavy-edge matching. Visits vertices ascending; matches each still-unmatched v
// to the unmatched neighbour on its heaviest incident edge (ties → lowest
// neighbour index, D(ord)-1); unmatchable vertices stay singletons. Fills
// `cmap[fine] = coarse id` (a matched pair shares one id; coarse ids are numbered
// by ascending lowest fine member, D(ord)-7). Returns the coarse vertex count.
crd::u32 coarsen_match(const WeightedGraph& g, crd::containers::Array<crd::u32>& cmap, crd::memory::IAllocator* alloc);

// Contract `g` by `cmap` into a coarser WeightedGraph of `n_coarse` vertices:
// merge parallel edges summing adjwgt, sum vwgt, drop self-loops, sort each
// coarse vertex's neighbours ascending (D(ord)-2/-4).
[[nodiscard]] WeightedGraph contract(const WeightedGraph& g, crd::containers::ConstSpan<crd::u32> cmap,
                                     crd::u32 n_coarse, crd::memory::IAllocator* alloc);

// Coarsen `base` (moved in as level 0) until n ≤ kCoarsestMax OR the matching
// stalls (n_coarse ≥ 0.9·n, no useful contraction) OR kMaxLevels is reached.
// `levels`[0] = base … back() = coarsest; `cmaps`[i] maps level i → level i+1
// (so cmaps.size() == levels.size() - 1).
void coarsen(WeightedGraph base, crd::containers::Array<WeightedGraph>& levels,
             crd::containers::Array<crd::containers::Array<crd::u32>>& cmaps, crd::memory::IAllocator* alloc);

inline constexpr double kBalanceTol = 1.03; // METIS default — heavier side <= 1.03 * (total/2)
inline constexpr crd::u32 kFmPasses = 4U;   // METIS default refinement passes per level

// Fiduccia-Mattheyses bipartition refinement, in place. Gain buckets (O(1) max
// extraction), best-prefix rollback, <= kFmPasses passes; equal-gain ties resolve
// to the lowest vertex index (D(ord)-1); keeps the heavier side within kBalanceTol
// of half the total vertex weight. Returns the final weighted edge-cut. Never
// increases the cut (a pass that finds no improving prefix is a no-op).
crd::u64 fm_refine(const WeightedGraph& g, crd::containers::Array<crd::u8>& part, crd::memory::IAllocator* alloc);

// The REFINED multilevel bipartition: coarsen -> bisect coarsest -> FM-refine
// after each uncoarsening projection (refine at every level). `nd_bipartition`
// and the recursive `nd_order` both build on this. Takes `base` by value (moved
// in as coarsening level 0).
[[nodiscard]] crd::containers::Array<crd::u8> bipartition_refined(WeightedGraph base, crd::memory::IAllocator* alloc);

// Re-seeding BFS region-grow bipartition of `g`. Grows part 0 from the lowest-
// index seed (D(ord)-3); when the frontier empties before part-0 weight reaches
// half the total, re-seeds from the lowest-index unassigned vertex (D(ord)-7 —
// the disconnected-graph contract); the remainder is part 1. Result ∈ {0,1},
// part 0 always non-empty (and part 1 non-empty for n ≥ 2).
[[nodiscard]] crd::containers::Array<crd::u8> bisect_coarsest(const WeightedGraph& g, crd::memory::IAllocator* alloc);

// Project a coarse partition to the finer level: fine v inherits part_coarse[cmap[v]].
[[nodiscard]] crd::containers::Array<crd::u8> project_down(crd::containers::ConstSpan<crd::u8> part_coarse,
                                                           crd::containers::ConstSpan<crd::u32> cmap,
                                                           crd::memory::IAllocator* alloc);

// Weighted edge-cut: Σ adjwgt over edges crossing the partition (each undirected
// edge counted once). The partition-quality metric Fiduccia-Mattheyses minimises in v2e.
[[nodiscard]] crd::u64 edge_cut(const WeightedGraph& g, crd::containers::ConstSpan<crd::u8> part) noexcept;
} // namespace crd::hesap::ordering::detail
