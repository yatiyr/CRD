#pragma once

// gmres_refine.hpp — v5f: GMRES-based iterative refinement (Carson-Higham) for a sparse DIRECT factor.
//
// The static-pivot LU (MC64 + GESP √ε perturbation, NO row interchanges — the determinism-moat design) is a
// POOR approximation of A on saddle-point / indefinite UNSYMMETRIC systems (2D-NS FEM, CFD/structural —
// garon2/raefsky3-class): tiny pivots get perturbed, so the factor drifts far from A and FIXED-POINT iterative
// refinement DIVERGES (the iteration-matrix spectral radius ≥ 1). FGMRES PRECONDITIONED by that same factor
// converges where fixed-point IR cannot — it builds a Krylov subspace rather than assuming the factor is a
// contraction. This is the textbook robust refinement (Carson & Higham 2017) and it reuses the v4 FGMRES,
// which is the determinism-moat solve (serial Arnoldi / Givens, parallel-spmv only ⇒ thread-count independent).
//
// Module edge: hesap-direct → hesap-iterative (ACYCLIC — iterative is a sibling that does not depend on
// direct; a refined direct solve composing Krylov machinery is as natural as its existing use of dense BLAS).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/factorization.hpp>
#include <crd/hesap/direct/mixed_refine.hpp> // csr_cast_copy — clone the owned residual matrix
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>
#include <type_traits>
#include <utility>

namespace crd::hesap::direct
{

// Tuning for the GMRES refinement loop. Defaults drive to f64 backward error; the restart bounds the
// Arnoldi memory ((restart+1)·n). max_iter caps the total inner iterations across restart cycles.
struct GmresRefineOptions
{
    crd::usize restart  = 60;
    crd::usize max_iter = 300;
};

// A right-preconditioner LinearOp<T> wrapping a direct factor's RAW apply_inverse: z = (factor)⁻¹·v.
// `Fac` = any IFactorization<T> (its const apply_inverse runs no inner IR — exactly the GMRES preconditioner).
template <typename T, typename Fac> class FactorPrecondOp final : public crd::hesap::LinearOp<T>
{
public:
    FactorPrecondOp(const Fac& f, crd::usize n) noexcept : m_f(&f), m_n(n) {}

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            y[i] = x[i];
        }
        m_f->apply_inverse({y.data(), m_n}, 1); // in-place; no internal refinement
        return true;
    }
    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    const Fac* m_f;
    crd::usize m_n;
};

// GMRES-refined direct solve: owns A (for the residual / spmv) + the cheap direct factor (the preconditioner).
// Drop-in IFactorization<T>: factor-once / solve-many, multi-RHS. `solve` returns false iff a column did not
// reach the backward-error target within max_iter (HONEST — no silent garbage on the genuinely hard systems).
template <typename T, typename Fac> class GmresRefinedSolve final : public IFactorization<T>
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>,
                  "GmresRefinedSolve: real working precision only (complex is a follow-on)");

public:
    GmresRefinedSolve(crd::memory::IAllocator* alloc, sparse::SparseMatrix<T, sparse::SparseFormat::Csr>&& a,
                      Fac&& fac, GmresRefineOptions opts = {}) noexcept
        : m_alloc(alloc), m_a(std::move(a)), m_fac(std::move(fac)), m_opts(opts)
    {
        m_n = static_cast<crd::usize>(m_a.rows());
    }

    [[nodiscard]] bool solve(crd::containers::Span<T> rhs, crd::usize nrhs) const override;
    using IFactorization<T>::solve; // un-hide the single-RHS convenience overload

    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] crd::u64 factor_nnz() const noexcept override { return m_fac.factor_nnz(); }
    [[nodiscard]] crd::usize info() const noexcept override { return m_fac.info(); }

    // GMRES iterations on the LAST solved column + whether it converged (diagnostics + the bench scoreboard).
    [[nodiscard]] crd::u32 last_iters() const noexcept { return m_last_iters; }
    [[nodiscard]] bool last_converged() const noexcept { return m_last_converged; }

    // The wrapped factor (const) — for the determinism moat test: its L/U are bit-identical across worker
    // counts, and FGMRES on top is deterministic, so the refined solution is bit-identical too.
    [[nodiscard]] const Fac& factor() const noexcept { return m_fac; }

private:
    crd::memory::IAllocator*                              m_alloc = nullptr;
    sparse::SparseMatrix<T, sparse::SparseFormat::Csr>    m_a; // owned matrix backing the residual / spmv
    Fac                                                   m_fac; // the cheap direct factor (preconditioner)
    GmresRefineOptions                                    m_opts;
    crd::usize                                            m_n = 0;
    mutable crd::u32                                      m_last_iters = 0;
    mutable bool                                          m_last_converged = false;
};

template <typename T, typename Fac>
bool GmresRefinedSolve<T, Fac>::solve(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    using R = crd::hesap::dense::RealType<T>;
    if (m_fac.info() != 0)
    {
        return false;
    }
    const sparse::SparseLinearOp<T>   aop(m_a);
    const FactorPrecondOp<T, Fac>     pre(m_fac, m_n);
    iterative::GmresWorkspace<T>      ws(m_alloc, m_n, m_opts.restart);
    iterative::IterativeOptions<R>    opts;
    opts.rel_tol  = static_cast<R>(64) * std::numeric_limits<R>::epsilon(); // f64 backward error
    opts.max_iter = m_opts.max_iter;

    crd::containers::Array<T> x(m_alloc);
    x.resize(m_n);

    bool      all_ok = true;
    crd::u32  iters  = 0;
    bool      conv   = true;
    for (crd::usize col = 0; col < nrhs; ++col)
    {
        T* const b = rhs.data() + col * m_n;
        for (crd::usize i = 0; i < m_n; ++i)
        {
            x[i] = T{0};
        }
        const auto res = iterative::fgmres<T>(aop, &pre, {b, m_n}, {x.data(), m_n}, opts, ws, m_alloc);
        for (crd::usize i = 0; i < m_n; ++i)
        {
            b[i] = x[i];
        }
        iters  = static_cast<crd::u32>(res.iterations);
        conv   = conv && res.converged;
        all_ok = all_ok && res.converged;
    }
    m_last_iters     = iters;
    m_last_converged = conv;
    return all_ok;
}

// Factor a general square unsymmetric A (CSR) with the static-pivot multifrontal LU and wrap it in GMRES-IR.
// The robust direct solve for the saddle-point / indefinite systems where the bare factor's fixed-point IR
// diverges (garon2-class). Returns an IFactorization<f64>. (raefsky3-class — where even GMRES-IR needs
// hundreds of iters — wants threshold partial pivoting in the front; tracked as the v5f follow-on.)
// `pivot_threshold > 0` (v5f-(a)) factors the preconditioner with within-front PARTIAL PIVOTING instead of
// static-diagonal — for the hardest saddle-point systems (raefsky3) where even GMRES-IR over the static factor
// needs hundreds of iterations, the partial-pivot factor is a far stronger preconditioner (GMRES then
// converges in a few iters). Default 0 ⇒ the static-factor preconditioner.
[[nodiscard]] inline GmresRefinedSolve<crd::f64, MultifrontalLU<crd::f64>>
factor_gmres_refined_lu(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr>& a,
                        crd::memory::IAllocator* alloc, crd::u32 num_workers = 1, GmresRefineOptions opts = {},
                        crd::f64 pivot_threshold = crd::f64(0))
{
    MultifrontalLU<crd::f64> fac =
        (pivot_threshold > crd::f64(0)) ? factor_multifrontal_lu_pp<crd::f64>(a, alloc, num_workers, pivot_threshold)
                                        : factor_multifrontal_lu<crd::f64>(a, alloc, num_workers);
    sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr> a_copy = csr_cast_copy<crd::f64>(alloc, a);
    return GmresRefinedSolve<crd::f64, MultifrontalLU<crd::f64>>(alloc, std::move(a_copy), std::move(fac), opts);
}

} // namespace crd::hesap::direct
