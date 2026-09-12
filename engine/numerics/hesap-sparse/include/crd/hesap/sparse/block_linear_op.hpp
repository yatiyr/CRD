#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
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
// D(sparse)) above a working-set threshold, serial spmm below (the same
// size-adaptive switch as ParallelSparseLinearOp). One pass over A for all s RHS.
// -----------------------------------------------------------------------

template <typename T>
class ParallelSpmmLinearOp final : public BlockLinearOp<T>
{
public:
    explicit ParallelSpmmLinearOp(const SparseMatrix<T, SparseFormat::Csr>& a,
                                  crd::usize parallel_min_stored_bytes = (crd::usize{1} << 21))
        : m_a(&a), m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "ParallelSpmmLinearOp: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "ParallelSpmmLinearOp: requires a compressed CSR matrix");
        m_parallel = a.nnz() * sizeof(T) >= parallel_min_stored_bytes;
    }

    [[nodiscard]] bool apply_block(crd::containers::ConstSpan<T> x, crd::u32 ldx, crd::containers::Span<T> y,
                                   crd::u32 ldy, crd::u32 s) const override
    {
        if (!m_parallel)
        {
            spmm<T>(T(1), *m_a, x.data(), ldx, s, T(0), y.data(), ldy);
            return true;
        }
        spmm_parallel<T>(T(1), *m_a, x.data(), ldx, s, T(0), y.data(), ldy);
        crd::jobs::frame_reset(); // block-Krylov loop arena hygiene (cf. ParallelSparseLinearOp)
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }
    [[nodiscard]] bool       is_parallel() const noexcept { return m_parallel; }

private:
    const SparseMatrix<T, SparseFormat::Csr>* m_a;
    crd::usize                                m_n;
    bool                                      m_parallel = false;
};

} // namespace crd::hesap::sparse
