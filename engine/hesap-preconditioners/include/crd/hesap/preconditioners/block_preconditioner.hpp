#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// Block preconditioners for block-PCG (Phase 3.1.6 v4f-2). Two pieces complete
// the block-Krylov preconditioning family:
//
//   JacobiBlockPreconditioner<T>   -- native one-pass diagonal (point-Jacobi)
//                                     block apply: Z[i,:] = R[i,:] · 1/A[i,i],
//                                     all s columns of row i in one go. The cheap,
//                                     always-available SPD preconditioner.
//
//   BlockPreconditionerAdapter<T>  -- wraps ANY single-vector LinearOp<T>
//                                     preconditioner (SSOR, IC, block-Jacobi, …)
//                                     as a BlockLinearOp by gather/apply/scatter
//                                     per column. One adapter ⇒ every existing
//                                     preconditioner works in block mode (the
//                                     complete-family bridge, no per-preconditioner
//                                     block rewrite).
//
// Both are deterministic (serial, fixed-order) and allocation-free at apply time.
// -----------------------------------------------------------------------

// Diagonal (point-Jacobi) block preconditioner: M = diag(A), so M⁻¹R scales each
// row of the block by 1/A[i,i]. A missing/zero diagonal degrades gracefully to 1
// (identity on that row) — a preconditioner is approximate; never assert.
template <typename T>
class JacobiBlockPreconditioner final : public crd::hesap::sparse::BlockLinearOp<T>
{
public:
    JacobiBlockPreconditioner(const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& a,
                              crd::memory::IAllocator* alloc)
        : m_inv_diag(alloc), m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "JacobiBlockPreconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "JacobiBlockPreconditioner: requires a compressed CSR matrix");
        const auto* outer = a.pattern().outer_ptr.data();
        const auto* inner = a.pattern().inner_idx.data();
        const T*    vals  = a.values().values.data();
        m_inv_diag.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            T diag{};
            for (crd::u32 t = outer[i]; t < outer[i + 1]; ++t)
            {
                if (inner[t] == i)
                {
                    diag = vals[t];
                    break;
                }
            }
            m_inv_diag[i] = (diag == T{}) ? T(1) : T(1) / diag;
        }
    }

    [[nodiscard]] bool apply_block(crd::containers::ConstSpan<T> x, crd::u32 ldx, crd::containers::Span<T> y,
                                   crd::u32 ldy, crd::u32 s) const override
    {
        const T* xr = x.data();
        T*       yr = y.data();
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            const T  d  = m_inv_diag[i];
            const T* xi = xr + static_cast<crd::usize>(i) * ldx;
            T*       yi = yr + static_cast<crd::usize>(i) * ldy;
            for (crd::u32 j = 0; j < s; ++j)
            {
                yi[j] = d * xi[j]; // scale the whole row by 1/A[i,i]
            }
        }
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    crd::containers::Array<T> m_inv_diag; // 1/A[i,i] per row (1 where diagonal absent/zero)
    crd::u32                  m_n;
};

// Adapter: present any single-vector LinearOp<T> preconditioner as a BlockLinearOp
// by applying its action to each of the s columns (gather column → apply → scatter).
// The n-length gather/scatter scratch is owned (allocated once); apply_block writes
// it, so the adapter is NOT thread-safe across concurrent calls — which is fine:
// block-Krylov applies the preconditioner serially on the calling thread (the only
// parallel step is the operator's block spmm). Determinism is preserved: each column
// gets the wrapped op's deterministic action in fixed column order.
template <typename T>
class BlockPreconditionerAdapter final : public crd::hesap::sparse::BlockLinearOp<T>
{
public:
    BlockPreconditionerAdapter(const crd::hesap::LinearOp<T>& m, crd::memory::IAllocator* alloc)
        : m_m(&m), m_col_in(alloc), m_col_out(alloc), m_n(m.n_rows())
    {
        CRD_ASSERT_MSG(m.n_rows() == m.n_cols(), "BlockPreconditionerAdapter: preconditioner must be square");
        m_col_in.resize(m_n);
        m_col_out.resize(m_n);
    }

    [[nodiscard]] bool apply_block(crd::containers::ConstSpan<T> x, crd::u32 ldx, crd::containers::Span<T> y,
                                   crd::u32 ldy, crd::u32 s) const override
    {
        const T* xr = x.data();
        T*       yr = y.data();
        T*       ci = m_col_in.data();
        T*       co = m_col_out.data();
        for (crd::u32 j = 0; j < s; ++j)
        {
            for (crd::usize i = 0; i < m_n; ++i) { ci[i] = xr[i * ldx + j]; } // gather column j
            if (!m_m->apply(crd::containers::ConstSpan<T>{ci, m_n}, crd::containers::Span<T>{co, m_n}))
            {
                return false;
            }
            for (crd::usize i = 0; i < m_n; ++i) { yr[i * ldy + j] = co[i]; } // scatter back
        }
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    const crd::hesap::LinearOp<T>*    m_m;
    mutable crd::containers::Array<T> m_col_in;  // serial-solver scratch (see note above)
    mutable crd::containers::Array<T> m_col_out;
    crd::usize                        m_n;
};

} // namespace crd::hesap::preconditioners
