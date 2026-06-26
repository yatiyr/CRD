#pragma once

// thick_restart.hpp — Phase 3.1.6 v6-b: THICK-RESTART LANCZOS (Wu-Simon 2000 ≡ implicitly-restarted Lanczos).
//
// The bounded-memory symmetric workhorse: keep a Krylov basis of size ≤ ncv, and when it fills, RESTART by
// retaining the k best Ritz vectors (the "thick" set — more than 1) + the residual vector, then continue.
// Converges CLUSTERED / large-n spectra that the v6-a no-restart pass cannot (e.g. the Laplacian's largest
// eigenvalues, bunched near 4).
//
// THE RESTART STRUCTURE: after keeping {x_0..x_{k-1}} (Ritz vectors, Ritz values θ_i) + the residual vector
// v (= v_{m+1}), the projected matrix in the new basis [x_0..x_{k-1}, v, …] is an ARROWHEAD: diag(θ) on the
// kept block, with coupling s_i = β_m·Y[m-1][i] from v to each x_i; the continued part is tridiagonal. With
// FULL reorthogonalization the recurrence needs no explicit arrowhead term (reorthog removes the kept
// components); the s_i appear ONLY in the Rayleigh-Ritz matrix. Chosen over implicit shifted-QR (IRLM)
// because it is deterministic (moat-safe) — see the v6 plan note.
//
// MOAT: recurrence + Rayleigh-Ritz + restart selection run serially (bit-exact reductions); only `a.apply`
// (spmv) is parallel + bit-exact ⇒ eigenpairs bit-identical across {1,2,4,8} workers.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/eigen/lanczos.hpp> // detail::splitmix_pm1 + detail::more_wanted + the spec
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>
#include <type_traits>

namespace crd::hesap::eigen
{
// Thick-restart symmetric Lanczos. Same contract as `eigs_sym` but bounded-memory + restarting ⇒ converges
// where the single no-restart pass stalls. `ncv` is the MAX basis size (default auto); `max_restarts` caps
// the cycles.
template <typename T>
[[nodiscard]] EigenResult<T> eigs_sym_tr(const crd::hesap::LinearOp<T>& a, const EigenOptions<T>& opts,
                                         crd::memory::IAllocator* alloc)
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>,
                  "eigs_sym_tr: v6-b is real symmetric");
    namespace dn = crd::hesap::dense;
    using R = T;

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
    if (m < opts.nev + 1)
    {
        m = opts.nev + 1; // need room for at least one continued step
    }
    if (m > n)
    {
        m = n;
    }

    crd::containers::Array<T> v(alloc); // n × (m+1)
    v.resize(static_cast<crd::usize>(n) * (static_cast<crd::usize>(m) + 1));
    crd::containers::Array<R> alpha(alloc);
    crd::containers::Array<R> beta(alloc);
    crd::containers::Array<R> sc(alloc); // arrowhead couplings s_i for the kept block
    alpha.resize(m);
    beta.resize(m);
    sc.resize(m);
    crd::containers::Array<T> w(alloc);
    w.resize(n);
    crd::containers::Array<T> kept(alloc); // n × m scratch for recomputed Ritz vectors at restart
    kept.resize(static_cast<crd::usize>(n) * m);
    auto col = [&](crd::u32 j) noexcept -> T* { return v.data() + static_cast<crd::usize>(j) * n; };

    // Deterministic start v0, normalized.
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
    crd::u32 seed_ctr = 1; // for re-seeding on a lucky breakdown (deterministic)

    crd::u32 nkeep = 0;     // kept Ritz vectors at the start of the current cycle (0 = plain Lanczos)
    crd::u32 restarts = 0;
    crd::u32 matvecs = 0;
    dn::EigSym<T> es(alloc); // last Rayleigh-Ritz (kept for the final extraction)
    R amax = R{0};
    R last_beta = R{0};

    while (true)
    {
        // ---- extend the Lanczos factorization from column nkeep to m (full reorthogonalization) ----
        for (crd::u32 j = nkeep; j < m; ++j)
        {
            T* vj = col(j);
            (void)a.apply({vj, n}, {w.data(), n}); // w = A·v_j
            ++matvecs;
            if (j > 0 && j != nkeep)
            {
                dn::axpy<T>(static_cast<T>(-beta[j - 1]), {col(j - 1), n}, {w.data(), n}); // standard 3-term
            }
            // (at j == nkeep > 0 the restart-boundary coupling is the arrowhead — handled by full reorthog.)
            const T aj = dn::dot<T>({w.data(), n}, {vj, n});
            alpha[j] = aj;
            dn::axpy<T>(-aj, {vj, n}, {w.data(), n});
            for (int pass = 0; pass < 2; ++pass)
            {
                for (crd::u32 i = 0; i <= j; ++i)
                {
                    const T c = dn::dot<T>({w.data(), n}, {col(i), n});
                    dn::axpy<T>(-c, {col(i), n}, {w.data(), n});
                }
            }
            R bj = dn::nrm2<T>({w.data(), n});
            const R am = crd::math::fabs(aj);
            amax = am > amax ? am : amax;
            T* vnext = col(j + 1);
            if (bj <= static_cast<R>(16) * eps * (amax > R{0} ? amax : R{1}))
            {
                // Lucky breakdown: re-seed v_{j+1} with a fresh deterministic vector, reorthogonalize.
                for (crd::u32 i = 0; i < n; ++i)
                {
                    vnext[i] = detail::splitmix_pm1<R>(opts.seed + seed_ctr * 0x100000001B3ULL, i);
                }
                ++seed_ctr;
                for (int pass = 0; pass < 2; ++pass)
                {
                    for (crd::u32 i = 0; i <= j; ++i)
                    {
                        const T c = dn::dot<T>({vnext, n}, {col(i), n});
                        dn::axpy<T>(-c, {col(i), n}, {vnext, n});
                    }
                }
                bj = dn::nrm2<T>({vnext, n});
                beta[j] = R{0}; // true off-diagonal is zero (decoupled) — the T stays block-correct
                dn::scal<T>(static_cast<T>(R{1} / bj), {vnext, n});
                continue;
            }
            beta[j] = bj;
            for (crd::u32 i = 0; i < n; ++i)
            {
                vnext[i] = w[i];
            }
            dn::scal<T>(static_cast<T>(R{1} / bj), {vnext, n});
        }
        last_beta = beta[m - 1];

        // ---- Rayleigh-Ritz: build the m×m projected matrix (kept diag θ + arrowhead sc | continued tridiag) ----
        dn::Symmetric<T> tm(alloc, m);
        for (crd::u32 i = 0; i < nkeep; ++i)
        {
            tm.at(i, i) = static_cast<T>(alpha[i]);   // θ_i
            tm.at(nkeep, i) = static_cast<T>(sc[i]);  // arrowhead coupling v_k ↔ x_i
        }
        for (crd::u32 j = nkeep; j < m; ++j)
        {
            tm.at(j, j) = static_cast<T>(alpha[j]);
            if (j + 1 < m)
            {
                tm.at(j + 1, j) = static_cast<T>(beta[j]);
            }
        }
        es = dn::eig_sym<T>(alloc, tm);
        const R* theta = es.values.data();

        // ---- select the nev wanted + cheap convergence (Ritz residual = |β_m · Y[m-1][idx]|) ----
        crd::containers::Array<crd::u32> order(alloc);
        order.resize(m);
        for (crd::u32 i = 0; i < m; ++i)
        {
            order[i] = i;
        }
        // partial selection sort: front of `order` = the most-wanted, descending by `which`.
        const crd::u32 nsel = opts.nev < m ? opts.nev : m;
        for (crd::u32 s = 0; s < nsel; ++s)
        {
            crd::u32 best = s;
            for (crd::u32 t = s + 1; t < m; ++t)
            {
                if (detail::more_wanted<R>(opts.which, theta[order[t]], theta[order[best]]))
                {
                    best = t;
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
            const R rest = crd::math::fabs(last_beta * static_cast<R>(es.vectors.at(m - 1, idx)));
            if (rest <= tol * (crd::math::fabs(theta[idx]) > R{1} ? crd::math::fabs(theta[idx]) : R{1}))
            {
                ++nconv;
            }
        }
        if (nconv >= opts.nev || m >= n || restarts >= opts.max_restarts)
        {
            break; // converged (or full basis / cap) — extract below from `es` + the current basis
        }

        // ---- THICK RESTART: keep k Ritz pairs (the wanted nev + a buffer) ----
        crd::u32 k = opts.nev + (opts.nev < 10U ? opts.nev : 10U);
        if (k > m - 1)
        {
            k = m - 1;
        }
        if (k < opts.nev)
        {
            k = opts.nev;
        }
        // continue the partial sort so order[0..k-1] are the k most-wanted.
        for (crd::u32 s = nsel; s < k; ++s)
        {
            crd::u32 best = s;
            for (crd::u32 t = s + 1; t < m; ++t)
            {
                if (detail::more_wanted<R>(opts.which, theta[order[t]], theta[order[best]]))
                {
                    best = t;
                }
            }
            const crd::u32 tmp = order[s];
            order[s] = order[best];
            order[best] = tmp;
        }
        // recompute the kept Ritz vectors x_i = V · Y[:,order[i]] (before overwriting V).
        for (crd::u32 i = 0; i < k; ++i)
        {
            T* xi = kept.data() + static_cast<crd::usize>(i) * n;
            for (crd::u32 r = 0; r < n; ++r)
            {
                xi[r] = T{0};
            }
            for (crd::u32 jj = 0; jj < m; ++jj)
            {
                dn::axpy<T>(es.vectors.at(jj, order[i]), {col(jj), n}, {xi, n});
            }
        }
        // arrowhead couplings + kept Ritz values for the next cycle's projected matrix.
        for (crd::u32 i = 0; i < k; ++i)
        {
            sc[i] = last_beta * static_cast<R>(es.vectors.at(m - 1, order[i]));
        }
        // new basis: V[0..k-1] = kept Ritz vectors; V[k] = the residual vector v_{m+1} (== col(m)).
        for (crd::u32 i = 0; i < k; ++i)
        {
            T* dst = col(i);
            const T* src = kept.data() + static_cast<crd::usize>(i) * n;
            for (crd::u32 r = 0; r < n; ++r)
            {
                dst[r] = src[r];
            }
        }
        {
            T* dst = col(k);
            const T* src = col(m); // the last residual vector v_{m+1}
            for (crd::u32 r = 0; r < n; ++r)
            {
                dst[r] = src[r];
            }
        }
        for (crd::u32 i = 0; i < k; ++i)
        {
            alpha[i] = theta[order[i]]; // kept diagonal θ_i
        }
        nkeep = k;
        ++restarts;
    }
    result.iterations = matvecs;

    // ---- final extraction: the nev wanted eigenpairs from the last Rayleigh-Ritz + the current basis ----
    const R* theta = es.values.data();
    crd::containers::Array<crd::u32> pick(alloc);
    pick.resize(m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        pick[i] = i;
    }
    const crd::u32 kfin = opts.nev < m ? opts.nev : m;
    for (crd::u32 s = 0; s < kfin; ++s)
    {
        crd::u32 best = s;
        for (crd::u32 t = s + 1; t < m; ++t)
        {
            if (detail::more_wanted<R>(opts.which, theta[pick[t]], theta[pick[best]]))
            {
                best = t;
            }
        }
        const crd::u32 tmp = pick[s];
        pick[s] = pick[best];
        pick[best] = tmp;
    }

    result.values.resize(kfin);
    result.residuals.resize(kfin);
    if (opts.compute_vectors)
    {
        result.vectors.resize(static_cast<crd::usize>(n) * kfin);
    }
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> ax(alloc);
    x.resize(n);
    ax.resize(n);
    crd::u32 nconv = 0;
    for (crd::u32 s = 0; s < kfin; ++s)
    {
        const crd::u32 idx = pick[s];
        const R th = theta[idx];
        result.values[s] = crd::hesap::Complex<R>{th, R{0}};
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = T{0};
        }
        for (crd::u32 jj = 0; jj < m; ++jj)
        {
            dn::axpy<T>(es.vectors.at(jj, idx), {col(jj), n}, {x.data(), n});
        }
        const R xn = dn::nrm2<T>({x.data(), n});
        if (xn > R{0})
        {
            dn::scal<T>(static_cast<T>(R{1} / xn), {x.data(), n});
        }
        crd::u32 imax = 0;
        R vmax = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            const R am = crd::math::fabs(static_cast<R>(x[i]));
            if (am > vmax)
            {
                vmax = am;
                imax = i;
            }
        }
        if (static_cast<R>(x[imax]) < R{0})
        {
            dn::scal<T>(static_cast<T>(-R{1}), {x.data(), n});
        }
        (void)a.apply({x.data(), n}, {ax.data(), n});
        R rn = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            const R d = static_cast<R>(ax[i]) - th * static_cast<R>(x[i]);
            rn += d * d;
        }
        rn = crd::math::sqrt(rn);
        result.residuals[s] = rn;
        if (rn <= tol * (crd::math::fabs(th) > R{1} ? crd::math::fabs(th) : R{1}))
        {
            ++nconv;
        }
        if (opts.compute_vectors)
        {
            T* xc = result.vectors.data() + static_cast<crd::usize>(s) * n;
            for (crd::u32 i = 0; i < n; ++i)
            {
                xc[i] = x[i];
            }
        }
    }
    result.nconv = nconv;
    result.converged = nconv >= opts.nev;
    return result;
}

} // namespace crd::hesap::eigen
