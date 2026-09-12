#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_inner / krylov_real / krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// MINRES -- Minimal RESidual (Paige-Saunders 1975). Phase 3.1.6 v4c-2a.
//
// For SYMMETRIC / HERMITIAN (possibly INDEFINITE) A: minimizes ‖b − A x‖₂ over
// the Krylov space via the symmetric Lanczos 3-term recurrence + an incremental
// Givens QR of the tridiagonal + a short (3-term) w-recurrence solution update --
// O(1) vector storage (6 vectors), unlike GMRES. CG diverges on indefinite A;
// MINRES converges, which is the regime that motivates it.
//
// HERMITIAN CONTRACT: the Lanczos tridiagonal of a Hermitian A is REAL symmetric
// tridiagonal -- alpha = Re(vᴴ A v) (signed real; see detail::krylov_real) and
// beta = ‖·‖ (real). So the Givens/QR bookkeeping is all REAL even for complex A;
// only the Lanczos vectors + solution are T. Determinism moat: reductions are
// KBN-pairwise (blas1), the only parallel step is the operator's spmv (bit-exact
// across threads) ⇒ bit-identical solve across thread counts, gated like CG.
//
// Optional preconditioner M (SPD/HPD ONLY -- it must preserve the symmetric
// structure): the M-inner-product Lanczos tracks both the residual-space vector
// v and the preconditioned z = M⁻¹v, applies M⁻¹ once per iteration, uses the
// M⁻¹-norm beta = sqrt(Re(⟨p, M⁻¹p⟩)), and builds the solution from z. With
// m_inv == nullptr the path is bit-identical to plain MINRES (z aliases v,
// beta = nrm2). SYMMLQ is v4c-2b.
// -----------------------------------------------------------------------

template <typename T>
struct MinresWorkspace
{
    // v*: residual-space Lanczos vectors; z*: preconditioned (M⁻¹v); y: M⁻¹p; w*: solution dirs.
    crd::hesap::dense::Vector<T> v_old, v, v_new, w, w_old, p, z_old, z, z_new, y;

    MinresWorkspace(crd::memory::IAllocator* alloc, crd::usize n)
        : v_old(alloc, n), v(alloc, n), v_new(alloc, n), w(alloc, n), w_old(alloc, n), p(alloc, n), z_old(alloc, n),
          z(alloc, n), z_new(alloc, n), y(alloc, n)
    {
    }

    [[nodiscard]] crd::usize size() const noexcept { return v.size(); }
};

// Preconditioned MINRES. `m_inv` (SPD/HPD) applies z = M⁻¹ r; nullptr = plain MINRES.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> minres(const crd::hesap::LinearOp<T>&  a,
                                                       const crd::hesap::LinearOp<T>*  m_inv,
                                                       crd::containers::ConstSpan<T>   b,
                                                       crd::containers::Span<T>        x,
                                                       const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                       MinresWorkspace<T>&             ws,
                                                       crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   n      = a.n_rows();
    const bool         prec   = (m_inv != nullptr);
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "minres: operator must be square");
    CRD_ASSERT_MSG(b.size() == n && x.size() == n && ws.size() == n, "minres: span/workspace size mismatch");

    const auto v_old = ws.v_old.span();
    const auto v     = ws.v.span();
    const auto v_new = ws.v_new.span();
    const auto w     = ws.w.span();
    const auto w_old = ws.w_old.span();
    const auto p     = ws.p.span();
    // z* alias v* when unpreconditioned (z == M⁻¹v == v for M = I).
    const auto z_old = prec ? ws.z_old.span() : v_old;
    const auto z     = prec ? ws.z.span() : v;
    const auto z_new = prec ? ws.z_new.span() : v_new;

    // r0 = b - A x  (into v) ; M⁻¹-norm beta1 ; v1 = r0/beta1, z1 = M⁻¹r0/beta1.
    (void)a.apply(x, p);
    for (crd::usize i = 0; i < n; ++i)
    {
        v[i]     = b[i] - p[i];
        v_old[i] = T{};
        w[i]     = T{};
        w_old[i] = T{};
    }
    R beta1;
    if (prec)
    {
        (void)m_inv->apply(v, z); // z = M⁻¹ r0
        beta1 = std::sqrt(detail::krylov_real<T>(detail::krylov_inner<T>(v, z)));
    }
    else
    {
        beta1 = nrm2<T>(v);
    }
    R res = beta1;
    if (opts.record_residuals)
    {
        result.residual_history.push_back(beta1);
    }
    if (is_converged<R>(res, beta1, opts) || n == 0 || beta1 < smlnum)
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = res;
        return result;
    }
    dense::scal<T>(T(R(1) / beta1), v);
    if (prec)
    {
        dense::scal<T>(T(R(1) / beta1), z);
    }

    R beta_prev = R(0);
    R dbar = R(0), epsln = R(0), phibar = beta1;
    R cs = R(-1), sn = R(0);

    for (crd::usize k = 1; k <= opts.max_iter; ++k)
    {
        // Lanczos (M-inner-product): p = A z - alpha v - beta_prev v_old ; alpha = Re(⟨z, A z⟩).
        (void)a.apply(z, p);
        const R alpha = detail::krylov_real<T>(detail::krylov_inner<T>(z, p));
        dense::axpy<T>(-T(alpha), v, p);
        if (k > 1)
        {
            dense::axpy<T>(-T(beta_prev), v_old, p);
        }
        R beta_new;
        if (prec)
        {
            (void)m_inv->apply(p, ws.y.span()); // y = M⁻¹ p
            beta_new = std::sqrt(detail::krylov_real<T>(detail::krylov_inner<T>(p, ws.y.span())));
        }
        else
        {
            beta_new = nrm2<T>(p);
        }
        const bool lucky = beta_new < smlnum;
        if (!lucky)
        {
            dense::copy<T>(p, v_new);
            dense::scal<T>(T(R(1) / beta_new), v_new);
            if (prec)
            {
                dense::copy<T>(ws.y.span(), z_new);
                dense::scal<T>(T(R(1) / beta_new), z_new);
            }
        }

        const R oldeps = epsln;
        const R delta  = cs * dbar + sn * alpha;
        const R gbar   = sn * dbar - cs * alpha;
        epsln          = sn * beta_new;
        dbar           = -cs * beta_new;
        R gamma        = std::sqrt(gbar * gbar + beta_new * beta_new);
        if (gamma < smlnum)
        {
            gamma = smlnum;
        }
        cs          = gbar / gamma;
        sn          = beta_new / gamma;
        const R phi = cs * phibar;
        phibar      = sn * phibar;

        // w_new = (z - oldeps·w_old - delta·w) / gamma  (preconditioned direction; reuse p).
        dense::copy<T>(z, p);
        dense::axpy<T>(-T(oldeps), w_old, p);
        dense::axpy<T>(-T(delta), w, p);
        dense::scal<T>(T(R(1) / gamma), p);
        dense::axpy<T>(T(phi), p, x); // x += phi · w_new

        // Shuffle solution-direction + Lanczos (+ preconditioned) vectors.
        dense::copy<T>(w, w_old);
        dense::copy<T>(p, w);
        dense::copy<T>(v, v_old);
        if (!lucky)
        {
            dense::copy<T>(v_new, v);
        }
        if (prec)
        {
            dense::copy<T>(z, z_old);
            if (!lucky)
            {
                dense::copy<T>(z_new, z);
            }
        }
        beta_prev = beta_new;

        res               = phibar < R(0) ? -phibar : phibar;
        result.iterations = k;
        if (opts.record_residuals)
        {
            result.residual_history.push_back(res);
        }
        if (is_converged<R>(res, beta1, opts) || lucky)
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = res;
            return result;
        }
    }

    result.reason              = StopReason::MaxIterations;
    result.final_residual_norm = res;
    return result;
}

// Plain (unpreconditioned) MINRES convenience overload.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> minres(const crd::hesap::LinearOp<T>&                          a,
                                                       crd::containers::ConstSpan<T>                          b,
                                                       crd::containers::Span<T>                               x,
                                                       const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                       MinresWorkspace<T>&                                    ws,
                                                       crd::memory::IAllocator* result_alloc)
{
    return minres<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
