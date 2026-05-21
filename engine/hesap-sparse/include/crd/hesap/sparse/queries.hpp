#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// Structural queries (v1a-3). Read-only inspectors over a SparseMatrix's
// pattern -- type-independent; they look only at structure, never values.
// -----------------------------------------------------------------------

struct StructuralStats
{
    crd::u32   rows = 0;
    crd::u32   cols = 0;
    crd::usize nnz = 0;
    crd::f64   density = 0.0;
    crd::u32   n_outer = 0;        // inner vectors: rows (CSR) / cols (CSC)
    crd::u32   min_inner_nnz = 0;  // smallest used count over inner vectors
    crd::u32   max_inner_nnz = 0;  // largest used count over inner vectors
    bool       is_compressed = true;
};

template <typename T, SparseFormat F>
[[nodiscard]] StructuralStats structural_stats(const SparseMatrix<T, F>& m) noexcept
{
    const SparsePattern& pat = m.pattern();
    StructuralStats s;
    s.rows          = pat.rows;
    s.cols          = pat.cols;
    s.nnz           = m.nnz();
    s.density       = m.density();
    s.n_outer       = pat.n_outer();
    s.is_compressed = pat.is_compressed();
    if (s.n_outer > 0)
    {
        s.min_inner_nnz = pat.inner_count(0);
        s.max_inner_nnz = pat.inner_count(0);
        for (crd::u32 k = 1; k < s.n_outer; ++k)
        {
            const crd::u32 c = pat.inner_count(k);
            s.min_inner_nnz = c < s.min_inner_nnz ? c : s.min_inner_nnz;
            s.max_inner_nnz = c > s.max_inner_nnz ? c : s.max_inner_nnz;
        }
    }
    return s;
}

// The used, canonical-sorted inner indices of inner vector k (columns of row
// k for CSR / rows of column k for CSC). Empty span for an out-of-range k.
template <typename T, SparseFormat F>
[[nodiscard]] crd::containers::ConstSpan<crd::u32> inner_indices(const SparseMatrix<T, F>& m, crd::u32 k) noexcept
{
    const SparsePattern& pat = m.pattern();
    if (k >= pat.n_outer())
    {
        return {};
    }
    const crd::u32 start = pat.outer_ptr[k];
    const crd::u32 used  = pat.inner_count(k);
    return crd::containers::ConstSpan<crd::u32>{pat.inner_idx.data() + start, used};
}

} // namespace crd::hesap::sparse
