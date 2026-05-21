#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ordering
{
// -----------------------------------------------------------------------
// Symbolic fill metrics (v2a minimal subset; full symbolic factorisation —
// L-pattern + supernodes — lands in v2c). Operate on the SYMMETRIC pattern of a
// square matrix (the structure of A is symmetrised internally; only the lower
// triangle drives Cholesky symbolic). `nnz_l` is the fill metric the AMD gate
// (v2b) and ND (v2e) are judged by.
// -----------------------------------------------------------------------

inline constexpr crd::u32 kNoParent = 0xFFFFFFFFU;

// Elimination tree of chol(A). parent[j] = parent column of j (kNoParent for a
// root). Liu's algorithm with path compression on the symmetrised pattern.
[[nodiscard]] crd::containers::Array<crd::u32> elimination_tree(const sparse::SparsePattern& pattern,
                                                                crd::memory::IAllocator* alloc);

// Column counts of the Cholesky factor L: colcount[j] = nnz of column j of L
// (including the diagonal). Sum == nnz(L). Skeleton + etree (Gilbert-Ng-Peyton).
[[nodiscard]] crd::containers::Array<crd::u32> column_counts(const sparse::SparsePattern& pattern,
                                                             crd::containers::ConstSpan<crd::u32> etree,
                                                             crd::memory::IAllocator* alloc);

// Total nonzeros in the Cholesky factor L for this pattern+ordering — THE fill
// metric. Convenience: builds the etree + column counts and sums them.
[[nodiscard]] crd::u64 nnz_l(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc);

// Matrix bandwidth: max over stored (i,j) of |i - j| (the RCM target metric).
[[nodiscard]] crd::u32 bandwidth(const sparse::SparsePattern& pattern) noexcept;

// Matrix profile (envelope size): sum over rows i of (i - min column in row i).
[[nodiscard]] crd::u64 profile(const sparse::SparsePattern& pattern) noexcept;

} // namespace crd::hesap::ordering
