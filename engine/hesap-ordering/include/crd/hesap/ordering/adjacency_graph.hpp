#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ordering
{
// -----------------------------------------------------------------------
// AdjacencyGraph -- the undirected graph an ordering operates on (METIS-style
// CSR adjacency). Built from a SparsePattern by symmetrising (A ∪ Aᵀ), dropping
// the diagonal (self-loops carry no ordering information), and sorting each
// vertex's neighbour list ascending (D(ord)-4: makes every downstream graph
// algorithm input-adjacency-order-independent → deterministic).
//
// `xadj` (length n+1) indexes into `adjncy` (the concatenated neighbour lists),
// exactly like a CSR `outer_ptr`/`inner_idx` pair but structurally symmetric.
// -----------------------------------------------------------------------
struct AdjacencyGraph
{
    crd::u32                         n = 0;  // vertices (== pattern rows; square assumed)
    crd::containers::Array<crd::u32> xadj;    // length n+1
    crd::containers::Array<crd::u32> adjncy;  // length xadj[n]; neighbours, ascending per vertex

    explicit AdjacencyGraph(crd::memory::IAllocator* alloc) : xadj(alloc), adjncy(alloc) {}

    [[nodiscard]] crd::u32 degree(crd::u32 v) const noexcept { return xadj[v + 1] - xadj[v]; }
    [[nodiscard]] crd::u32 num_edges() const noexcept
    {
        return xadj.empty() ? 0U : (xadj[n] / 2U);  // each undirected edge stored twice
    }
};

// Build the symmetrised, diagonal-free, ascending-sorted adjacency from a
// COMPRESSED CSR pattern. Requires a square pattern (rows == cols).
[[nodiscard]] AdjacencyGraph build_adjacency(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc);

} // namespace crd::hesap::ordering
