#pragma once

// feast.hpp — Phase 3.1.6 v6-g: FEAST (Polizzi 2009) — the contour-integration eigensolver. Finds ALL
// eigenvalues of a REAL SYMMETRIC sparse A in a user interval [lo, hi] (the INTERVAL/INTERIOR specialist —
// v6-a/b reach only the spectrum ends; v6-d reaches eigenvalues near a point; FEAST sweeps a whole band).
//
// The approximate spectral projector onto the eigenspace whose eigenvalues lie inside the contour C enclosing
// [lo, hi] is  ρ = (1/2πi) ∮_C (zI − A)⁻¹ dz.  Applied to a random block Y (n × m0), Q = ρ·Y spans the wanted
// eigenspace; a Rayleigh-Ritz on Q recovers the eigenpairs inside, and subspace iteration refines them. The
// contour integral is a Gauss-Legendre quadrature on the half-circle (real-symmetric A ⇒ the conjugate-pair
// fold makes ρ real): for the circle of center c=(lo+hi)/2, radius r=(hi−lo)/2,
//     Q ≈ Σ_k Re[ ω_k · (z_k I − A)⁻¹ Y ] ,  z_k = c + r·e^{iθ_k}, θ_k = (π/2)(x_k+1), ω_k = (γ_k·r/2)·e^{iθ_k}
// with (x_k, γ_k) the Gauss-Legendre nodes/weights on [−1, 1]. Each (z_k I − A)⁻¹ is a COMPLEX shifted solve —
// factored ONCE per FEAST run by the v5 complex multifrontal LU (z_k is OFF the real axis ⇒ well-conditioned;
// this is the "quadrature points → shift-invert (v5)" of the plan, reusing the hesap-direct edge v6-d added).
//
// FILTERING (what actually selects the eigenvalues): ρ has eigenvalues ≈1 inside the contour and ≈0 outside, so
// Q = ρ·Y mostly spans the wanted eigenspace. With m0 > count the surplus columns capture the JUST-OUTSIDE
// eigenvalues with a small but NON-negligible filter response (~1e-2…1e-3 for nq = 8) — they do NOT collapse to
// machine zero. The in/out decision is therefore made by TWO filters (in order of what does the work):
//   (1) the INTERVAL test λ ∈ [lo, hi] — the contaminated (just-outside) Ritz values land OUTSIDE the interval;
//   (2) the RESIDUAL gate ‖A·x − λ·x‖/max(|λ|,1) ≤ tol — a spurious IN-interval Ritz (a noise direction) is not
//       a genuine eigenpair, so it is excluded from the result (the safety net).
// The block MGS also DROPS any column whose post-projection norm < drop (16·eps); for an 8-point quadrature with
// m0 > count this is DEFENSIVE only — verified q == m0 in practice (the ~1e-3 surplus columns survive), so it
// fires solely on an EXACT collapse (e.g. a duplicated start direction), NOT on the design rank-deficiency.
// Contract: m0 ≥ count; leave headroom (every kept Ritz inside [lo, hi] with no column dropped ⇒ m0 was likely
// too small ⇒ flagged via converged=false).
//
// MOAT: the Gauss-Legendre nodes are hardcoded constants; the nq complex factors are bit-identical across
// {1,2,4,8} build-workers (the v5b multifrontal-LU moat); the projector accumulation (fixed node order), the
// block MGS (fixed order), and the dense Rayleigh-Ritz (eig_sym) are deterministic ⇒ the eigenpairs are
// bit-identical across worker counts (the differentiator FEAST-lib / ARPACK lack).

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/eigen/eigen_problem.hpp>
#include <crd/hesap/eigen/lanczos.hpp> // detail::splitmix_pm1
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>
#include <type_traits>
#include <utility>

namespace crd::hesap::eigen
{
namespace detail
{
// 8-point Gauss-Legendre nodes/weights on [−1, 1] (standard constants; symmetric ± pairs). nq = 8 is the FEAST
// default and ample; parameterizing nq needs general GL-node generation (a v13-quadrature / follow-on).
inline constexpr int kFeastNq = 8;
inline constexpr double kFeastGlX[kFeastNq] = {
    -0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
    0.1834346424956498,  0.5255324099163290,  0.7966664774136267,  0.9602898564975363};
inline constexpr double kFeastGlW[kFeastNq] = {
    0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
    0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763};
} // namespace detail

// Compute ALL eigenpairs of a real symmetric sparse A (CSR) in the interval [lo, hi] via FEAST. `m0` = the
// subspace size (MUST be ≥ the number of eigenvalues in [lo, hi]; leave headroom). `opts.tol` / `max_restarts`
// (the subspace-iteration cap) / `seed` are honoured; `opts.which` / `nev` are ignored (the interval drives it).
// Returns the eigenpairs inside [lo, hi]; `converged` = every in-interval pair reached tol AND m0 had headroom.
template <typename T>
[[nodiscard]] EigenResult<T> eigs_sym_feast(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a, T lo, T hi,
                                            crd::u32 m0, const EigenOptions<T>& opts, crd::memory::IAllocator* alloc,
                                            crd::u32 num_workers = 1)
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>, "eigs_sym_feast: real symmetric");
    namespace dn = crd::hesap::dense;
    namespace dir = crd::hesap::direct;
    using R = T;
    using C = crd::hesap::Complex<T>;
    constexpr int nq = detail::kFeastNq;

    EigenResult<T> result(alloc);
    const crd::u32 n = a.pattern().rows;
    result.n = n;
    if (n == 0 || m0 == 0 || !(hi > lo))
    {
        return result;
    }
    if (m0 > n)
    {
        m0 = n;
    }

    const R center = static_cast<R>(0.5) * (lo + hi);
    const R radius = static_cast<R>(0.5) * (hi - lo);
    const R kpi = static_cast<R>(3.14159265358979323846);

    // Quadrature nodes z_k on the half-circle + the real-fold weights ω_k = (γ_k·r/2)·e^{iθ_k}.
    C zq[nq];
    C wq[nq];
    for (int k = 0; k < nq; ++k)
    {
        const R theta = (kpi * static_cast<R>(0.5)) * (static_cast<R>(detail::kFeastGlX[k]) + R{1});
        const R ct = crd::math::cos(theta);
        const R st = crd::math::sin(theta);
        zq[k] = C{center + radius * ct, radius * st};
        const R wmag = static_cast<R>(detail::kFeastGlW[k]) * radius * static_cast<R>(0.5);
        wq[k] = C{wmag * ct, wmag * st};
    }

    // ---- factor (z_k I − A) ONCE per node (robust diagonal handling, like shift_invert) ----
    const crd::u32* rp = a.pattern().outer_ptr.data();
    const crd::u32* ci = a.pattern().inner_idx.data();
    const T* av = a.values().values.data();
    crd::containers::Array<dir::MultifrontalLU<C>> facs(alloc);
    facs.reserve(static_cast<crd::usize>(nq));
    for (int k = 0; k < nq; ++k)
    {
        sparse::TripletBuilder<C> tb(alloc, n, n);
        for (crd::u32 r = 0; r < n; ++r)
        {
            T diag = T{0};
            for (crd::u32 p = rp[r]; p < rp[r + 1]; ++p)
            {
                if (ci[p] == r)
                {
                    diag = av[p];
                }
                else
                {
                    tb.add(r, ci[p], C{-av[p], T{0}}); // (z_k I − A)_{rc} = −A_{rc}
                }
            }
            tb.add(r, r, C{zq[k].re - diag, zq[k].im}); // diagonal = z_k − A_{rr}
        }
        auto cshift = tb.compress();
        auto f = dir::factor_multifrontal_lu<C>(cshift, alloc, num_workers);
        if (f.info() != 0)
        {
            return result; // (z_k I − A) singular ⇒ an eigenvalue sits on the contour — caller nudges [lo,hi]
        }
        facs.push_back(std::move(f));
    }

    // spmv op for the Rayleigh-Ritz A·Q + residuals. With workers available (jobs inited, num_workers>1) use the
    // FORCED-parallel SELL spmv (bit-exact across thread counts ⇒ a NON-VACUOUS moat); otherwise the serial CSR
    // op (the default/serial path needs no jobs). Both are jobs-safe to CONSTRUCT (only the parallel apply needs
    // a live scheduler, and it is only reached when num_workers>1).
    const sparse::SparseLinearOp<T> aop_serial(a);
    const sparse::ParallelSparseLinearOp<T> aop_par(a, alloc, /*parallel_min_stored_bytes=*/0);
    const crd::hesap::LinearOp<T>* aop =
        (num_workers > 1) ? static_cast<const crd::hesap::LinearOp<T>*>(&aop_par) : &aop_serial;
    const R eps = std::numeric_limits<R>::epsilon();
    const R drop = static_cast<R>(16) * eps;
    const R tol = opts.effective_tol();

    auto mk = [&](crd::u32 cols) {
        crd::containers::Array<T> b(alloc);
        b.resize(static_cast<crd::usize>(n) * cols);
        return b;
    };
    crd::containers::Array<T> yb = mk(m0);  // iteration block Y (real)
    crd::containers::Array<T> qb = mk(m0);  // projector output Q (real)
    crd::containers::Array<T> aqb = mk(m0); // A·Q
    crd::containers::Array<T> xb = mk(m0);  // Ritz vectors X = Q·S
    crd::containers::Array<C> xkc(alloc);   // complex per-node solve scratch
    xkc.resize(static_cast<crd::usize>(n) * m0);
    auto col = [&](crd::containers::Array<T>& b, crd::u32 j) noexcept -> T* {
        return b.data() + static_cast<crd::usize>(j) * n;
    };
    auto ccol = [&](const crd::containers::Array<T>& b, crd::u32 j) noexcept -> const T* {
        return b.data() + static_cast<crd::usize>(j) * n;
    };

    // ---- initial block Y = deterministic random (n × m0) ----
    for (crd::u32 j = 0; j < m0; ++j)
    {
        T* yj = col(yb, j);
        for (crd::u32 i = 0; i < n; ++i)
        {
            yj[i] = detail::splitmix_pm1<R>(opts.seed + static_cast<crd::u64>(j) * 0x100000001B3ULL, i);
        }
    }

    crd::u32 cols = m0;
    crd::u32 max_it = opts.max_restarts < 40 ? opts.max_restarts : 40; // FEAST converges in a handful
    if (max_it < 1)
    {
        max_it = 1;
    }

    for (crd::u32 iter = 0; iter < max_it; ++iter)
    {
        result.iterations = iter + 1;

        // ---- Q = Σ_k Re[ ω_k · (z_k I − A)⁻¹ Y ] ----
        for (crd::usize i = 0; i < static_cast<crd::usize>(n) * cols; ++i)
        {
            qb[i] = T{0};
        }
        for (int k = 0; k < nq; ++k)
        {
            for (crd::usize i = 0; i < static_cast<crd::usize>(n) * cols; ++i)
            {
                xkc[i] = C{yb[i], T{0}};
            }
            facs[static_cast<crd::usize>(k)].apply_inverse({xkc.data(), static_cast<crd::usize>(n) * cols}, cols);
            const C w = wq[k];
            for (crd::usize i = 0; i < static_cast<crd::usize>(n) * cols; ++i)
            {
                qb[i] += w.re * xkc[i].re - w.im * xkc[i].im; // Re(ω_k · x)
            }
        }

        // ---- block MGS, DROPPING collapsed columns (the rank-deficiency handling) ----
        crd::u32 q = 0;
        for (crd::u32 j = 0; j < cols; ++j)
        {
            if (q != j)
            {
                for (crd::u32 i = 0; i < n; ++i)
                {
                    col(qb, q)[i] = ccol(qb, j)[i];
                }
            }
            T* d = col(qb, q);
            for (int pass = 0; pass < 2; ++pass)
            {
                for (crd::u32 i = 0; i < q; ++i)
                {
                    const T c = dn::dot<T>({ccol(qb, i), n}, {d, n});
                    dn::axpy<T>(-c, {ccol(qb, i), n}, {d, n});
                }
            }
            const R nr = dn::nrm2<T>({d, n});
            if (nr > drop)
            {
                dn::scal<T>(static_cast<T>(R{1} / nr), {d, n});
                ++q;
            }
        }
        if (q == 0)
        {
            break; // projector empty ⇒ no eigenvalues in/near the interval (result stays empty)
        }

        // ---- AQ = A·Q (q columns), reduced M = QᵀAQ, Rayleigh-Ritz ----
        for (crd::u32 j = 0; j < q; ++j)
        {
            (void)aop->apply({ccol(qb, j), n}, {col(aqb, j), n});
        }
        dn::Symmetric<T> m(alloc, q);
        for (crd::u32 i = 0; i < q; ++i)
        {
            for (crd::u32 j = 0; j <= i; ++j)
            {
                m.at(i, j) = dn::dot<T>({ccol(qb, i), n}, {ccol(aqb, j), n});
            }
        }
        dn::EigSym<T> es = dn::eig_sym<T>(alloc, m);

        // ---- Ritz vectors X = Q·S (into xb); select [lo,hi]; residual ‖A·x − λ·x‖ (A·x = AQ·s) ----
        bool all_conv = true;
        crd::u32 m_inside = 0;
        for (crd::u32 t = 0; t < q; ++t)
        {
            T* xt = col(xb, t);
            for (crd::u32 i = 0; i < n; ++i)
            {
                xt[i] = T{0};
            }
            for (crd::u32 l = 0; l < q; ++l)
            {
                dn::axpy<T>(es.vectors.at(l, t), {ccol(qb, l), n}, {xt, n});
            }
            const R lam = es.values.data()[t];
            if (lam >= lo && lam <= hi)
            {
                ++m_inside;
                R rn2 = R{0};
                for (crd::u32 i = 0; i < n; ++i)
                {
                    R ax = R{0};
                    for (crd::u32 l = 0; l < q; ++l)
                    {
                        ax += static_cast<R>(es.vectors.at(l, t)) * static_cast<R>(ccol(aqb, l)[i]);
                    }
                    const R e = ax - lam * static_cast<R>(xt[i]);
                    rn2 += e * e;
                }
                const R rn = crd::math::sqrt(rn2);
                const R sc = crd::math::fabs(lam) > R{1} ? crd::math::fabs(lam) : R{1};
                if (rn / sc > tol)
                {
                    all_conv = false;
                }
            }
        }
        const bool saturated = (q == m0) && (m_inside == q); // no headroom ⇒ result may be truncated
        const bool last = (iter + 1 >= max_it);

        if (all_conv || last)
        {
            // ---- assemble: emit a pair ONLY if it is in [lo,hi] AND genuine (residual ≤ tol). The residual
            // gate is the safety net: it excludes a SPURIOUS in-interval Ritz (e.g. from a noise direction) that
            // the interval test alone would let through — so the result never contains a non-eigenpair. (`xb`
            // holds the Ritz vectors X = Q·S; A·x = AQ·s.) ----
            result.values.resize(m_inside); // upper bound; trimmed to the emitted count below
            result.vectors.resize(static_cast<crd::usize>(n) * m_inside);
            result.residuals.resize(m_inside);
            crd::u32 out = 0;
            for (crd::u32 t = 0; t < q; ++t)
            {
                const R lam = es.values.data()[t];
                if (!(lam >= lo && lam <= hi))
                {
                    continue;
                }
                const T* xt = ccol(xb, t);
                R rn2 = R{0};
                for (crd::u32 i = 0; i < n; ++i)
                {
                    R ax = R{0};
                    for (crd::u32 l = 0; l < q; ++l)
                    {
                        ax += static_cast<R>(es.vectors.at(l, t)) * static_cast<R>(ccol(aqb, l)[i]);
                    }
                    const R e = ax - lam * static_cast<R>(xt[i]);
                    rn2 += e * e;
                }
                const R rn = crd::math::sqrt(rn2);
                const R sc = crd::math::fabs(lam) > R{1} ? crd::math::fabs(lam) : R{1};
                if (rn / sc > tol)
                {
                    continue; // in-interval but not a genuine eigenpair ⇒ excluded (the safety net)
                }
                crd::u32 imax = 0;
                R vmax = R{0};
                for (crd::u32 i = 0; i < n; ++i)
                {
                    const R mv = crd::math::fabs(static_cast<R>(xt[i]));
                    if (mv > vmax)
                    {
                        vmax = mv;
                        imax = i;
                    }
                }
                const T sgn = (xt[imax] < T{0}) ? T{-1} : T{1};
                result.values[out] = crd::hesap::Complex<R>{lam, R{0}};
                T* vo = result.vectors.data() + static_cast<crd::usize>(out) * n;
                for (crd::u32 i = 0; i < n; ++i)
                {
                    vo[i] = sgn * xt[i];
                }
                result.residuals[out] = rn;
                ++out;
            }
            result.values.resize(out); // trim to the genuine in-interval eigenpairs actually emitted
            result.vectors.resize(static_cast<crd::usize>(n) * out);
            result.residuals.resize(out);
            result.nconv = out;
            // converged ⇔ every in-interval Ritz was a genuine eigenpair (out == m_inside) AND m0 left headroom
            // (so the count is trustworthy) AND at least one was found.
            result.converged = (out == m_inside) && (out > 0) && !saturated;
            return result;
        }

        // ---- Y = the q Ritz vectors for the next projection ----
        for (crd::u32 t = 0; t < q; ++t)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(yb, t)[i] = ccol(xb, t)[i];
            }
        }
        cols = q;
    }
    return result; // empty (projector collapsed) — no eigenvalues found in [lo, hi]
}

} // namespace crd::hesap::eigen
