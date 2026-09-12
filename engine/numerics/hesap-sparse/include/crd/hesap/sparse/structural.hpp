#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>

#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// Structural extract / slice ops (v1c-3) on compressed CSR. All preserve the
// canonical column-sorted invariant and are deterministic. Filtering ops
// (`triu`/`tril`/`submatrix`) walk each row in order and copy the surviving
// entries; `scale_rows` keeps the pattern and scales values.
// -----------------------------------------------------------------------

// Extract the main diagonal as a dense array of length min(rows, cols):
// d[i] = A[i, i] (T{} when structurally absent).
template <typename T>
[[nodiscard]] crd::containers::Array<T> extract_diagonal(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                         crd::memory::IAllocator* alloc)
{
    const SparsePattern& pat = a.pattern();
    CRD_ASSERT_MSG(pat.is_compressed(), "extract_diagonal requires compressed CSR");
    const crd::u32  n     = pat.rows < pat.cols ? pat.rows : pat.cols;
    const crd::u32* outer = pat.outer_ptr.data();
    const crd::u32* inner = pat.inner_idx.data();
    const T*        vals  = a.values().values.data();

    crd::containers::Array<T> d(alloc);
    d.resize(n);  // value-init T{}
    for (crd::u32 r = 0; r < n; ++r)
    {
        for (crd::u32 k = outer[r]; k < outer[r + 1]; ++k)
        {
            if (inner[k] == r)
            {
                d[r] = vals[k];
                break;
            }
        }
    }
    return d;
}

// C = diag(scale) * A : scale row i of A by scale[i]. Pattern preserved.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> scale_rows(crd::containers::ConstSpan<T> scale,
                                                            const SparseMatrix<T, SparseFormat::Csr>& a,
                                                            crd::memory::IAllocator* alloc)
{
    const SparsePattern& pa = a.pattern();
    CRD_ASSERT_MSG(pa.is_compressed(), "scale_rows requires compressed CSR");
    CRD_ASSERT_MSG(scale.size() == pa.rows, "scale_rows: scale length must equal rows");
    const crd::u32  nnz   = static_cast<crd::u32>(pa.nnz());
    const crd::u32* outer = pa.outer_ptr.data();
    const T*        va    = a.values().values.data();

    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = pa.rows;
    pat.cols       = pa.cols;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    pat.outer_ptr.resize(pa.outer_ptr.size());
    for (crd::usize i = 0; i < pa.outer_ptr.size(); ++i)
    {
        pat.outer_ptr[i] = pa.outer_ptr[i];
    }
    pat.inner_idx.resize_uninitialized(nnz);
    vals.values.resize_uninitialized(nnz);
    const crd::u32* ia = pa.inner_idx.data();
    for (crd::u32 r = 0; r < pa.rows; ++r)
    {
        const T s = scale[r];
        for (crd::u32 k = outer[r]; k < outer[r + 1]; ++k)
        {
            pat.inner_idx[k] = ia[k];
            vals.values[k]   = s * va[k];
        }
    }
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

namespace detail
{
// Keep entries of row r whose column satisfies a predicate. upper=true keeps
// col >= r + k (triu); upper=false keeps col <= r + k (tril). k is the diagonal
// offset (signed).
template <typename T, bool Upper>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> triangular(const SparseMatrix<T, SparseFormat::Csr>& a, crd::i32 k,
                                                            crd::memory::IAllocator* alloc)
{
    const SparsePattern& pa = a.pattern();
    CRD_ASSERT_MSG(pa.is_compressed(), "triu/tril require compressed CSR");
    const crd::u32* outer = pa.outer_ptr.data();
    const crd::u32* ia    = pa.inner_idx.data();
    const T*        va    = a.values().values.data();

    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = pa.rows;
    pat.cols       = pa.cols;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    pat.outer_ptr.resize(pa.outer_ptr.size());
    pat.outer_ptr[0] = 0;
    pat.inner_idx.reserve(pa.nnz());
    vals.values.reserve(pa.nnz());
    for (crd::u32 r = 0; r < pa.rows; ++r)
    {
        const crd::i64 thresh = static_cast<crd::i64>(r) + k;
        for (crd::u32 idx = outer[r]; idx < outer[r + 1]; ++idx)
        {
            const crd::i64 c    = static_cast<crd::i64>(ia[idx]);
            const bool     keep = Upper ? (c >= thresh) : (c <= thresh);
            if (keep)
            {
                pat.inner_idx.push_back(ia[idx]);
                vals.values.push_back(va[idx]);
            }
        }
        pat.outer_ptr[r + 1] = static_cast<crd::u32>(pat.inner_idx.size());
    }
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}
} // namespace detail

// Upper triangular part (entries with col >= row + k). k=0 includes the diagonal.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> triu(const SparseMatrix<T, SparseFormat::Csr>& a, crd::i32 k,
                                                      crd::memory::IAllocator* alloc)
{
    return detail::triangular<T, true>(a, k, alloc);
}

// Lower triangular part (entries with col <= row + k). k=0 includes the diagonal.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> tril(const SparseMatrix<T, SparseFormat::Csr>& a, crd::i32 k,
                                                      crd::memory::IAllocator* alloc)
{
    return detail::triangular<T, false>(a, k, alloc);
}

// Submatrix A[r0:r1, c0:c1] as a new (r1-r0) x (c1-c0) CSR, columns reindexed.
// Row / column slices are the natural special cases.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> submatrix(const SparseMatrix<T, SparseFormat::Csr>& a, crd::u32 r0,
                                                           crd::u32 r1, crd::u32 c0, crd::u32 c1,
                                                           crd::memory::IAllocator* alloc)
{
    const SparsePattern& pa = a.pattern();
    CRD_ASSERT_MSG(pa.is_compressed(), "submatrix requires compressed CSR");
    CRD_ASSERT_MSG(r0 <= r1 && r1 <= pa.rows && c0 <= c1 && c1 <= pa.cols, "submatrix: range out of bounds");
    const crd::u32* outer = pa.outer_ptr.data();
    const crd::u32* ia    = pa.inner_idx.data();
    const T*        va    = a.values().values.data();

    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = r1 - r0;
    pat.cols       = c1 - c0;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    pat.outer_ptr.resize(static_cast<crd::usize>(pat.rows) + 1);
    pat.outer_ptr[0] = 0;
    pat.inner_idx.reserve(pa.nnz());
    vals.values.reserve(pa.nnz());
    for (crd::u32 r = r0; r < r1; ++r)
    {
        for (crd::u32 k = outer[r]; k < outer[r + 1]; ++k)
        {
            const crd::u32 c = ia[k];
            if (c >= c0 && c < c1)  // inner_idx sorted: could early-break, but simple scan is clear
            {
                pat.inner_idx.push_back(c - c0);
                vals.values.push_back(va[k]);
            }
        }
        pat.outer_ptr[r - r0 + 1] = static_cast<crd::u32>(pat.inner_idx.size());
    }
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

} // namespace crd::hesap::sparse
