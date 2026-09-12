#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ordering
{
// -----------------------------------------------------------------------
// Permutation -- a symmetric reordering of a square matrix's rows+columns.
// Convention: perm[new_index] = original_index (the vertex placed at slot
// `new_index`); inv_perm[original_index] = new_index. Factoring PAPᵀ means
// new(i,j) = A(perm[i], perm[j]).
// -----------------------------------------------------------------------
struct Permutation
{
    crd::containers::Array<crd::u32> perm;      // length n; perm[i] = original vertex at new slot i
    crd::containers::Array<crd::u32> inv_perm;  // length n; inv_perm[perm[i]] = i

    explicit Permutation(crd::memory::IAllocator* alloc) : perm(alloc), inv_perm(alloc) {}

    [[nodiscard]] crd::u32 size() const noexcept { return static_cast<crd::u32>(perm.size()); }

    // Fill inv_perm from a populated perm (perm must be a valid permutation of [0,n)).
    void rebuild_inverse();
};

// Apply PAPᵀ to a pattern's STRUCTURE: returns the permuted CSR pattern (rows +
// columns reordered by `p`). Canonical (each output row's columns ascending).
// (Values-carrying apply deferred to v5, when the direct solver permutes a real
// system — no v2 consumer needs it.)
[[nodiscard]] sparse::SparsePattern apply_symmetric(const sparse::SparsePattern& pattern, const Permutation& p,
                                                    crd::memory::IAllocator* alloc);

} // namespace crd::hesap::ordering
