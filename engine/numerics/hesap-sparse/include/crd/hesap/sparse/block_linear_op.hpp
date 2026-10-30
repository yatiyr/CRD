#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/detail/operator_arms.hpp>
#include <crd/hesap/sparse/sparse_dispatch.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spmm.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// BlockLinearOp<T> -- the block (multi-RHS) analogue of LinearOp<T>. Phase 3.1.6
// v4f-2. Deliberately SEPARATE from LinearOp (whose vtable is LOCKED): a virtual
// block-apply default-implemented as a column loop would silently degrade the
// "block" framing to s spmvs. Implementing this interface is an explicit opt-in to
// the one-pass spmm throughput path that block-Krylov needs.
//
// `apply_block(X, ldx, Y, ldy, s)`: Y (n_rows × s, row-major, ldy stride) ← A · X
// (n_cols × s, row-major, ldx stride). The explicit leading dimensions let a block
// solver pass sub-blocks of a larger workspace without copying.
// -----------------------------------------------------------------------

template <typename T>
class BlockLinearOp
{
public:
    virtual ~BlockLinearOp() = default;

    [[nodiscard]] virtual bool apply_block(crd::containers::ConstSpan<T> x, crd::u32 ldx, crd::containers::Span<T> y,
                                           crd::u32 ldy, crd::u32 s) const = 0;

    [[nodiscard]] virtual crd::usize n_rows() const noexcept = 0;
    [[nodiscard]] virtual crd::usize n_cols() const noexcept = 0;
};

// -----------------------------------------------------------------------
// ParallelSpmmLinearOp<T> -- a square BlockLinearOp over a CSR matrix whose
// apply_block uses the nnz-balanced parallel spmm (bit-exact across threads, v1e
// D(sparse)) above a working-set threshold, serial spmm below or on a pool that is
// down (the same switch as ParallelSparseLinearOp). One pass over A for all s RHS.
// -----------------------------------------------------------------------

template <typename T>
class ParallelSpmmLinearOp final : public BlockLinearOp<T>
{
public:
    explicit ParallelSpmmLinearOp(const SparseMatrix<T, SparseFormat::Csr>& a,
                                  crd::usize parallel_min_stored_bytes = sparse_resident_bytes())
        : ParallelSpmmLinearOp(a, detail::OperatorPick{.parallel_min_bytes = parallel_min_stored_bytes})
    {
    }

    // The cut (on A's values) and the forced jobs (detail/operator_arms.hpp).
    ParallelSpmmLinearOp(const SparseMatrix<T, SparseFormat::Csr>& a, const detail::OperatorPick& pick)
        : m_a(&a), m_n(a.rows()), m_pick(pick)
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "ParallelSpmmLinearOp: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "ParallelSpmmLinearOp: requires a compressed CSR matrix");
        m_parallel = a.nnz() * sizeof(T) >= pick.parallel_min_bytes;
    }

    [[nodiscard]] bool apply_block(crd::containers::ConstSpan<T> x, crd::u32 ldx, crd::containers::Span<T> y,
                                   crd::u32 ldy, crd::u32 s) const override
    {
        const crd::u32 jobs = m_parallel ? detail::operator_jobs(m_pick) : 1U;
        if (jobs <= 1)
        {
            spmm<T>(T(1), *m_a, x.data(), ldx, s, T(0), y.data(), ldy);
            return true;
        }
        spmm_parallel<T>(T(1), *m_a, x.data(), ldx, s, T(0), y.data(), ldy, jobs);
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }
    [[nodiscard]] bool       is_parallel() const noexcept { return m_parallel; }

private:
    const SparseMatrix<T, SparseFormat::Csr>* m_a;
    crd::usize                                m_n;
    detail::OperatorPick m_pick;
    bool                                      m_parallel = false;
};

} // namespace crd::hesap::sparse
