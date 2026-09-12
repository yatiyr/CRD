#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/block_cg.hpp> // detail::block_gram / block_gemm_update / block_lu_solve
#include <crd/hesap/iterative/cg.hpp>       // detail::krylov_inner / krylov_mag / krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// Block-BiCGSTAB -- block stabilized BiConjugate Gradient (El Guennouni-Jbilou-Sadok
// 2003). Phase 3.1.6 v4f-3.
//
// SHORT-recurrence block-Krylov solver for GENERAL (nonsymmetric / non-Hermitian) A,
// s right-hand sides at once (X, B are n×s ROW-MAJOR). No growing basis (unlike block-
// GMRES) ⇒ converges hard nonsymmetric multi-RHS systems where restarted block-GMRES(m)
// stagnates, with ONE block spmm per A-application for all s RHS. Eigen ships no block-
// BiCGSTAB → breadth + the A-pass-reuse / shared-Krylov win over per-column BiCGSTAB.
//
// The s×s coefficients α, β use the GENERAL (non-Hermitian) s×s solve (block_lu_solve;
// R̃₀ᴴAP is NOT SPD — Cholesky would be wrong), with the SAME matrix M = R̃₀ᴴAP reused
// for both. The steering parameter ω is SCALAR (Frobenius ⟨AS,S⟩/⟨AS,AS⟩) — what makes
// Bl-BiCGSTAB work. β = (1/ω)·M⁻¹·(R̃₀ᴴR_new) reduces to scalar BiCGSTAB at s=1. Shadow
// R̃₀ = R₀ (deterministic; matches the scalar bicgstab convention). Right-preconditioned
// (M_inv applied to the search blocks P, S). Real + complex (dotc throughout).
//
// Determinism: block_gram / block_lu_solve / the Frobenius dots all run SERIALLY on the
// calling thread; only the operator's block spmm is parallel (bit-exact) ⇒ the whole
// solve is thread-count independent (the v4 determinism moat).
// -----------------------------------------------------------------------

template <typename T>
struct BlockBicgstabWorkspace
{
    crd::usize n;
    crd::u32   s;
    crd::hesap::dense::Vector<T> rblk, r0hat, pblk, phat, apblk, sblk, shat, asblk, ptmp; // n·s each
    crd::containers::Array<T>    mmat, mwrk, alpha, beta, rr;                              // s·s each

    BlockBicgstabWorkspace(crd::memory::IAllocator* alloc, crd::usize size, crd::u32 nrhs)
        : n(size), s(nrhs)
        , rblk(alloc, size * nrhs), r0hat(alloc, size * nrhs), pblk(alloc, size * nrhs), phat(alloc, size * nrhs)
        , apblk(alloc, size * nrhs), sblk(alloc, size * nrhs), shat(alloc, size * nrhs), asblk(alloc, size * nrhs)
        , ptmp(alloc, size * nrhs), mmat(alloc), mwrk(alloc), alpha(alloc), beta(alloc), rr(alloc)
    {
        CRD_ASSERT_MSG(nrhs >= 1, "BlockBicgstabWorkspace: nrhs must be >= 1");
        const crd::usize ss = static_cast<crd::usize>(nrhs) * nrhs;
        mmat.resize(ss);
        mwrk.resize(ss);
        alpha.resize(ss);
        beta.resize(ss);
        rr.resize(ss);
    }
};

namespace detail
{
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> block_bicgstab_impl(
    const crd::hesap::sparse::BlockLinearOp<T>&             a,
    const crd::hesap::sparse::BlockLinearOp<T>*             m_inv,
    crd::containers::ConstSpan<T>                           b,
    crd::containers::Span<T>                                x,
    const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
    BlockBicgstabWorkspace<T>&                              ws,
    crd::memory::IAllocator*                                result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   n      = a.n_rows();
    const crd::u32     s      = ws.s;
    const crd::usize   ns     = n * s;
    const crd::usize   ss     = static_cast<crd::usize>(s) * s;
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "block_bicgstab: operator must be square");
    CRD_ASSERT_MSG(b.size() == ns && x.size() == ns, "block_bicgstab: B/X must be n×s row-major");

    T* Rb   = ws.rblk.data();
    T* R0   = ws.r0hat.data();
    T* P    = ws.pblk.data();
    T* Phat = ws.phat.data();
    T* AP   = ws.apblk.data();
    T* S    = ws.sblk.data();
    T* Shat = ws.shat.data();
    T* AS   = ws.asblk.data();
    T* Pt   = ws.ptmp.data();

    // R = B - A·X ; R̃₀ = R ; P = R.
    (void)a.apply_block(x, s, crd::containers::Span<T>{AP, ns}, s, s); // AP = A·X (scratch)
    for (crd::usize i = 0; i < ns; ++i)
    {
        Rb[i] = b[i] - AP[i];
        R0[i] = Rb[i];
        P[i]  = Rb[i];
    }
    crd::containers::Array<R> bnorm(result_alloc);
    bnorm.resize(s);
    for (crd::u32 l = 0; l < s; ++l)
    {
        R acc = R(0);
        for (crd::usize k = 0; k < n; ++k) { const R mg = detail::krylov_mag<T>(b[k * s + l]); acc += mg * mg; }
        bnorm[l] = std::sqrt(acc) + smlnum;
    }
    auto worst_rel = [&](const T* r) -> R {
        R worst = R(0);
        for (crd::u32 l = 0; l < s; ++l)
        {
            R acc = R(0);
            for (crd::usize k = 0; k < n; ++k) { const R mg = detail::krylov_mag<T>(r[k * s + l]); acc += mg * mg; }
            worst = std::max(worst, std::sqrt(acc) / bnorm[l]);
        }
        return worst;
    };
    auto solve_M = [&](const T* m, T* rhs) { // rhs ← M⁻¹ rhs (M preserved via working copy)
        for (crd::usize i = 0; i < ss; ++i) { ws.mwrk[i] = m[i]; }
        detail::block_lu_solve<T>(ws.mwrk.data(), s, rhs, s);
    };

    R res = worst_rel(Rb);
    if (opts.record_residuals) { result.residual_history.push_back(res); }
    if (res <= opts.rel_tol || n == 0)
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = res;
        return result;
    }

    for (crd::usize k = 1; k <= opts.max_iter; ++k)
    {
        // P̂ = M⁻¹ P ; AP = A P̂.
        if (m_inv != nullptr) { (void)m_inv->apply_block(crd::containers::ConstSpan<T>{P, ns}, s, crd::containers::Span<T>{Phat, ns}, s, s); }
        else { for (crd::usize i = 0; i < ns; ++i) { Phat[i] = P[i]; } }
        (void)a.apply_block(crd::containers::ConstSpan<T>{Phat, ns}, s, crd::containers::Span<T>{AP, ns}, s, s);

        // M = R̃₀ᴴ AP ; α = M⁻¹ (R̃₀ᴴ R).
        detail::block_gram<T>(R0, AP, n, s, ws.mmat.data());
        detail::block_gram<T>(R0, Rb, n, s, ws.alpha.data());
        solve_M(ws.mmat.data(), ws.alpha.data());

        // S = R − AP·α.
        for (crd::usize i = 0; i < ns; ++i) { S[i] = Rb[i]; }
        detail::block_gemm_update<T>(AP, ws.alpha.data(), n, s, S, -1);

        // Lucky breakdown: S already converged ⇒ X += P̂·α.
        if (worst_rel(S) <= opts.rel_tol)
        {
            detail::block_gemm_update<T>(Phat, ws.alpha.data(), n, s, x.data(), +1);
            result.iterations          = k;
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = worst_rel(S);
            return result;
        }

        // Ŝ = M⁻¹ S ; AS = A Ŝ.
        if (m_inv != nullptr) { (void)m_inv->apply_block(crd::containers::ConstSpan<T>{S, ns}, s, crd::containers::Span<T>{Shat, ns}, s, s); }
        else { for (crd::usize i = 0; i < ns; ++i) { Shat[i] = S[i]; } }
        (void)a.apply_block(crd::containers::ConstSpan<T>{Shat, ns}, s, crd::containers::Span<T>{AS, ns}, s, s);

        // ω = ⟨AS, S⟩_F / ⟨AS, AS⟩_F  (scalar Frobenius dots).
        const T tt = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{AS, ns}, crd::containers::ConstSpan<T>{AS, ns});
        if (detail::krylov_mag<T>(tt) < smlnum)
        {
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            return result;
        }
        const T omega = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{AS, ns}, crd::containers::ConstSpan<T>{S, ns}) / tt;

        // X += P̂·α + ω·Ŝ ; R_new = S − ω·AS.
        detail::block_gemm_update<T>(Phat, ws.alpha.data(), n, s, x.data(), +1);
        dense::axpy<T>(omega, crd::containers::ConstSpan<T>{Shat, ns}, crd::containers::Span<T>{x.data(), ns});
        for (crd::usize i = 0; i < ns; ++i) { Rb[i] = S[i]; }
        dense::axpy<T>(-omega, crd::containers::ConstSpan<T>{AS, ns}, crd::containers::Span<T>{Rb, ns});

        res               = worst_rel(Rb);
        result.iterations = k;
        if (opts.record_residuals) { result.residual_history.push_back(res); }
        if (res <= opts.rel_tol)
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = res;
            return result;
        }
        // Divergence guard: BiCGSTAB is non-monotone and erratic on hard nonsym A; the
        // block ω-via-Frobenius amplifies instability across all s RHS. Bail before the
        // relative residual overflows f64 (→ Inf/NaN contaminating the next step).
        if (res > R(1e10))
        {
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            return result;
        }
        if (detail::krylov_mag<T>(omega) < smlnum)
        {
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            return result;
        }

        // β = (1/ω)·M⁻¹·(R̃₀ᴴ R_new) ; P_new = R_new + (P − ω·AP)·β.
        detail::block_gram<T>(R0, Rb, n, s, ws.beta.data()); // R̃₀ᴴ R_new
        solve_M(ws.mmat.data(), ws.beta.data());             // M⁻¹ (R̃₀ᴴ R_new)
        const T inv_omega = T(1) / omega;
        for (crd::usize i = 0; i < ss; ++i) { ws.beta[i] = ws.beta[i] * inv_omega; }
        for (crd::usize i = 0; i < ns; ++i) { Pt[i] = P[i]; }
        dense::axpy<T>(-omega, crd::containers::ConstSpan<T>{AP, ns}, crd::containers::Span<T>{Pt, ns}); // Pt = P − ω·AP
        for (crd::usize i = 0; i < ns; ++i) { P[i] = Rb[i]; }                                            // P_new = R_new ...
        detail::block_gemm_update<T>(Pt, ws.beta.data(), n, s, P, +1);                                   // ... + (P−ω·AP)·β
    }

    result.reason              = StopReason::MaxIterations;
    result.final_residual_norm = res;
    return result;
}
} // namespace detail

// Block-BiCGSTAB (unpreconditioned).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> block_bicgstab(const crd::hesap::sparse::BlockLinearOp<T>& a,
                                                               crd::containers::ConstSpan<T>               b,
                                                               crd::containers::Span<T>                    x,
                                                               const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                               BlockBicgstabWorkspace<T>&                  ws,
                                                               crd::memory::IAllocator*                    result_alloc)
{
    return detail::block_bicgstab_impl<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

// Preconditioned block-BiCGSTAB (right preconditioner via M⁻¹ block apply on P, S).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> block_pbicgstab(const crd::hesap::sparse::BlockLinearOp<T>& a,
                                                                const crd::hesap::sparse::BlockLinearOp<T>& m_inv,
                                                                crd::containers::ConstSpan<T>               b,
                                                                crd::containers::Span<T>                    x,
                                                                const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                                BlockBicgstabWorkspace<T>&                  ws,
                                                                crd::memory::IAllocator*                    result_alloc)
{
    return detail::block_bicgstab_impl<T>(a, &m_inv, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
