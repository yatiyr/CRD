#pragma once

// krylov_schur.hpp — Phase 3.1.6 v6-c (completion): KRYLOV-SCHUR restart (Stewart 2001 ≡ IRAM) — the bounded-
// memory NONSYMMETRIC eigensolver. Chosen over implicit shifted-QR (IRAM) because it is DETERMINISTIC (moat-
// safe): restart = truncate a REORDERED real-Schur form, not delicate bulge-chasing with exact shifts.
//
// One cycle: extend Arnoldi to m ⇒ A·V_m = V_m·H_m + β_m·v_{m+1}·e_mᵀ. Schur H_m = Z·T·Zᵀ (real_schur) ⇒
// A·(V_m·Z) = (V_m·Z)·T + β_m·v_{m+1}·(last row of Z)ᵀ. Reorder T (reorder_schur) so the WANTED Ritz values
// lead; keep the leading k (respecting 2×2 conjugate blocks). The restarted projected matrix is
//   B = [ T_k          new-cols ]   (T_k = leading quasi-tri Schur block; rows 0..k-1)
//       [ β_m·b_kᵀ     Hessenberg ]  (row k = the residual coupling; then standard Arnoldi)
// with the new basis [V_m·Z[:,:k], v_{m+1}]. Re-Rayleigh-Ritz via dense eig(B); repeat until converged.
//
// MOAT: serial Arnoldi + serial Schur/reorder/eig; only a.apply (spmv) is parallel + bit-exact ⇒ the
// eigenpairs are bit-identical across {1,2,4,8} (the Schur QR + reorder are deterministic per D(non-sym)-1).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/eig_nonsym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/eigen/arnoldi.hpp> // detail::splitmix_pm1 / cmag / more_wanted_c + the spec
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::hesap::eigen
{
namespace detail
{
// The eigenvalue (and size 1 or 2) of the real-Schur block whose top is row `i`.
template <typename T>
[[nodiscard]] inline crd::hesap::Complex<T> schur_block(const crd::hesap::dense::Matrix<T>& t, crd::u32 m,
                                                       crd::u32 i, crd::u32& size) noexcept
{
    if (i + 1 < m && t.at(i + 1, i) != T{0})
    {
        size = 2;
        const T a = t.at(i, i);
        const T b = t.at(i, i + 1);
        const T c = t.at(i + 1, i);
        const T d = t.at(i + 1, i + 1);
        const T re = (a + d) / T{2};
        const T half = (a - d) / T{2};
        const T q = half * half + b * c; // < 0 for a complex-conjugate pair (standardized block)
        if (q < T{0})
        {
            return crd::hesap::Complex<T>{re, std::sqrt(-q)}; // representative = the +imag member
        }
        size = 1; // degenerate: treat as 1×1
    }
    size = (i + 1 < m && t.at(i + 1, i) != T{0}) ? 2 : 1;
    return crd::hesap::Complex<T>{t.at(i, i), T{0}};
}

// Real Schur form (T, Z with h = Z·T·Zᵀ) of a GENERAL m×m matrix `h` — the restarted projected matrix is NOT
// upper-Hessenberg (it carries the kept Schur block T_k + the arrowhead residual row), so `real_schur` (which
// assumes Hessenberg) cannot be called on it directly. Pipeline: Hessenberg-reduce h = Q·Hess·Qᵀ, real_schur
// the Hessenberg part Hess = Z_s·T·Z_sᵀ, then compose Z = Q·Z_s. (No balance — Krylov projected matrices are
// well-scaled; the deterministic Schur QR is the moat-relevant part.)
template <typename T>
[[nodiscard]] inline crd::hesap::dense::RealSchur<T> general_real_schur(crd::memory::IAllocator* alloc,
                                                                       const crd::hesap::dense::Matrix<T>& h,
                                                                       crd::u32 m)
{
    namespace dn = crd::hesap::dense;
    dn::Matrix<T> work = h.clone();
    crd::containers::Array<T> tau(alloc);
    dn::hessenberg<T>(work, 0, m - 1, tau);
    dn::Matrix<T> qh = dn::form_hessenberg_q<T>(alloc, work, 0, m - 1, tau);
    dn::Matrix<T> hmat(alloc, m, m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < m; ++j)
        {
            hmat.at(i, j) = (j + 1 >= i) ? work.at(i, j) : T{0}; // upper triangle + first subdiagonal
        }
    }
    dn::RealSchur<T> sch = dn::real_schur<T>(alloc, hmat, 0, m - 1, /*vectors=*/true);
    dn::RealSchur<T> out(alloc);
    out.t = std::move(sch.t);
    out.z = dn::Matrix<T>(alloc, m, m);
    for (crd::u32 i = 0; i < m; ++i) // Z = Q · Z_s
    {
        for (crd::u32 j = 0; j < m; ++j)
        {
            T acc = T{0};
            for (crd::u32 l = 0; l < m; ++l)
            {
                acc += qh.at(i, l) * sch.z.at(l, j);
            }
            out.z.at(i, j) = acc;
        }
    }
    out.converged = sch.converged;
    return out;
}
} // namespace detail

// Bounded-memory nonsymmetric eigensolver via Krylov-Schur restart. Returns the wanted complex eigenvalues +
// eigenvectors (+ true residuals), like `eigs_nonsym`, but converges large-n / clustered spectra at ncv ≤ n.
template <typename T>
[[nodiscard]] EigenResult<T> eigs_nonsym_ks(const crd::hesap::LinearOp<T>& a, const EigenOptions<T>& opts,
                                            crd::memory::IAllocator* alloc)
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>, "eigs_nonsym_ks: real nonsymmetric");
    namespace dn = crd::hesap::dense;
    using R = T;
    using C = crd::hesap::Complex<R>;

    EigenResult<T> result(alloc);
    const crd::u32 n = static_cast<crd::u32>(a.n_rows());
    result.n = n;
    if (n == 0 || opts.nev == 0)
    {
        return result;
    }
    crd::u32 m = opts.ncv;
    if (m == 0)
    {
        const crd::u32 def = 2 * opts.nev + 1;
        m = def > 20U ? def : 20U;
    }
    if (m < opts.nev + 2)
    {
        m = opts.nev + 2;
    }
    if (m > n)
    {
        m = n;
    }

    crd::containers::Array<T> v(alloc);
    v.resize(static_cast<crd::usize>(n) * (static_cast<crd::usize>(m) + 1));
    crd::containers::Array<T> w(alloc);
    w.resize(n);
    crd::containers::Array<T> kept(alloc); // n × m scratch for the restarted basis V·Z[:,:k]
    kept.resize(static_cast<crd::usize>(n) * m);
    auto col = [&](crd::u32 j) noexcept -> T* { return v.data() + static_cast<crd::usize>(j) * n; };
    dn::Matrix<T> h(alloc, m, m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < m; ++j)
        {
            h.at(i, j) = T{0};
        }
    }

    {
        T* v0 = col(0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            v0[i] = detail::splitmix_pm1<R>(opts.seed, i);
        }
        const R nrm = dn::nrm2<T>({v0, n});
        dn::scal<T>(static_cast<T>(R{1} / nrm), {v0, n});
    }

    const R eps = std::numeric_limits<R>::epsilon();
    const R tol = opts.effective_tol();
    crd::u32 seed_ctr = 1;
    crd::u32 nkeep = 0;
    crd::u32 restarts = 0;
    crd::u32 matvecs = 0;
    R beta_m = R{0};
    dn::Matrix<T> bsub(alloc, m, m); // the leading-mm copy fed to eig (Rayleigh-Ritz)

    while (true)
    {
        // ---- extend Arnoldi from column nkeep to m (full GS; leading T_k + arrowhead row pre-set) ----
        for (crd::u32 j = nkeep; j < m; ++j)
        {
            T* vj = col(j);
            (void)a.apply({vj, n}, {w.data(), n});
            ++matvecs;
            for (int pass = 0; pass < 2; ++pass)
            {
                for (crd::u32 i = 0; i <= j; ++i)
                {
                    const T cc = dn::dot<T>({col(i), n}, {w.data(), n});
                    h.at(i, j) += cc;
                    dn::axpy<T>(-cc, {col(i), n}, {w.data(), n});
                }
            }
            R beta = dn::nrm2<T>({w.data(), n});
            beta_m = beta;
            T* vnext = col(j + 1);
            if (beta <= static_cast<R>(16) * eps)
            {
                for (crd::u32 i = 0; i < n; ++i)
                {
                    vnext[i] = detail::splitmix_pm1<R>(opts.seed + seed_ctr * 0x100000001B3ULL, i);
                }
                ++seed_ctr;
                for (int pass = 0; pass < 2; ++pass)
                {
                    for (crd::u32 i = 0; i <= j; ++i)
                    {
                        const T cc = dn::dot<T>({vnext, n}, {col(i), n});
                        dn::axpy<T>(-cc, {col(i), n}, {vnext, n});
                    }
                }
                beta = dn::nrm2<T>({vnext, n});
                if (j + 1 < m)
                {
                    h.at(j + 1, j) = T{0};
                }
                dn::scal<T>(static_cast<T>(R{1} / beta), {vnext, n});
                continue;
            }
            if (j + 1 < m)
            {
                h.at(j + 1, j) = static_cast<T>(beta);
            }
            for (crd::u32 i = 0; i < n; ++i)
            {
                vnext[i] = w[i];
            }
            dn::scal<T>(static_cast<T>(R{1} / beta), {vnext, n});
        }

        // ---- Rayleigh-Ritz: eig(H) for convergence test ----
        for (crd::u32 i = 0; i < m; ++i)
        {
            for (crd::u32 j = 0; j < m; ++j)
            {
                bsub.at(i, j) = h.at(i, j);
            }
        }
        dn::EigNonsym<T> es = dn::eig<T>(alloc, bsub);
        // count converged wanted via the cheap Arnoldi estimate β_m·|s[m-1]|.
        crd::containers::Array<crd::u32> order(alloc);
        order.resize(m);
        for (crd::u32 i = 0; i < m; ++i)
        {
            order[i] = i;
        }
        const crd::u32 nsel = opts.nev < m ? opts.nev : m;
        for (crd::u32 s = 0; s < nsel; ++s)
        {
            crd::u32 best = s;
            for (crd::u32 t2 = s + 1; t2 < m; ++t2)
            {
                if (detail::more_wanted_c<R>(opts.which, es.values.data()[order[t2]], es.values.data()[order[best]]))
                {
                    best = t2;
                }
            }
            const crd::u32 tmp = order[s];
            order[s] = order[best];
            order[best] = tmp;
        }
        crd::u32 nconv = 0;
        for (crd::u32 s = 0; s < nsel; ++s)
        {
            const crd::u32 idx = order[s];
            const R rest = beta_m * detail::cmag<R>(es.vectors.at(m - 1, idx));
            const R sc = detail::cmag<R>(es.values.data()[idx]);
            if (rest <= tol * (sc > R{1} ? sc : R{1}))
            {
                ++nconv;
            }
        }
        if (nconv >= opts.nev || m >= n || restarts >= opts.max_restarts)
        {
            break;
        }

        // ---- restart: real Schur of the (NON-Hessenberg) H, reorder the wanted to the front, truncate to k ----
        dn::RealSchur<T> rs = detail::general_real_schur<T>(alloc, h, m);
        crd::u32 ktarget = opts.nev + (opts.nev < 8U ? opts.nev : 8U);
        if (ktarget > m - 2)
        {
            ktarget = m - 2;
        }
        if (ktarget < opts.nev)
        {
            ktarget = opts.nev;
        }
        // Selection: repeatedly move the most-wanted remaining block to the next free leading slot `p`.
        crd::u32 p = 0;
        while (p < ktarget && p < m)
        {
            crd::u32 best = p;
            crd::u32 bsize = 1;
            C bestval = detail::schur_block<T>(rs.t, m, p, bsize);
            crd::u32 i = p;
            while (i < m)
            {
                crd::u32 sz = 1;
                const C val = detail::schur_block<T>(rs.t, m, i, sz);
                if (detail::more_wanted_c<R>(opts.which, val, bestval))
                {
                    best = i;
                    bestval = val;
                    bsize = sz;
                }
                i += sz;
            }
            if (best != p)
            {
                (void)dn::reorder_schur<T>(rs.t, rs.z, best, p);
            }
            crd::u32 movedsz = 1;
            (void)detail::schur_block<T>(rs.t, m, p, movedsz);
            p += movedsz;
        }
        crd::u32 k = p; // ends on a block boundary (≥ nev)
        if (k > m - 1)
        {
            k = m - 1;
        }
        // new basis V[:k] = V·Z[:,:k] (recompute before overwriting V); residual row b_k = Z[m-1, :k].
        for (crd::u32 jj = 0; jj < k; ++jj)
        {
            T* xj = kept.data() + static_cast<crd::usize>(jj) * n;
            for (crd::u32 r = 0; r < n; ++r)
            {
                xj[r] = T{0};
            }
            for (crd::u32 ii = 0; ii < m; ++ii)
            {
                dn::axpy<T>(rs.z.at(ii, jj), {col(ii), n}, {xj, n});
            }
        }
        for (crd::u32 jj = 0; jj < k; ++jj)
        {
            const T* src = kept.data() + static_cast<crd::usize>(jj) * n;
            T* dst = col(jj);
            for (crd::u32 r = 0; r < n; ++r)
            {
                dst[r] = src[r];
            }
        }
        {
            const T* src = col(m); // v_{m+1}
            T* dst = col(k);
            for (crd::u32 r = 0; r < n; ++r)
            {
                dst[r] = src[r];
            }
        }
        // restarted H: leading k×k = T_k, arrowhead row k = β_m·(last row of Z)[:k], everything else 0.
        for (crd::u32 i = 0; i < m; ++i)
        {
            for (crd::u32 j = 0; j < m; ++j)
            {
                h.at(i, j) = T{0};
            }
        }
        for (crd::u32 i = 0; i < k; ++i)
        {
            for (crd::u32 j = 0; j < k; ++j)
            {
                h.at(i, j) = rs.t.at(i, j);
            }
            h.at(k, i) = static_cast<T>(beta_m) * rs.z.at(m - 1, i);
        }
        nkeep = k;
        ++restarts;
    }
    result.iterations = matvecs;

    // ---- final extraction: complex eigenvalues + eigenvectors + true residuals (the eigs_nonsym path) ----
    dn::EigNonsym<T> es = dn::eig<T>(alloc, bsub);
    const C* vals = es.values.data();
    crd::containers::Array<crd::u32> pick(alloc);
    pick.resize(m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        pick[i] = i;
    }
    const crd::u32 k = opts.nev < m ? opts.nev : m;
    for (crd::u32 s = 0; s < k; ++s)
    {
        crd::u32 best = s;
        for (crd::u32 t2 = s + 1; t2 < m; ++t2)
        {
            if (detail::more_wanted_c<R>(opts.which, vals[pick[t2]], vals[pick[best]]))
            {
                best = t2;
            }
        }
        const crd::u32 tmp = pick[s];
        pick[s] = pick[best];
        pick[best] = tmp;
    }
    result.values.resize(k);
    result.residuals.resize(k);
    result.vectors.resize(static_cast<crd::usize>(n) * k);
    result.vectors_im.resize(static_cast<crd::usize>(n) * k);
    crd::containers::Array<T> xre(alloc);
    crd::containers::Array<T> xim(alloc);
    crd::containers::Array<T> axre(alloc);
    crd::containers::Array<T> axim(alloc);
    xre.resize(n);
    xim.resize(n);
    axre.resize(n);
    axim.resize(n);
    crd::u32 nconv = 0;
    for (crd::u32 s = 0; s < k; ++s)
    {
        const crd::u32 idx = pick[s];
        const C th = vals[idx];
        result.values[s] = th;
        for (crd::u32 i = 0; i < n; ++i)
        {
            xre[i] = T{0};
            xim[i] = T{0};
        }
        for (crd::u32 jj = 0; jj < m; ++jj)
        {
            const C sj = es.vectors.at(jj, idx);
            dn::axpy<T>(sj.re, {col(jj), n}, {xre.data(), n});
            dn::axpy<T>(sj.im, {col(jj), n}, {xim.data(), n});
        }
        R xn2 = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            xn2 += xre[i] * xre[i] + xim[i] * xim[i];
        }
        const R xn = std::sqrt(xn2);
        if (xn > R{0})
        {
            const T inv = static_cast<T>(R{1} / xn);
            dn::scal<T>(inv, {xre.data(), n});
            dn::scal<T>(inv, {xim.data(), n});
        }
        crd::u32 imax = 0;
        R vmax = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            const R m2 = xre[i] * xre[i] + xim[i] * xim[i];
            if (m2 > vmax)
            {
                vmax = m2;
                imax = i;
            }
        }
        const R mag = std::sqrt(vmax);
        if (mag > R{0})
        {
            const R cph = xre[imax] / mag;
            const R sph = xim[imax] / mag;
            for (crd::u32 i = 0; i < n; ++i)
            {
                const R nr = xre[i] * cph + xim[i] * sph;
                const R ni = xim[i] * cph - xre[i] * sph;
                xre[i] = nr;
                xim[i] = ni;
            }
        }
        (void)a.apply({xre.data(), n}, {axre.data(), n});
        (void)a.apply({xim.data(), n}, {axim.data(), n});
        R rn2 = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            const R rr = axre[i] - (th.re * xre[i] - th.im * xim[i]);
            const R ri = axim[i] - (th.re * xim[i] + th.im * xre[i]);
            rn2 += rr * rr + ri * ri;
        }
        const R rn = std::sqrt(rn2);
        result.residuals[s] = rn;
        const R sc = detail::cmag<R>(th);
        if (rn <= tol * (sc > R{1} ? sc : R{1}))
        {
            ++nconv;
        }
        T* vr = result.vectors.data() + static_cast<crd::usize>(s) * n;
        T* vi = result.vectors_im.data() + static_cast<crd::usize>(s) * n;
        for (crd::u32 i = 0; i < n; ++i)
        {
            vr[i] = xre[i];
            vi[i] = xim[i];
        }
    }
    result.nconv = nconv;
    result.converged = nconv >= opts.nev;
    return result;
}

} // namespace crd::hesap::eigen
