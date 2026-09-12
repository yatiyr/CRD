#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// LSQR -- least-squares QR (Paige-Saunders 1982). Phase 3.1.6 v4d-1.
//
// Solves min ‖A x − b‖₂ for a GENERAL RECTANGULAR (m×n) A via the Golub-Kahan
// bidiagonalization (needs both A·x and Aᴴ·y -- consume a LinearOp with
// apply + apply_adjoint, e.g. ParallelSpmvLeastSquaresOp). Analytically
// equivalent to CG on the normal equations AᴴA x = Aᴴb but WITHOUT forming
// them (so it does not square the condition number -- the whole point; CG on
// the normal equations diverges on ill-conditioned least-squares where LSQR
// converges). Eigen ships no LSQR (its LeastSquaresConjugateGradient IS the
// normal-equations CG that LSQR beats) -- breadth + the determinism moat.
//
// Two stopping tests (Paige-Saunders): consistent ‖r‖ ≤ tol·‖b‖, and
// least-squares ‖Aᴴr‖ ≤ tol·‖A‖·‖r‖. Determinism: reductions KBN-pairwise,
// both spmv directions bit-exact across threads ⇒ thread-count-independent.
// Recurrence transcribed from Krylov.jl `lsqr.jl`.
// -----------------------------------------------------------------------

template <typename T>
struct LsqrWorkspace
{
    crd::hesap::dense::Vector<T> u, av;       // size m (rows): residual-space
    crd::hesap::dense::Vector<T> v, w, atu, nv; // size n (cols): solution-space (nv = residual-space v under N-precond)

    LsqrWorkspace(crd::memory::IAllocator* alloc, crd::usize m, crd::usize n)
        : u(alloc, m), av(alloc, m), v(alloc, n), w(alloc, n), atu(alloc, n), nv(alloc, n)
    {
    }

    [[nodiscard]] crd::usize rows() const noexcept { return u.size(); }
    [[nodiscard]] crd::usize cols() const noexcept { return v.size(); }
};

// Optional N preconditioner (SPD/HPD, n-space column preconditioner -- the
// standard least-squares preconditioner): the bidiagonalization v-side becomes
// the N-inner-product (v = N⁻¹·(residual-space nv), α = √Re⟨v, nv⟩); the solution
// is built directly from the N-preconditioned v (no untransform). nullptr ⇒
// bit-identical to plain LSQR (nv aliases v, α = nrm2).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> lsqr(const crd::hesap::LinearOp<T>&  a,
                                                     const crd::hesap::LinearOp<T>*  n_inv,
                                                     crd::containers::ConstSpan<T>   b,
                                                     crd::containers::Span<T>        x,
                                                     const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                     LsqrWorkspace<T>&               ws,
                                                     crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   m      = a.n_rows();
    [[maybe_unused]] const crd::usize n = a.n_cols(); // used only in asserts (compiled out in release)
    const bool         prec   = (n_inv != nullptr);
    CRD_ASSERT_MSG(b.size() == m && x.size() == n, "lsqr: b must be rows(), x must be cols()");
    CRD_ASSERT_MSG(ws.rows() == m && ws.cols() == n, "lsqr: workspace size mismatch");
    CRD_ASSERT_MSG(a.has_adjoint(), "lsqr: operator must provide apply_adjoint (Aᴴ)");

    const auto u   = ws.u.span();
    const auto av  = ws.av.span();
    const auto v   = ws.v.span();
    const auto w   = ws.w.span();
    const auto atu = ws.atu.span();
    const auto nv  = prec ? ws.nv.span() : v; // residual-space Aᴴu; aliases v when unpreconditioned

    // N-inner-product norm of the residual-space vector held in `nv` (= nrm2 when N=I).
    auto vnorm = [&]() -> R {
        if (prec)
        {
            return std::sqrt(detail::krylov_real<T>(detail::krylov_inner<T>(v, nv)));
        }
        return nrm2<T>(v);
    };

    // ---- Golub-Kahan setup ----
    (void)a.apply(x, av); // av = A x0
    for (crd::usize i = 0; i < m; ++i)
    {
        u[i] = b[i] - av[i]; // u = r0
    }
    const R beta1 = nrm2<T>(u);
    R       res   = beta1;
    if (opts.record_residuals)
    {
        result.residual_history.push_back(beta1);
    }
    if (beta1 < smlnum) // x0 already solves it exactly.
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = R(0);
        return result;
    }
    dense::scal<T>(T(R(1) / beta1), u);

    (void)a.apply_adjoint(u, nv); // nv = Aᴴ u (residual-space)
    if (prec)
    {
        (void)n_inv->apply(nv, v); // v = N⁻¹ nv
    }
    R alpha = vnorm();
    if (alpha < smlnum) // Aᴴr0 = 0: x0 is already the least-squares solution.
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = beta1;
        return result;
    }
    dense::scal<T>(T(R(1) / alpha), v);
    if (prec)
    {
        dense::scal<T>(T(R(1) / alpha), nv);
    }
    dense::copy<T>(v, w);

    R phibar = beta1;
    R rhobar = alpha;
    R anorm2 = alpha * alpha;

    for (crd::usize k = 1; k <= opts.max_iter; ++k)
    {
        // Bidiagonalization: u = (A v - alpha u)/beta ; v = N⁻¹(Aᴴ u - beta nv)/alpha.
        (void)a.apply(v, av);
        dense::axpy<T>(-T(alpha), u, av); // av = A v - alpha u
        R beta = nrm2<T>(av);
        if (beta >= smlnum)
        {
            dense::copy<T>(av, u);
            dense::scal<T>(T(R(1) / beta), u);
        }
        (void)a.apply_adjoint(u, atu);
        dense::axpy<T>(-T(beta), nv, atu); // atu = Aᴴ u - beta nv (residual-space)
        dense::copy<T>(atu, nv);           // nv = new residual-space v (unnormalized)
        if (prec)
        {
            (void)n_inv->apply(nv, v); // v = N⁻¹ nv
        }
        alpha = vnorm();
        if (alpha >= smlnum)
        {
            dense::scal<T>(T(R(1) / alpha), v);
            if (prec)
            {
                dense::scal<T>(T(R(1) / alpha), nv);
            }
        }

        // Givens rotation eliminating beta.
        R rho = std::sqrt(rhobar * rhobar + beta * beta);
        if (rho < smlnum)
        {
            rho = smlnum;
        }
        const R c     = rhobar / rho;
        const R s     = beta / rho;
        const R phi   = c * phibar;
        phibar        = s * phibar;
        const R theta = s * alpha;
        rhobar        = -c * alpha;

        // Solution update: x += (phi/rho) w ; w = v - (theta/rho) w.
        dense::axpy<T>(T(phi / rho), w, x);
        dense::scal<T>(T(-theta / rho), w);
        dense::axpy<T>(T(1), v, w);

        anorm2 += alpha * alpha + beta * beta;
        const R anorm  = std::sqrt(anorm2);
        const R rnorm  = phibar;                              // ‖r‖
        const R arnorm = phibar * alpha * (c < R(0) ? -c : c); // ‖Aᴴr‖ estimate

        res               = rnorm;
        result.iterations = k;
        if (opts.record_residuals)
        {
            result.residual_history.push_back(rnorm);
        }
        // Consistent OR least-squares convergence.
        const bool consistent = rnorm <= opts.rel_tol * beta1 || rnorm < opts.abs_tol;
        const bool least_sq    = arnorm <= opts.rel_tol * anorm * rnorm;
        if (consistent || least_sq)
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = rnorm;
            return result;
        }
    }

    result.reason              = StopReason::MaxIterations;
    result.final_residual_norm = res;
    return result;
}

// Plain (unpreconditioned) LSQR convenience overload.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> lsqr(const crd::hesap::LinearOp<T>&                          a,
                                                     crd::containers::ConstSpan<T>                          b,
                                                     crd::containers::Span<T>                               x,
                                                     const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                     LsqrWorkspace<T>&                                      ws,
                                                     crd::memory::IAllocator* result_alloc)
{
    return lsqr<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
