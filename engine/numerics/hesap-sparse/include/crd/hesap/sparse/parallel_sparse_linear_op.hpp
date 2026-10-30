#pragma once

#include <crd/core/assert.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/detail/operator_arms.hpp>
#include <crd/hesap/sparse/sell.hpp>
#include <crd/hesap/sparse/sell_parallel.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spmv.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::sparse
{
namespace detail
{
// y = s x through the pick: the serial SELL spmv below the cut or on one job, the parallel one on the pick's jobs.
template <typename T>
void apply_sell(const OperatorPick& pick, bool parallel, const SellMatrix<T>& s, crd::containers::ConstSpan<T> x,
                crd::containers::Span<T> y)
{
    const crd::u32 jobs = parallel ? operator_jobs(pick) : 1U;
    if (jobs <= 1)
    {
        spmv_sell<T>(T(1), s, x, T(0), y); // sub-cache: serial wins (no dispatch overhead)
        return;
    }
    spmv_sell_parallel<T>(T(1), s, x, T(0), y, jobs);
}
} // namespace detail

// -----------------------------------------------------------------------
// ParallelSparseLinearOp<T> -- a LinearOp<T> whose apply uses the row-balanced
// parallel SELL-C-σ spmv (over crd::jobs). Phase 3.1.6 v4a-2.
//
// Built once from a compressed CSR matrix (converted to SELL, owned). This is
// BOTH the performance path for Krylov solvers (DRAM-bound parallel spmv, the
// regime where Cerid beats Eigen's serial scalar spmv) AND the operator behind
// the determinism moat: spmv_sell_parallel is BIT-EXACT vs serial at any job
// count (v1b D(sparse)-3), so a CG solve over this operator yields a
// bit-identical {iterations, residual sequence, solution} regardless of thread
// count. Apply is None-orientation only (SELL spmv has no transpose path).
//
// Past the cut it applies on crd::jobs; on a pool that is down, serially. Non-square / transpose
// uses are not supported (CG/PCG only need A·x). The wrapped CSR need not
// outlive the op (the SELL copy is owned).
// -----------------------------------------------------------------------

template <typename T>
class ParallelSparseLinearOp final : public crd::hesap::LinearOp<T>
{
public:
    // `parallel_min_stored_bytes` is the working-set threshold above which apply
    // dispatches the parallel spmv; at or below it (a cache-resident matrix) the
    // serial spmv wins because per-call job dispatch costs more than it saves
    // (the v1b spmv regime, confirmed by the CG-vs-Eigen bench). Default: the L2 (sparse_resident_bytes()).
    explicit ParallelSparseLinearOp(const SparseMatrix<T, SparseFormat::Csr>& matrix, crd::memory::IAllocator* alloc,
                                    crd::usize parallel_min_stored_bytes = sparse_resident_bytes())
        : ParallelSparseLinearOp(matrix, alloc, detail::OperatorPick{.parallel_min_bytes = parallel_min_stored_bytes})
    {
    }

    // The cut and the forced jobs (detail/operator_arms.hpp); the bytes constructor is this with the cut alone.
    ParallelSparseLinearOp(const SparseMatrix<T, SparseFormat::Csr>& matrix, crd::memory::IAllocator* alloc,
                           const detail::OperatorPick& pick)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/false), m_sell(to_sell<T>(matrix, alloc)),
          m_pick(pick)
    {
        CRD_ASSERT_MSG(matrix.rows() == matrix.cols(), "ParallelSparseLinearOp: matrix must be square");
        m_parallel = m_sell.stored_bytes() >= pick.parallel_min_bytes;
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        detail::apply_sell<T>(m_pick, m_parallel, m_sell, x, y);
        return true;
    }

    // Whether the matrix reached the cut (apply then runs on the pick's jobs unless that is one or the pool is down).
    [[nodiscard]] bool is_parallel() const noexcept { return m_parallel; }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_sell.rows; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_sell.cols; }

private:
    SellMatrix<T> m_sell;
    detail::OperatorPick m_pick;
    bool          m_parallel = false;
};

// -----------------------------------------------------------------------
// ParallelSpmvLeastSquaresOp<T> -- a RECTANGULAR (m×n) LinearOp<T> with both
// apply (A·x) and apply_adjoint (Aᴴ·y) on the parallel SELL-C-σ path. Phase
// 3.1.6 v4d. For LSQR/LSMR/QMR, which need both directions every iteration:
// `SparseLinearOp::apply_transpose` is serial scalar CSR (v1b-slow), so this op
// stores A as SELL AND Aᴴ (conjugate-transpose) as a SECOND SELL -- both spmv
// directions are then parallel + bit-exact (the determinism moat) at the cost of
// ~2× storage (deliberate; the spmv-parallelism win dominates). Adjoint Aᴴ
// (not Aᵀ) is the single inner-product policy across the iterative module
// (matches BiCGSTAB/QMR dotc). Size-adaptive serial/parallel like the square op.
// -----------------------------------------------------------------------

template <typename T>
class ParallelSpmvLeastSquaresOp final : public crd::hesap::LinearOp<T>
{
public:
    explicit ParallelSpmvLeastSquaresOp(const SparseMatrix<T, SparseFormat::Csr>& a, crd::memory::IAllocator* alloc,
                                        crd::usize parallel_min_stored_bytes = sparse_resident_bytes())
        : ParallelSpmvLeastSquaresOp(a, alloc, detail::OperatorPick{.parallel_min_bytes = parallel_min_stored_bytes})
    {
    }

    // The cut (on the larger of A's and Aᴴ's SELL) and the forced jobs (detail/operator_arms.hpp).
    ParallelSpmvLeastSquaresOp(const SparseMatrix<T, SparseFormat::Csr>& a, crd::memory::IAllocator* alloc,
                               const detail::OperatorPick& pick)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true), m_a(to_sell<T>(a, alloc)),
          m_ah(build_adjoint_sell(a, alloc)), m_pick(pick), m_rows(a.rows()), m_cols(a.cols())
    {
        const crd::usize bytes = m_a.stored_bytes() > m_ah.stored_bytes() ? m_a.stored_bytes() : m_ah.stored_bytes();
        m_parallel = bytes >= pick.parallel_min_bytes;
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        detail::apply_sell<T>(m_pick, m_parallel, m_a, x, y);
        return true;
    }

    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        detail::apply_sell<T>(m_pick, m_parallel, m_ah, x, y);
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_rows; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_cols; }
    [[nodiscard]] bool       is_parallel() const noexcept { return m_parallel; }

private:
    static SellMatrix<T> build_adjoint_sell(const SparseMatrix<T, SparseFormat::Csr>& a, crd::memory::IAllocator* alloc)
    {
        auto at = transpose<T>(a, alloc); // Aᵀ (structural transpose; values not conjugated)
        // Conjugate values -> Aᴴ (identity for real T via spmv_conj).
        auto& vals = at.values().values;
        for (crd::usize k = 0; k < vals.size(); ++k)
        {
            vals[k] = detail::spmv_conj(vals[k]);
        }
        return to_sell<T>(at, alloc);
    }

    SellMatrix<T> m_a;  // A   (rows × cols)
    SellMatrix<T> m_ah; // Aᴴ  (cols × rows)
    detail::OperatorPick m_pick;
    crd::u32      m_rows;
    crd::u32      m_cols;
    bool          m_parallel = false;
};

} // namespace crd::hesap::sparse
