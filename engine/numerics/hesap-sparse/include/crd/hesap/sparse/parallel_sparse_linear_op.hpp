#pragma once

#include <crd/core/assert.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/sell.hpp>
#include <crd/hesap/sparse/sell_parallel.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spmv.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::sparse
{
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
// Requires crd::jobs to be initialised by the caller. Non-square / transpose
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
    // (the v1b spmv regime, confirmed by the CG-vs-Eigen bench). Default ≈ L2.
    explicit ParallelSparseLinearOp(const SparseMatrix<T, SparseFormat::Csr>& matrix, crd::memory::IAllocator* alloc,
                                    crd::usize parallel_min_stored_bytes = (crd::usize{1} << 21))
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/false), m_sell(to_sell<T>(matrix, alloc))
    {
        CRD_ASSERT_MSG(matrix.rows() == matrix.cols(), "ParallelSparseLinearOp: matrix must be square");
        m_parallel = m_sell.stored() * sizeof(T) >= parallel_min_stored_bytes;
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        if (!m_parallel)
        {
            spmv_sell<T>(T(1), m_sell, x, T(0), y); // sub-cache: serial wins (no dispatch overhead)
            return true;
        }
        spmv_sell_parallel<T>(T(1), m_sell, x, T(0), y);
        // parallel_for bump-allocates JobDecls from the per-thread frame arena
        // (reclaimed only on frame_reset). A Krylov solve calls apply thousands
        // of times, so reclaim here -- the operator owns its jobs scratch.
        // Contract: apply must NOT be called nested inside another frame-arena
        // consumer (e.g. from within a parallel_for body). CG/PCG call it from
        // the main thread, serially, which is the supported usage.
        crd::jobs::frame_reset();
        return true;
    }

    // Whether apply uses the parallel path (working set exceeded the threshold).
    [[nodiscard]] bool is_parallel() const noexcept { return m_parallel; }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_sell.rows; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_sell.cols; }

private:
    SellMatrix<T> m_sell;
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
                                        crd::usize parallel_min_stored_bytes = (crd::usize{1} << 21))
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_a(to_sell<T>(a, alloc))
        , m_ah(build_adjoint_sell(a, alloc))
        , m_rows(a.rows())
        , m_cols(a.cols())
    {
        const crd::usize bytes = (m_a.stored() > m_ah.stored() ? m_a.stored() : m_ah.stored()) * sizeof(T);
        m_parallel             = bytes >= parallel_min_stored_bytes;
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        spmv_dispatch(m_a, x, y);
        return true;
    }

    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        spmv_dispatch(m_ah, x, y);
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

    void spmv_dispatch(const SellMatrix<T>& s, crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const
    {
        if (!m_parallel)
        {
            spmv_sell<T>(T(1), s, x, T(0), y);
            return;
        }
        spmv_sell_parallel<T>(T(1), s, x, T(0), y);
        crd::jobs::frame_reset(); // Krylov-loop arena hygiene (see ParallelSparseLinearOp).
    }

    SellMatrix<T> m_a;  // A   (rows × cols)
    SellMatrix<T> m_ah; // Aᴴ  (cols × rows)
    crd::u32      m_rows;
    crd::u32      m_cols;
    bool          m_parallel = false;
};

} // namespace crd::hesap::sparse
