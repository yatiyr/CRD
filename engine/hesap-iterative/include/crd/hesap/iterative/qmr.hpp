#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_inner / krylov_mag / krylov_real / krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// QMR -- Quasi-Minimal Residual (Freund-Nachtigal 1991). Phase 3.1.6 v4d-2a.
//
// SHORT-recurrence Krylov solver for GENERAL (nonsymmetric / non-Hermitian) A.
// Built on the two-sided (bi-)Lanczos process, so it consumes BOTH A·x and
// Aᴴ·y every iteration (operator must provide apply_adjoint, e.g. a square
// ParallelSpmvLeastSquaresOp). Unlike BiCGSTAB its residual is quasi-minimized
// (smooth, monotone-ish) rather than oscillating; unlike GMRES it has O(1)
// storage. Eigen ships no QMR -- breadth + the determinism moat.
//
// INNER-PRODUCT CONVENTION: all dots are CONJUGATED (Hermitian `dotc`), matching
// the single module policy (BiCGSTAB/LSQR) and the Krylov.jl formulation this is
// transcribed from (which uses Aᴴ + conjugated dots, NOT the classic Aᵀ bilinear
// form). The left shadow start is c = r₀ (deterministic; cᴴr₀ = ‖r₀‖² ≠ 0).
//
// PRECONDITIONING: optional RIGHT preconditioner N = m_inv (solve A·N⁻¹·y = b,
// x = N⁻¹·y). Right-side keeps the quasi-residual in the TRUE-residual norm
// (matches BiCGSTAB). The bi-Lanczos adjoint sequence applies N⁻ᴴ, so a
// preconditioner used here MUST provide apply_adjoint (asserted). Unpreconditioned
// (m_inv == nullptr) is bit-identical (N = I: Nvₖ aliases vₖ, p aliases s, no
// untransform). The solution is untransformed once after the loop: x ← N⁻¹·x.
//
// Recurrence (coupled two-term + sym_givens rotations) transcribed VERBATIM from
// Krylov.jl `qmr.jl` / `sym_givens` to avoid sign errors in the rotations. The
// reported rNorm = |ζ̄|·√τ is the QUASI-residual (an upper bound on ‖b−Ax‖);
// tests verify the true residual separately. Determinism: reductions are
// KBN-pairwise, the only parallel steps are the operator's spmv/adjoint-spmv
// (bit-exact across threads) ⇒ bit-identical solve across thread counts.
// -----------------------------------------------------------------------

namespace detail
{
// Symmetric Givens rotation (Paige-Saunders). Given a (T) and b (the real,
// non-negative Lanczos β), returns (c real, s in T, r in T) with
// [c sᴴ; -s c]·[a; b] = [r; 0]. Transcribed verbatim from Krylov.jl `sym_givens`
// (the mixed Complex/real method promotes b to (b,0)). Reuses the shared
// detail::GivensRot<T> (a DIFFERENT convention from gmres_givens, hence its own fn).
template <typename T>
[[nodiscard]] inline GivensRot<T> sym_givens(T a, crd::hesap::dense::RealType<T> b) noexcept
{
    using R = crd::hesap::dense::RealType<T>;
    GivensRot<T> g{};
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        const R abs_a = detail::krylov_mag<T>(a);
        const R abs_b = b < R(0) ? -b : b; // b is a norm (≥0); guard anyway
        if (abs_b == R(0))
        {
            g.c   = R(1);
            g.s   = T{};
            g.r = a;
        }
        else if (abs_a == R(0))
        {
            g.c   = R(0);
            g.s   = T(R(1));
            g.r = T(abs_b);
        }
        else if (abs_b > abs_a)
        {
            const R t     = abs_a / abs_b;
            R       c     = R(1) / std::sqrt(R(1) + t * t);
            const T bb    = T(abs_b) / abs_b;   // (1,0)
            const T aa    = a / abs_a;          // a/|a|
            const T ratio = bb / aa;
            g.s   = T(c) * crd::hesap::conj(ratio);
            g.c   = c * t;
            g.r = T(abs_b) / crd::hesap::conj(g.s);
        }
        else
        {
            const R t     = abs_b / abs_a;
            const R c     = R(1) / std::sqrt(R(1) + t * t);
            const T bb    = T(abs_b) / abs_b;   // (1,0)
            const T aa    = a / abs_a;          // a/|a|
            const T ratio = bb / aa;
            g.s   = T(c * t) * crd::hesap::conj(ratio);
            g.c   = c;
            g.r = a / T(c);
        }
    }
    else
    {
        const R sa = a > R(0) ? R(1) : (a < R(0) ? R(-1) : R(0));
        const R sb = b > R(0) ? R(1) : (b < R(0) ? R(-1) : R(0));
        const R aa = a < R(0) ? -a : a;
        const R ab = b < R(0) ? -b : b;
        if (b == R(0))
        {
            g.c   = sa + (a == R(0) ? R(1) : R(0));
            g.s   = R(0);
            g.r = aa;
        }
        else if (a == R(0))
        {
            g.c   = R(0);
            g.s   = sb;
            g.r = ab;
        }
        else if (ab > aa)
        {
            const R t = a / b;
            g.s       = sb / std::sqrt(R(1) + t * t);
            g.c       = g.s * t;
            g.r     = b / g.s;
        }
        else
        {
            const R t = b / a;
            g.c       = sa / std::sqrt(R(1) + t * t);
            g.s       = g.c * t;
            g.r     = a / g.c;
        }
    }
    return g;
}
} // namespace detail

template <typename T>
struct QmrWorkspace
{
    // Bi-Lanczos vectors v*/u*, matvec scratch q/s, three w-recurrence directions,
    // and (preconditioned only) the right-precond v-side Nv + adjoint-side p.
    crd::hesap::dense::Vector<T> v_old, v, u_old, u, q, s, w0, w1, w2, nv, p;

    QmrWorkspace(crd::memory::IAllocator* alloc, crd::usize n)
        : v_old(alloc, n), v(alloc, n), u_old(alloc, n), u(alloc, n), q(alloc, n), s(alloc, n), w0(alloc, n),
          w1(alloc, n), w2(alloc, n), nv(alloc, n), p(alloc, n)
    {
    }

    [[nodiscard]] crd::usize size() const noexcept { return v.size(); }
};

// QMR with optional RIGHT preconditioner N = m_inv (nullptr ⇒ plain QMR).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> qmr(const crd::hesap::LinearOp<T>&  a,
                                                    const crd::hesap::LinearOp<T>*  m_inv,
                                                    crd::containers::ConstSpan<T>   b,
                                                    crd::containers::Span<T>        x,
                                                    const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                    QmrWorkspace<T>&                ws,
                                                    crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   n      = a.n_rows();
    const bool         prec   = (m_inv != nullptr);
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "qmr: operator must be square");
    CRD_ASSERT_MSG(a.has_adjoint(), "qmr: operator must provide apply_adjoint (Aᴴ)");
    CRD_ASSERT_MSG(!prec || m_inv->has_adjoint(), "qmr: right preconditioner must provide apply_adjoint (N⁻ᴴ)");
    CRD_ASSERT_MSG(b.size() == n && x.size() == n && ws.size() == n, "qmr: span/workspace size mismatch");

    const auto v_old = ws.v_old.span();
    const auto v     = ws.v.span();
    const auto u_old = ws.u_old.span();
    const auto u     = ws.u.span();
    const auto q     = ws.q.span();
    const auto s     = ws.s.span();
    auto       w0    = ws.w0.span();
    auto       w1    = ws.w1.span();
    auto       w2    = ws.w2.span();
    const auto nv    = prec ? ws.nv.span() : v; // Nvₖ = N⁻¹vₖ (aliases vₖ when N = I)
    const auto p     = prec ? ws.p.span() : s;  // p = N⁻ᴴ·(Aᴴuₖ) (aliases s when N = I)

    // r₀ = b - A·x (into q as scratch), left shadow c = r₀.
    (void)a.apply(x, q);
    for (crd::usize i = 0; i < n; ++i)
    {
        v[i]     = b[i] - q[i]; // r₀ (becomes v₁ after scaling)
        v_old[i] = T{};
        u_old[i] = T{};
        w0[i]    = T{};
        w1[i]    = T{};
    }
    const R res0 = nrm2<T>(v);
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

    // cᴴb = ⟨c, r₀⟩ = ‖r₀‖² (c = r₀); βₖ = √|cᴴb|, γₖ = cᴴb/βₖ.
    const T cb = detail::krylov_inner<T>(v, v); // c = r₀ copy of v ⇒ ⟨r₀,r₀⟩
    if (detail::krylov_mag<T>(cb) < smlnum)
    {
        result.reason              = StopReason::Breakdown; // bᴴc = 0
        result.final_residual_norm = res;
        return result;
    }
    R beta  = std::sqrt(detail::krylov_mag<T>(cb));
    T gamma = cb / T(beta);
    // v₁ = r₀/βₖ ; u₁ = c/conj(γₖ) = r₀/conj(γₖ).
    dense::copy<T>(v, u);
    dense::scal<T>(T(R(1) / beta), v);
    {
        const T inv = T(1) / detail::krylov_conj<T>(gamma);
        dense::scal<T>(inv, u);
    }

    R cs_2 = R(0), cs_1 = R(0); // cₖ₋₂, cₖ₋₁ (real)
    T sn_2 = T{}, sn_1 = T{};   // sₖ₋₂, sₖ₋₁
    T zbar = T(beta);           // ζ̄ₖ
    R tau  = detail::krylov_real<T>(detail::krylov_inner<T>(v, v)); // τₖ = ‖vₖ‖²

    T lambda_bar{}, lambda{}, eps2{}, delta_bar{}, delta{};

    for (crd::usize k = 1; k <= opts.max_iter; ++k)
    {
        // ---- bi-Lanczos step: build the next v and u ----
        // v-side: Nvₖ = N⁻¹vₖ ; q = A·Nvₖ (= t, since left precond M = I).
        if (prec)
        {
            (void)m_inv->apply(v, nv);
        }
        (void)a.apply(nv, q);
        // u-side: s = Aᴴ·uₖ ; p = N⁻ᴴ·s.
        (void)a.apply_adjoint(u, s);
        if (prec)
        {
            (void)m_inv->apply_adjoint(s, p);
        }

        // Orthogonalize against the previous pair, then against the current pair.
        dense::axpy<T>(-gamma, v_old, q);              // q -= γₖ·vₖ₋₁
        dense::axpy<T>(T(-beta), u_old, p);            // p -= βₖ·uₖ₋₁
        const T alpha = detail::krylov_inner<T>(u, q);  // αₖ = ⟨uₖ, q⟩
        dense::axpy<T>(-alpha, v, q);                   // q -= αₖ·vₖ
        dense::axpy<T>(-detail::krylov_conj<T>(alpha), u, p); // p -= conj(αₖ)·uₖ

        const T pq      = detail::krylov_inner<T>(p, q); // pᴴq
        const R beta_n  = std::sqrt(detail::krylov_mag<T>(pq));
        const bool brk  = beta_n < smlnum;               // pᴴq ≈ 0 ⇒ Lanczos breakdown
        T          gamma_n{};
        if (!brk)
        {
            gamma_n = pq / T(beta_n);
        }

        // ---- apply previous Givens reflections to the new tridiagonal column ----
        if (k >= 3)
        {
            eps2       = sn_2 * gamma;       // ϵₖ₋₂
            lambda_bar = -T(cs_2) * gamma;   // λ̄ₖ₋₁
        }
        if (k >= 2)
        {
            if (k == 2)
            {
                lambda_bar = gamma;
            }
            lambda    = T(cs_1) * lambda_bar + sn_1 * alpha;                          // λₖ₋₁
            delta_bar = detail::krylov_conj<T>(sn_1) * lambda_bar - T(cs_1) * alpha;   // δ̄ₖ
            sn_2      = sn_1;
            cs_2      = cs_1;
        }
        if (k == 1)
        {
            delta_bar = alpha;
        }

        // current rotation eliminating βₖ₊₁ from the column
        const detail::GivensRot<T> g = detail::sym_givens<T>(delta_bar, beta_n);
        const R                    ck = g.c;
        const T                    sk = g.s;
        delta                         = g.r;
        const T zeta                  = T(ck) * zbar;                    // ζₖ
        const T zbar_n                = detail::krylov_conj<T>(sk) * zbar; // ζ̄ₖ₊₁
        sn_1                          = sk;
        cs_1                          = ck;

        if (detail::krylov_mag<T>(delta) < smlnum)
        {
            result.iterations          = k;
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            break;
        }

        // ---- w-recurrence direction wₖ, then x += ζₖ·wₖ ----
        // w2 = (vₖ - λₖ₋₁·wₖ₋₁ - ϵₖ₋₂·wₖ₋₂) / δₖ   (missing terms are zero for k=1,2)
        dense::copy<T>(v, w2);
        if (k >= 2)
        {
            dense::axpy<T>(-lambda, w1, w2);
        }
        if (k >= 3)
        {
            dense::axpy<T>(-eps2, w0, w2);
        }
        dense::scal<T>(T(1) / delta, w2);
        dense::axpy<T>(zeta, w2, x); // x += ζₖ·wₖ

        // rotate w buffers: wₖ₋₂ ← wₖ₋₁ ← wₖ
        {
            auto tmp = w0;
            w0       = w1;
            w1       = w2;
            w2       = tmp;
        }

        // ---- advance the Lanczos vectors ----
        dense::copy<T>(v, v_old);
        dense::copy<T>(u, u_old);
        if (!brk)
        {
            dense::copy<T>(q, v);
            dense::scal<T>(T(R(1) / beta_n), v); // vₖ₊₁ = q/βₖ₊₁
            dense::copy<T>(p, u);
            dense::scal<T>(T(1) / detail::krylov_conj<T>(gamma_n), u); // uₖ₊₁ = p/conj(γₖ₊₁)
        }

        // ---- quasi-residual estimate + stopping ----
        tau += detail::krylov_real<T>(detail::krylov_inner<T>(v, v)); // τₖ₊₁
        res = detail::krylov_mag<T>(zbar_n) * std::sqrt(tau);
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
            break;
        }
        if (brk)
        {
            result.reason              = StopReason::Breakdown; // pᴴq ≈ 0
            result.final_residual_norm = res;
            break;
        }
        if (k == opts.max_iter)
        {
            result.reason              = StopReason::MaxIterations;
            result.final_residual_norm = res;
        }

        zbar  = zbar_n;
        beta  = beta_n;
        gamma = gamma_n;
    }

    // Right-precond untransform: x ← N⁻¹·x (q is free scratch after the loop).
    if (prec)
    {
        (void)m_inv->apply(x, q);
        dense::copy<T>(q, x);
    }
    return result;
}

// Plain (unpreconditioned) QMR convenience overload.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> qmr(const crd::hesap::LinearOp<T>&                          a,
                                                    crd::containers::ConstSpan<T>                          b,
                                                    crd::containers::Span<T>                               x,
                                                    const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                    QmrWorkspace<T>&                                       ws,
                                                    crd::memory::IAllocator* result_alloc)
{
    return qmr<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
