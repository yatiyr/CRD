#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_format.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// SparsePattern -- the structural skeleton of a sparse matrix, values-free.
// First leg of the pattern / values / analysis trinity (sparsematrices.md
// "the heart"): the symbolic structure is decoupled from the numeric values
// so an analysis / ordering / preprocess plan can be cached across many
// value frames as long as the structure is unchanged.
//
// Two storage modes (Eigen 4-array model):
//   * Compressed (inner_nnz EMPTY): `outer_ptr` (length n_outer+1) gives both
//     the start AND the end of each inner vector; `inner_idx` is tight.
//   * Uncompressed (inner_nnz present, length n_outer): `outer_ptr[k]` is the
//     start and `outer_ptr[k]+inner_nnz[k]` the end of the USED region of
//     inner vector k; `outer_ptr[k+1]` is its capacity end (the gap between
//     is insert slack). This is what SparseMatrix::coeff_ref / incremental
//     insert build before make_compressed().
//
//   CSR:  inner vector k = row k     -> stored column indices
//   CSC:  inner vector k = column k  -> stored row indices
// `n_outer` = outer_ptr.size()-1 = rows (CSR) / cols (CSC).
//
// Canonical invariant: within each inner vector the USED indices are sorted
// ascending and duplicate-free. compress() and coeff_ref both uphold it;
// topology_hash assumes it.
//
// Move-only (owns its Arrays). `topology_hash` is a CACHED field: after any
// structural mutation, the owner must call `recompute_topology_hash()`.
// The hash is the analysis-cache key -- see AnalysisHandle::is_valid_for.
// -----------------------------------------------------------------------

struct SparsePattern
{
    crd::u32                         rows = 0;
    crd::u32                         cols = 0;
    SparseFormat                     format = SparseFormat::Csr;
    crd::u16                         block_size = 1;  // 1 except for BSR (v1f)
    crd::containers::Array<crd::u32> outer_ptr;       // length n_outer+1
    crd::containers::Array<crd::u32> inner_idx;       // indices (used regions canonical-sorted)
    crd::containers::Array<crd::u32> inner_nnz;       // EMPTY = compressed; else used-count per inner vector
    crd::u64                         topology_hash = 0;  // cached; recompute after mutation

    explicit SparsePattern(crd::memory::IAllocator* alloc)
        : outer_ptr(alloc), inner_idx(alloc), inner_nnz(alloc)
    {
    }

    // True when stored tightly (no insert slack). Empty inner_nnz == compressed.
    [[nodiscard]] bool is_compressed() const noexcept { return inner_nnz.empty(); }

    // Number of inner vectors (rows for CSR / cols for CSC).
    [[nodiscard]] crd::u32 n_outer() const noexcept
    {
        return outer_ptr.empty() ? 0U : static_cast<crd::u32>(outer_ptr.size() - 1);
    }

    // Used non-zeros in inner vector k.
    [[nodiscard]] crd::u32 inner_count(crd::u32 k) const noexcept
    {
        return is_compressed() ? (outer_ptr[k + 1] - outer_ptr[k]) : inner_nnz[k];
    }

    // Total stored entries (scalar nnz; for BSR this counts blocks).
    [[nodiscard]] crd::usize nnz() const noexcept
    {
        if (is_compressed())
        {
            return inner_idx.size();
        }
        crd::usize total = 0;
        const crd::u32 n = n_outer();
        for (crd::u32 k = 0; k < n; ++k)
        {
            total += inner_nnz[k];
        }
        return total;
    }

    // Recompute and cache `topology_hash` over the current structure.
    void recompute_topology_hash() noexcept;
};

// -----------------------------------------------------------------------
// topology_hash -- deterministic, cross-platform-reproducible FNV-1a-64 over
// the CANONICAL logical structure. Pinned algorithm (v1a decision D1,
// refined in v1a-2 for slack invariance):
//
//   * Mixes rows, cols, format, block_size, then n_outer, then for each
//     inner vector: its USED count followed by its USED, canonical-sorted
//     indices. Reading the *logical* used region (not the physical slack)
//     makes the hash identical for a compressed matrix and its uncompressed
//     equivalent -- a slack-padded layout is not a different matrix.
//   * Multi-byte integers are fed little-endian via EXPLICIT byte shifts,
//     never memcpy, so the value is identical on big-endian hosts.
//   * Includes `format` + `block_size`: a CSR and a CSC pattern with
//     identical index arrays describe DIFFERENT matrices and must hash
//     differently (the analysis-cache correctness invariant).
//
// Used as the AnalysisHandle cache key; equality of the hash means "the
// cached symbolic plan still applies to this pattern".
// -----------------------------------------------------------------------
[[nodiscard]] crd::u64 topology_hash(const SparsePattern& pattern) noexcept;

} // namespace crd::hesap::sparse
