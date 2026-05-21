#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>

#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// Element-wise CSR ops (v1c-2). Deterministic: outputs are canonical
// column-sorted; matched columns combine as `a OP b` (LEFT operand first),
// one rounding per matched column (D(sparse)-5). A±B uses a fast path when
// the operands share structure (topology_hash equal — the hash covers
// format + block_size + sorted indices, so equality is a safe gate) and a
// symbolic-union merge otherwise. Hadamard keeps the intersection only.
// -----------------------------------------------------------------------

namespace detail
{
// Sign of B's contribution: +1 for add, -1 for subtract.
enum class EwiseSign
{
    Add,
    Sub
};

// A±B with symbolic-union row merge. Both compressed CSR, same dims.
template <typename T, EwiseSign Sign>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> add_sub(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                         const SparseMatrix<T, SparseFormat::Csr>& b,
                                                         crd::memory::IAllocator* alloc)
{
    const SparsePattern& pa = a.pattern();
    const SparsePattern& pb = b.pattern();
    CRD_ASSERT_MSG(pa.is_compressed() && pb.is_compressed(), "add/sub require compressed CSR");
    CRD_ASSERT_MSG(pa.rows == pb.rows && pa.cols == pb.cols, "add/sub: dimension mismatch");

    const crd::u32* oa = pa.outer_ptr.data();
    const crd::u32* ia = pa.inner_idx.data();
    const T*        va = a.values().values.data();
    const crd::u32* ob = pb.outer_ptr.data();
    const crd::u32* ib = pb.inner_idx.data();
    const T*        vb = b.values().values.data();

    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = pa.rows;
    pat.cols       = pa.cols;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    pat.outer_ptr.resize(static_cast<crd::usize>(pa.rows) + 1);
    pat.outer_ptr[0] = 0;
    pat.inner_idx.reserve(pa.nnz() + pb.nnz());
    vals.values.reserve(pa.nnz() + pb.nnz());

    // Fast path: identical structure -> walk one index set, combine values.
    if (pa.topology_hash == pb.topology_hash)
    {
        const crd::u32 nnz = static_cast<crd::u32>(pa.nnz());
        for (crd::u32 k = 0; k < nnz; ++k)
        {
            pat.inner_idx.push_back(ia[k]);
            if constexpr (Sign == EwiseSign::Add)
            {
                vals.values.push_back(va[k] + vb[k]);
            }
            else
            {
                vals.values.push_back(va[k] - vb[k]);
            }
        }
        for (crd::u32 r = 0; r < pa.rows; ++r)
        {
            pat.outer_ptr[r + 1] = oa[r + 1];
        }
        pat.recompute_topology_hash();
        return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
    }

    // General path: per-row sorted merge of the two column sets.
    for (crd::u32 r = 0; r < pa.rows; ++r)
    {
        crd::u32 ka = oa[r];
        crd::u32 kb = ob[r];
        const crd::u32 ea = oa[r + 1];
        const crd::u32 eb = ob[r + 1];
        while (ka < ea && kb < eb)
        {
            if (ia[ka] < ib[kb])
            {
                pat.inner_idx.push_back(ia[ka]);
                vals.values.push_back(va[ka]);
                ++ka;
            }
            else if (ib[kb] < ia[ka])
            {
                pat.inner_idx.push_back(ib[kb]);
                vals.values.push_back(Sign == EwiseSign::Add ? vb[kb] : T{} - vb[kb]);
                ++kb;
            }
            else  // matched column: a OP b (left first, one rounding)
            {
                pat.inner_idx.push_back(ia[ka]);
                vals.values.push_back(Sign == EwiseSign::Add ? (va[ka] + vb[kb]) : (va[ka] - vb[kb]));
                ++ka;
                ++kb;
            }
        }
        for (; ka < ea; ++ka)
        {
            pat.inner_idx.push_back(ia[ka]);
            vals.values.push_back(va[ka]);
        }
        for (; kb < eb; ++kb)
        {
            pat.inner_idx.push_back(ib[kb]);
            vals.values.push_back(Sign == EwiseSign::Add ? vb[kb] : T{} - vb[kb]);
        }
        pat.outer_ptr[r + 1] = static_cast<crd::u32>(pat.inner_idx.size());
    }
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}
} // namespace detail

// C = A + B (structural union; matched columns summed a+b).
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> add(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                     const SparseMatrix<T, SparseFormat::Csr>& b,
                                                     crd::memory::IAllocator* alloc)
{
    return detail::add_sub<T, detail::EwiseSign::Add>(a, b, alloc);
}

// C = A - B (structural union; matched columns a-b).
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> subtract(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                          const SparseMatrix<T, SparseFormat::Csr>& b,
                                                          crd::memory::IAllocator* alloc)
{
    return detail::add_sub<T, detail::EwiseSign::Sub>(a, b, alloc);
}

// C = alpha * A (same structure; values scaled).
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> scale(T alpha, const SparseMatrix<T, SparseFormat::Csr>& a,
                                                       crd::memory::IAllocator* alloc)
{
    const SparsePattern& pa = a.pattern();
    CRD_ASSERT_MSG(pa.is_compressed(), "scale requires compressed CSR");
    const crd::u32 nnz = static_cast<crd::u32>(pa.nnz());

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
    const T*        va = a.values().values.data();
    for (crd::u32 k = 0; k < nnz; ++k)
    {
        pat.inner_idx[k] = ia[k];
        vals.values[k]   = alpha * va[k];
    }
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

// C = A .* B (Hadamard / element-wise product; INTERSECTION of structures).
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> hadamard(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                          const SparseMatrix<T, SparseFormat::Csr>& b,
                                                          crd::memory::IAllocator* alloc)
{
    const SparsePattern& pa = a.pattern();
    const SparsePattern& pb = b.pattern();
    CRD_ASSERT_MSG(pa.is_compressed() && pb.is_compressed(), "hadamard requires compressed CSR");
    CRD_ASSERT_MSG(pa.rows == pb.rows && pa.cols == pb.cols, "hadamard: dimension mismatch");

    const crd::u32* oa = pa.outer_ptr.data();
    const crd::u32* ia = pa.inner_idx.data();
    const T*        va = a.values().values.data();
    const crd::u32* ob = pb.outer_ptr.data();
    const crd::u32* ib = pb.inner_idx.data();
    const T*        vb = b.values().values.data();

    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = pa.rows;
    pat.cols       = pa.cols;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    pat.outer_ptr.resize(static_cast<crd::usize>(pa.rows) + 1);
    pat.outer_ptr[0] = 0;
    const crd::usize cap = pa.nnz() < pb.nnz() ? pa.nnz() : pb.nnz();
    pat.inner_idx.reserve(cap);
    vals.values.reserve(cap);

    for (crd::u32 r = 0; r < pa.rows; ++r)
    {
        crd::u32       ka = oa[r];
        crd::u32       kb = ob[r];
        const crd::u32 ea = oa[r + 1];
        const crd::u32 eb = ob[r + 1];
        while (ka < ea && kb < eb)
        {
            if (ia[ka] < ib[kb])
            {
                ++ka;
            }
            else if (ib[kb] < ia[ka])
            {
                ++kb;
            }
            else
            {
                pat.inner_idx.push_back(ia[ka]);
                vals.values.push_back(va[ka] * vb[kb]);
                ++ka;
                ++kb;
            }
        }
        pat.outer_ptr[r + 1] = static_cast<crd::u32>(pat.inner_idx.size());
    }
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

} // namespace crd::hesap::sparse
