#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_inner / krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// GCR(m) -- restarted Generalized Conjugate Residual (Eisenstat-Elman-Schultz
// 1983). Phase 3.1.6 v4e-1. The OPTIMAL-residual projection substrate that
// GCRO-DR (v4e-2) and cross-solve recycling (v4e-3) sit on.
//
// For GENERAL (nonsymmetric / non-Hermitian) A. Maintains an orthonormal basis
// C = A·U (cᵢᴴcⱼ = δᵢⱼ) of the search space; each step adds a direction (seeded by
// the current residual, MGS-orthogonalized so the new A·p ⊥ the prior C), then does
// the minimal-residual correction x ← x + ⟨c,r⟩·u, r ← r − ⟨c,r⟩·c. Mathematically
// equivalent to GMRES(m) in exact arithmetic, but it stores U and C explicitly --
// which is precisely the recycle space GCRO-DR deflates and reuses. Restarts every
// m directions (C/U discarded; the warm residual carries over).
//
// PRECONDITIONING: optional RIGHT preconditioner N = m_inv (the search direction is
// seeded with N⁻¹·r, so the update stays in the original variable and r is the TRUE
// residual). Unpreconditioned (m_inv == nullptr) seeds with r directly.
//
// Determinism: every inner product is the KBN-pairwise dotc (detail::krylov_inner);
// the only parallel step is the operator's spmv (bit-exact across threads) ⇒
// bit-identical solve across thread counts (the v4 moat), gated like CG/GMRES.
// -----------------------------------------------------------------------

template <typename T>
struct GcrWorkspace
{
    crd::usize                   n;
    crd::usize                   m; // restart length (max stored directions)
    crd::hesap::dense::Vector<T> cbuf; // m·n  -- C = A·U, orthonormal
    crd::hesap::dense::Vector<T> ubuf; // m·n  -- search directions U
    crd::hesap::dense::Vector<T> r, p, q, mp; // n each (residual, seed dir, A·p, precond scratch)

    GcrWorkspace(crd::memory::IAllocator* alloc, crd::usize size, crd::usize restart)
        : n(size), m(restart), cbuf(alloc, restart * size), ubuf(alloc, restart * size), r(alloc, size), p(alloc, size),
          q(alloc, size), mp(alloc, size)
    {
        CRD_ASSERT_MSG(restart >= 1, "GcrWorkspace: restart length must be >= 1");
    }

    [[nodiscard]] crd::usize size() const noexcept { return n; }
    [[nodiscard]] crd::containers::Span<T> c(crd::usize j) noexcept { return {cbuf.data() + j * n, n}; }
    [[nodiscard]] crd::containers::Span<T> u(crd::usize j) noexcept { return {ubuf.data() + j * n, n}; }
};

// GCR(m) with optional RIGHT preconditioner N = m_inv (nullptr ⇒ plain GCR).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> gcr(const crd::hesap::LinearOp<T>&  a,
                                                    const crd::hesap::LinearOp<T>*  m_inv,
                                                    crd::containers::ConstSpan<T>   b,
                                                    crd::containers::Span<T>        x,
                                                    const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                    GcrWorkspace<T>&                ws,
                                                    crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   n      = a.n_rows();
    const crd::usize   m      = ws.m;
    const bool         prec   = (m_inv != nullptr);
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "gcr: operator must be square");
    CRD_ASSERT_MSG(b.size() == n && x.size() == n && ws.size() == n, "gcr: span/workspace size mismatch");

    const auto r  = ws.r.span();
    const auto p  = ws.p.span();
    const auto q  = ws.q.span();
    const auto mp = ws.mp.span();

    // r = b - A·x
    (void)a.apply(x, q);
    for (crd::usize i = 0; i < n; ++i)
    {
        r[i] = b[i] - q[i];
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

    while (result.iterations < opts.max_iter)
    {
        for (crd::usize k = 0; k < m && result.iterations < opts.max_iter; ++k)
        {
            // Seed the new direction with the (right-preconditioned) residual.
            if (prec)
            {
                (void)m_inv->apply(crd::containers::ConstSpan<T>{r.data(), n}, mp);
                dense::copy<T>(crd::containers::ConstSpan<T>{mp.data(), n}, p);
            }
            else
            {
                dense::copy<T>(crd::containers::ConstSpan<T>{r.data(), n}, p);
            }
            (void)a.apply(crd::containers::ConstSpan<T>{p.data(), n}, q); // q = A·p

            // MGS: orthogonalize q against c_0..c_{k-1}; mirror the same combination on p.
            for (crd::usize i = 0; i < k; ++i)
            {
                const T beta = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{ws.c(i).data(), n},
                                                       crd::containers::ConstSpan<T>{q.data(), n}); // ⟨c_i, q⟩
                dense::axpy<T>(-beta, crd::containers::ConstSpan<T>{ws.c(i).data(), n}, q);
                dense::axpy<T>(-beta, crd::containers::ConstSpan<T>{ws.u(i).data(), n}, p);
            }

            const R nrm = nrm2<T>(q);
            if (nrm < smlnum)
            {
                result.reason              = StopReason::Breakdown; // A·p in range(C): no further progress
                result.final_residual_norm = res;
                return result;
            }
            const T inv = T(R(1) / nrm);
            auto    ck  = ws.c(k);
            auto    uk  = ws.u(k);
            for (crd::usize i = 0; i < n; ++i)
            {
                ck[i] = q[i] * inv;
                uk[i] = p[i] * inv;
            }

            // Minimal-residual correction: alpha = ⟨c_k, r⟩ ; x += alpha·u_k ; r -= alpha·c_k.
            const T alpha = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{ck.data(), n},
                                                    crd::containers::ConstSpan<T>{r.data(), n});
            dense::axpy<T>(alpha, crd::containers::ConstSpan<T>{uk.data(), n}, x);
            dense::axpy<T>(-alpha, crd::containers::ConstSpan<T>{ck.data(), n}, r);

            res = nrm2<T>(r);
            ++result.iterations;
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
        }
        // restart: C/U are overwritten next cycle; the warm residual r carries over.
    }

    result.reason              = StopReason::MaxIterations;
    result.final_residual_norm = res;
    return result;
}

// Plain (unpreconditioned) GCR(m) convenience overload.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> gcr(const crd::hesap::LinearOp<T>&                          a,
                                                    crd::containers::ConstSpan<T>                          b,
                                                    crd::containers::Span<T>                               x,
                                                    const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                    GcrWorkspace<T>&                                       ws,
                                                    crd::memory::IAllocator* result_alloc)
{
    return gcr<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
