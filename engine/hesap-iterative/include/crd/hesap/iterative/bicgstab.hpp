#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_inner / krylov_mag / krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// BiCGSTAB -- stabilized BiConjugate Gradient (van der Vorst 1992). Phase 3.1.6 v4c.
//
// SHORT-recurrence Krylov solver for GENERAL (nonsymmetric / non-Hermitian) A --
// no growing basis (unlike GMRES), so it converges hard nonsymmetric systems
// (e.g. sherman3) where restarted GMRES(m) stagnates. Preconditioned (M⁻¹ applied
// to p and s). Real + complex.
//
// INNER-PRODUCT CONVENTION: all dots are CONJUGATED (Hermitian, `dotc`), including
// the shadow ⟨r̂₀,r⟩ -- matching Eigen's BiCGSTAB (`.adjoint()*`) and PETSc's
// complex `VecDot`, and keeping one inner-product policy across the module. The
// shadow residual r̂₀ = r₀ (deterministic; matches Eigen).
//
// BREAKDOWN: ρ, ⟨r̂₀,v⟩, ⟨t,t⟩, ω are tested against a LAPACK-style smlnum
// (|·| < min/eps) rather than == 0, so a near-breakdown at scale is reported as
// StopReason::Breakdown instead of silently diverging. Lucky breakdown (‖s‖
// already converged after the α-step) early-outs with x += α·p̂.
//
// Determinism moat: reductions are KBN-pairwise (blas1); the only parallel step is
// the operator's spmv (bit-exact across threads) ⇒ bit-identical solve across
// thread counts, gated like CG/GMRES. (Jacobi zero-diagonal fallback per
// feedback_iterative_crush_claim_same_algorithm.)
// -----------------------------------------------------------------------

template <typename T>
struct BicgstabWorkspace
{
    crd::hesap::dense::Vector<T> r, r0hat, p, phat, v, s, shat, t;

    BicgstabWorkspace(crd::memory::IAllocator* alloc, crd::usize n)
        : r(alloc, n), r0hat(alloc, n), p(alloc, n), phat(alloc, n), v(alloc, n), s(alloc, n), shat(alloc, n),
          t(alloc, n)
    {
    }

    [[nodiscard]] crd::usize size() const noexcept { return r.size(); }
};

template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> bicgstab(const crd::hesap::LinearOp<T>&  a,
                                                         const crd::hesap::LinearOp<T>*  m_inv,
                                                         crd::containers::ConstSpan<T>   b,
                                                         crd::containers::Span<T>        x,
                                                         const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                         BicgstabWorkspace<T>&           ws,
                                                         crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   n      = a.n_rows();
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "bicgstab: operator must be square");
    CRD_ASSERT_MSG(b.size() == n && x.size() == n && ws.size() == n, "bicgstab: span/workspace size mismatch");

    const auto r    = ws.r.span();
    const auto r0   = ws.r0hat.span();
    const auto p    = ws.p.span();
    const auto phat = ws.phat.span();
    const auto v    = ws.v.span();
    const auto s    = ws.s.span();
    const auto shat = ws.shat.span();
    const auto tv   = ws.t.span();

    // r = b - A·x ; r̂₀ = r
    (void)a.apply(x, tv);
    for (crd::usize i = 0; i < n; ++i)
    {
        r[i]  = b[i] - tv[i];
        r0[i] = r[i];
        p[i]  = T{};
        v[i]  = T{};
    }
    const R res0 = nrm2<T>(r);
    R       res  = res0;
    if (opts.record_residuals)
    {
        result.residual_history.push_back(res0);
    }
    if (is_converged<R>(res, res0, opts) || n == 0)
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = res;
        return result;
    }

    T rho = T(1), alpha = T(1), omega = T(1);

    for (crd::usize k = 1; k <= opts.max_iter; ++k)
    {
        const T rho_new = detail::krylov_inner<T>(r0, r); // ⟨r̂₀, r⟩
        if (detail::krylov_mag<T>(rho_new) < smlnum)
        {
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            return result;
        }
        const T beta = (rho_new / rho) * (alpha / omega);

        // p = r + beta·(p - omega·v)
        dense::axpy<T>(-omega, v, p); // p -= omega·v
        dense::scal<T>(beta, p);      // p *= beta
        dense::axpy<T>(T(1), r, p);   // p += r

        // p̂ = M⁻¹ p ; v = A p̂
        if (m_inv != nullptr)
        {
            (void)m_inv->apply(p, phat);
        }
        else
        {
            dense::copy<T>(p, phat);
        }
        (void)a.apply(phat, v);

        const T r0v = detail::krylov_inner<T>(r0, v); // ⟨r̂₀, v⟩
        if (detail::krylov_mag<T>(r0v) < smlnum)
        {
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            return result;
        }
        alpha = rho_new / r0v;

        // s = r - alpha·v
        dense::copy<T>(r, s);
        dense::axpy<T>(-alpha, v, s);

        const R snorm = nrm2<T>(s);
        if (is_converged<R>(snorm, res0, opts)) // lucky breakdown: x += alpha·p̂
        {
            dense::axpy<T>(alpha, phat, x);
            result.iterations          = k;
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = snorm;
            if (opts.record_residuals)
            {
                result.residual_history.push_back(snorm);
            }
            return result;
        }

        // ŝ = M⁻¹ s ; t = A ŝ
        if (m_inv != nullptr)
        {
            (void)m_inv->apply(s, shat);
        }
        else
        {
            dense::copy<T>(s, shat);
        }
        (void)a.apply(shat, tv);

        const T tt = detail::krylov_inner<T>(tv, tv); // ⟨t,t⟩ = ‖t‖² (real-positive)
        if (detail::krylov_mag<T>(tt) < smlnum)
        {
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            return result;
        }
        omega = detail::krylov_inner<T>(tv, s) / tt; // ⟨t,s⟩ / ⟨t,t⟩

        // x += alpha·p̂ + omega·ŝ ; r = s - omega·t
        dense::axpy<T>(alpha, phat, x);
        dense::axpy<T>(omega, shat, x);
        dense::copy<T>(s, r);
        dense::axpy<T>(-omega, tv, r);

        res               = nrm2<T>(r);
        result.iterations = k;
        if (opts.record_residuals)
        {
            result.residual_history.push_back(res);
        }
        if (is_converged<R>(res, res0, opts))
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = res;
            return result;
        }
        if (detail::krylov_mag<T>(omega) < smlnum)
        {
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            return result;
        }
        rho = rho_new;
    }

    result.reason              = StopReason::MaxIterations;
    result.final_residual_norm = res;
    return result;
}

} // namespace crd::hesap::iterative
