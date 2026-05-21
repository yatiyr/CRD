#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>

#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// Format conversions + transpose (v1c-1). CSR is the pivot format. All are
// real O(nnz) builds (NOT zero-cost relabels): organise a compressed matrix's
// entries by its INNER index via count + prefix-sum + ordered scatter. Because
// the source outer vectors are walked in increasing order, the scattered
// indices land sorted within each new outer vector -> canonical, deterministic.
//
// `to_csc(CSR)` and `transpose(CSR)` share the identical kernel and differ
// only in how the result is labelled: CSC keeps (rows, cols); the transpose
// swaps them and labels the result CSR (A's CSC structure IS Aᵀ's CSR).
// -----------------------------------------------------------------------

namespace detail
{
// Reorganise `pat` (compressed) by its inner index into the transposed
// compressed arrays. `n_inner` is the range of the inner index (cols for a
// CSR source / rows for a CSC source). Output: out_outer (length n_inner+1),
// out_inner (the source's outer indices, sorted ascending per bucket), out_vals.
template <typename T>
void organize_by_inner(const SparsePattern& pat, const T* src_vals, crd::u32 n_inner,
                       crd::memory::IAllocator* alloc, crd::containers::Array<crd::u32>& out_outer,
                       crd::containers::Array<crd::u32>& out_inner, crd::containers::Array<T>& out_vals)
{
    CRD_ASSERT_MSG(pat.is_compressed(), "organize_by_inner requires a compressed matrix");
    const crd::u32  n_outer = pat.n_outer();
    const crd::u32  nnz     = static_cast<crd::u32>(pat.inner_idx.size());
    const crd::u32* outer   = pat.outer_ptr.data();
    const crd::u32* inner   = pat.inner_idx.data();

    out_outer.resize(static_cast<crd::usize>(n_inner) + 1);  // value-init 0
    for (crd::u32 k = 0; k < nnz; ++k)
    {
        ++out_outer[inner[k] + 1];
    }
    for (crd::u32 c = 0; c < n_inner; ++c)
    {
        out_outer[c + 1] += out_outer[c];
    }

    out_inner.resize_uninitialized(nnz);
    out_vals.resize_uninitialized(nnz);
    crd::containers::Array<crd::u32> wp(alloc);
    wp.resize(n_inner);
    for (crd::u32 c = 0; c < n_inner; ++c)
    {
        wp[c] = out_outer[c];
    }
    for (crd::u32 o = 0; o < n_outer; ++o)  // ascending o => sorted inner per bucket
    {
        for (crd::u32 k = outer[o]; k < outer[o + 1]; ++k)
        {
            const crd::u32 c = inner[k];
            const crd::u32 p = wp[c]++;
            out_inner[p] = o;
            out_vals[p]  = src_vals[k];
        }
    }
}
} // namespace detail

// CSR -> CSC (same matrix, column-organised). O(nnz).
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csc> to_csc(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                        crd::memory::IAllocator* alloc)
{
    const SparsePattern& src = a.pattern();
    SparsePattern        pat(alloc);
    SparseValues<T>      vals(alloc);
    pat.rows       = src.rows;
    pat.cols       = src.cols;
    pat.format     = SparseFormat::Csc;
    pat.block_size = 1;
    detail::organize_by_inner<T>(src, a.values().values.data(), src.cols, alloc, pat.outer_ptr, pat.inner_idx,
                                 vals.values);
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csc>(std::move(pat), std::move(vals));
}

// CSC -> CSR (same matrix, row-organised). O(nnz).
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> from_csc(const SparseMatrix<T, SparseFormat::Csc>& a,
                                                          crd::memory::IAllocator* alloc)
{
    const SparsePattern& src = a.pattern();
    SparsePattern        pat(alloc);
    SparseValues<T>      vals(alloc);
    pat.rows       = src.rows;
    pat.cols       = src.cols;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    detail::organize_by_inner<T>(src, a.values().values.data(), src.rows, alloc, pat.outer_ptr, pat.inner_idx,
                                 vals.values);
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

// transpose: CSR A -> CSR Aᵀ. Real build (A's column structure becomes Aᵀ's
// rows); dims swapped. transpose(transpose(A)) == A byte-exactly.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> transpose(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                           crd::memory::IAllocator* alloc)
{
    const SparsePattern& src = a.pattern();
    SparsePattern        pat(alloc);
    SparseValues<T>      vals(alloc);
    pat.rows       = src.cols;  // Aᵀ has A's column count as its row count
    pat.cols       = src.rows;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    detail::organize_by_inner<T>(src, a.values().values.data(), src.cols, alloc, pat.outer_ptr, pat.inner_idx,
                                 vals.values);
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

// Coordinate (COO) expansion of a CSR matrix: parallel (row, col, value) arrays
// in row-major order. Round-trips through TripletBuilder::compress.
template <typename T>
struct Coo
{
    crd::u32                          rows = 0;
    crd::u32                          cols = 0;
    crd::containers::Array<crd::u32>  row_idx;
    crd::containers::Array<crd::u32>  col_idx;
    crd::containers::Array<T>         values;

    explicit Coo(crd::memory::IAllocator* alloc) : row_idx(alloc), col_idx(alloc), values(alloc) {}
};

template <typename T>
[[nodiscard]] Coo<T> to_coo(const SparseMatrix<T, SparseFormat::Csr>& a, crd::memory::IAllocator* alloc)
{
    const SparsePattern& pat = a.pattern();
    CRD_ASSERT_MSG(pat.is_compressed(), "to_coo requires a compressed CSR matrix");
    const crd::u32  nnz   = static_cast<crd::u32>(pat.nnz());
    const crd::u32* outer = pat.outer_ptr.data();
    const crd::u32* inner = pat.inner_idx.data();
    const T*        vals  = a.values().values.data();

    Coo<T> out(alloc);
    out.rows = pat.rows;
    out.cols = pat.cols;
    out.row_idx.resize_uninitialized(nnz);
    out.col_idx.resize_uninitialized(nnz);
    out.values.resize_uninitialized(nnz);
    for (crd::u32 r = 0; r < pat.rows; ++r)
    {
        for (crd::u32 k = outer[r]; k < outer[r + 1]; ++k)
        {
            out.row_idx[k] = r;
            out.col_idx[k] = inner[k];
            out.values[k]  = vals[k];
        }
    }
    return out;
}

} // namespace crd::hesap::sparse
