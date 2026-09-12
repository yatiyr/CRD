#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/convert.hpp>  // transpose (for A*A^T)
#include <crd/hesap/sparse/spgemm_hash.hpp>  // hash accumulator path (cols > kMaxSpaCols)
#include <crd/hesap/sparse/sparse_matrix.hpp>

#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// spgemm -- sparse matrix-matrix product C = A * B (all compressed CSR).
// Gustavson's algorithm with a per-row dense SPA (sparse accumulator):
// fused single pass (no separate symbolic re-walk -- advisor pin), so the
// cost is O(flops) = sum_i sum_{k in A[i]} nnz(B[k]).
//
// SPA-clear trick: the marker carries a per-row STAMP (row+1) so a slot is
// "touched this row" iff marker[j] == stamp -- no per-row memset of the
// B.cols-sized arrays. Only the actually-touched columns are revisited.
//
// Determinism (D(sparse)): each C row is accumulated in a FIXED order
// (A[i]'s stored k order, then B[k]'s stored column order) and emitted
// column-sorted -> bit-reproducible. Each C row is independent of the
// others, so the v1d-2 row-parallel driver is bit-exact vs this serial form.
//
// SPA scratch is sized by B.cols; `kMaxSpaCols` caps it (a hash accumulator
// would replace the dense SPA above that, a future refinement).
// -----------------------------------------------------------------------

inline constexpr crd::u32 kMaxSpaCols = 4U * 1024U * 1024U;  // 4M cols * (8+4) B ~ 48 MB/worker max

namespace detail
{
// One A-row range [i_begin, i_end) of C = A*B, written into per-call C arrays.
// `spa_val` (size n) + `marker` (size n, persistent stamp) + `touched` are the
// caller-owned scratch (one set per worker). `outer_ptr` must be sized m+1 and
// outer_ptr[i_begin] already set; this fills (i_begin, i_end] and appends to
// inner_idx/values. Returns nothing; the caller stitches per-worker outputs.
template <typename T>
void spgemm_rows(const SparseMatrix<T, SparseFormat::Csr>& a, const SparseMatrix<T, SparseFormat::Csr>& b,
                 crd::u32 i_begin, crd::u32 i_end, crd::containers::Array<T>& spa_val,
                 crd::containers::Array<crd::u32>& marker, crd::containers::Array<crd::u32>& touched,
                 crd::containers::Array<crd::u32>& out_inner, crd::containers::Array<T>& out_vals,
                 crd::containers::Array<crd::u32>& out_outer)
{
    const crd::u32* ao = a.pattern().outer_ptr.data();
    const crd::u32* ai = a.pattern().inner_idx.data();
    const T*        av = a.values().values.data();
    const crd::u32* bo = b.pattern().outer_ptr.data();
    const crd::u32* bi = b.pattern().inner_idx.data();
    const T*        bv = b.values().values.data();

    for (crd::u32 i = i_begin; i < i_end; ++i)
    {
        const crd::u32 stamp = i + 1U;  // never 0 (marker init is 0)
        touched.clear();
        for (crd::u32 ka = ao[i]; ka < ao[i + 1]; ++ka)
        {
            const crd::u32 k = ai[ka];
            const T        aval = av[ka];
            for (crd::u32 kb = bo[k]; kb < bo[k + 1]; ++kb)
            {
                const crd::u32 j = bi[kb];
                const T        prod = aval * bv[kb];
                if (marker[j] != stamp)
                {
                    marker[j]  = stamp;
                    spa_val[j] = prod;
                    touched.push_back(j);
                }
                else
                {
                    spa_val[j] = spa_val[j] + prod;
                }
            }
        }
        crd::containers::sort(touched.data(), touched.data() + touched.size());
        for (crd::usize t = 0; t < touched.size(); ++t)
        {
            const crd::u32 j = touched[t];
            out_inner.push_back(j);
            out_vals.push_back(spa_val[j]);
        }
        out_outer[i + 1] = static_cast<crd::u32>(out_inner.size());
    }
}
} // namespace detail

// C = A * B (serial). A is m x k, B is k x n -> C is m x n.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> spgemm(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                        const SparseMatrix<T, SparseFormat::Csr>& b,
                                                        crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(a.pattern().is_compressed() && b.pattern().is_compressed(), "spgemm requires compressed CSR");
    CRD_ASSERT_MSG(a.cols() == b.rows(), "spgemm: inner dimension mismatch (A.cols != B.rows)");
    const crd::u32 m = a.rows();
    const crd::u32 n = b.cols();

    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = m;
    pat.cols       = n;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    pat.outer_ptr.resize(static_cast<crd::usize>(m) + 1);
    pat.outer_ptr[0] = 0;
    pat.inner_idx.reserve(a.nnz() + b.nnz());
    vals.values.reserve(a.nnz() + b.nnz());

    if (n > kMaxSpaCols)
    {
        // Hash-accumulator path (bounded memory) -- lifts the dense-SPA ceiling.
        // Bit-exact with the dense path (same accumulation order + sorted emit).
        detail::SpgemmHash<T> hash(alloc);
        detail::spgemm_rows_hash<T>(a, b, 0, m, hash, pat.inner_idx, vals.values, pat.outer_ptr);
        pat.recompute_topology_hash();
        return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
    }

    crd::containers::Array<T>        spa_val(alloc);
    crd::containers::Array<crd::u32> marker(alloc);
    crd::containers::Array<crd::u32> touched(alloc);
    spa_val.resize(n);  // values valid only where marked; no need to zero meaningfully
    marker.resize(n);   // value-init 0 == "never touched"

    detail::spgemm_rows<T>(a, b, 0, m, spa_val, marker, touched, pat.inner_idx, vals.values, pat.outer_ptr);

    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

// C = A * Aᵀ (normal-equations form; C is m x m, symmetric for real A).
// Composes transpose (v1c-1) + spgemm; materialising Aᵀ is O(nnz) -- cheap vs
// the multiply. NON-conjugating for complex (true transpose, not adjoint).
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> spgemm_ata(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                            crd::memory::IAllocator* alloc)
{
    return spgemm(a, transpose(a, alloc), alloc);
}

} // namespace crd::hesap::sparse
