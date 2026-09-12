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
// AMD fill-reducing ordering. Eliminates min-(approximate-)degree variables on
// the quotient graph; the elimination order IS the returned permutation
// (perm[k] = the k-th eliminated variable). Deterministic (D(ord)-1/-5/-6).
// v2b-2 rung 2 ships EXACT min-degree (sanity); rung 3 swaps in the Amestoy
// approximate-degree bound + supervariables + mass elimination + aggressive
// absorption to reach the Eigen-AMD fill gate.
[[nodiscard]] Permutation amd_order(const AdjacencyGraph& graph, crd::memory::IAllocator* alloc);
[[nodiscard]] Permutation amd_order(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc);
} // namespace crd::hesap::ordering

// -----------------------------------------------------------------------
// AMD (Approximate Minimum Degree, Amestoy/Davis/Duff 1996) — the workhorse
// fill-reducing ordering (what SuiteSparse / Eigen / MATLAB use by default).
//
// v2b-1 (this slice): the quotient-graph (George-Liu) elimination MACHINERY,
// validated in isolation. `quotient_fill` eliminates variables in a GIVEN order
// and returns the resulting nnz(L) — which must match the symbolic-Cholesky
// nnz_l (v2a, cs_counts) for the same ordering. This proves the quotient graph
// represents fill correctly before v2b-2 layers the adaptive min-degree
// selection + approximate degree + supervariables + mass elimination +
// aggressive absorption on top (and ships `amd_order`).
//
// Determinism: integer, fixed tie-breaks (D(ord)-1, -5, -6). See
// docs/systems/hesap-ordering.md.
// -----------------------------------------------------------------------

namespace crd::hesap::ordering::detail
{
// nnz(L) of chol(PAPᵀ) where P eliminates variables in `elim_order` (a
// permutation of [0,n)), computed by quotient-graph elimination. The
// correctness oracle for the AMD machinery: equals nnz_l(apply_symmetric(pattern,
// P)) computed independently via cs_counts.
[[nodiscard]] crd::u64 quotient_fill(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u32> elim_order,
                                     crd::memory::IAllocator* alloc);

// v2b-2 RUNG 1: the same elimination as quotient_fill but on the PACKED
// workspace (Pe/Len/Elen/Iw flat arrays) that AMD proper will select on. Same
// algorithm, different storage → must equal quotient_fill bit-for-bit. Proves
// the packed representation before rung 2 (exact min-degree selection) and rung
// 3 (Amestoy approximate degree + supervariables + mass elim + absorption + the
// Eigen-AMD gate) build on it. GC disabled (Iw grows; dead space bounded by
// nnz(L) — reclaim lands with the rung-3 perf workspace).
[[nodiscard]] crd::u64 packed_fill(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u32> elim_order,
                                   crd::memory::IAllocator* alloc);
} // namespace crd::hesap::ordering::detail
