#pragma once

// jacobi_davidson.hpp — Phase 3.1.6 v6-f: Jacobi-Davidson (JDQR, symmetric) for A·x = λ·x. The Davidson
// subspace method whose distinguishing feature is the CORRECTION EQUATION solved inexactly by a Krylov method:
//
//     (I − Ũ Ũᵀ)(A − θ I)(I − Ũ Ũᵀ) t = −r ,   t ⊥ Ũ ,   Ũ = [Q | u]
//
// where (θ, u) is the current Ritz pair, r = A·u − θ·u the residual, and Q the already-converged (locked)
// eigenvectors. The projected operator is well-posed even as θ → λ because the (near-)null direction ≈ u lives
// in Ũ and is projected out — that is the elegance of JD (Sleijpen & Van der Vorst 1996). We solve the
// correction inexactly with FGMRES (crd-hesap-iterative — the determinism-moat solve: serial Arnoldi / Givens,
// parallel spmv only), optionally PRECONDITIONED by (I − ŨŨᵀ) K⁻¹ (I − ŨŨᵀ) with K ≈ A⁻¹ — the algorithmic-
// crush hook (a good preconditioner cuts the total matvec count vs unpreconditioned, the v6-f mechanism).
//
// SCOPE: ships EXTREME (Smallest/Largest) + CLUSTERED (one-at-a-time deflation handles multiplicity) + the
// preconditioned correction. INTERIOR via harmonic-Ritz extraction is DEFERRED — covered by v6-d shift-invert,
// and standard-Ritz interior extraction is unreliable (spurious Ritz values).
//
// MOAT: deterministic counter-RNG start + Rayleigh-Ritz via the deterministic dense eig_sym + fixed-order MGS +
// deterministic thick restart + FGMRES (serial). The only parallel step is the operator's spmv (bit-exact
// across worker counts) ⇒ the inner FGMRES iteration count is worker-identical ⇒ the eigenpairs are bit-
// identical across {1,2,4,8} workers, WITH or WITHOUT a (deterministic, serial-applied) preconditioner.
//
// Module edge: hesap-eigen → hesap-iterative (ACYCLIC — iterative is a lower sibling; same direction as the
// v5f-c2 hesap-direct → hesap-iterative edge).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/eigen/eigen_problem.hpp>
#include <crd/hesap/eigen/lanczos.hpp> // detail::splitmix_pm1
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace crd::hesap::eigen
{
namespace detail
{
// out = (I − Ũ Ũᵀ) x = x − Σ_j u_j (u_jᵀ x), Ũ = column-major `u` (n × nd, orthonormal). `c` = nd-coefficient
// scratch. `out` MUST be a distinct buffer from `x` (we read x while writing out).
template <typename T>
inline void jd_project(const T* u, crd::u32 nd, crd::usize n, const T* x, T* out, T* c) noexcept
{
    namespace dn = crd::hesap::dense;
    for (crd::u32 j = 0; j < nd; ++j)
    {
        c[j] = dn::dot<T>({u + static_cast<crd::usize>(j) * n, n}, {x, n});
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        out[i] = x[i];
    }
    for (crd::u32 j = 0; j < nd; ++j)
    {
        dn::axpy<T>(-c[j], {u + static_cast<crd::usize>(j) * n, n}, {out, n});
    }
}

// The JD correction operator y = (I − ŨŨᵀ)(A − θ I)(I − ŨŨᵀ) x. Holds views into the driver's deflation basis
// and scratch (used SERIALLY by FGMRES on the calling thread; only A's spmv inside is parallel).
template <typename T>
class JdProjectedOp final : public crd::hesap::LinearOp<T>
{
public:
    JdProjectedOp(const crd::hesap::LinearOp<T>& a, const T* u, crd::u32 nd, T theta, T* work, T* c,
                  crd::usize n) noexcept
        : m_a(&a), m_u(u), m_nd(nd), m_theta(theta), m_work(work), m_c(c), m_n(n)
    {
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        jd_project<T>(m_u, m_nd, m_n, x.data(), m_work, m_c); // m_work = P x
        (void)m_a->apply({m_work, m_n}, y);                   // y = A (P x)
        for (crd::usize i = 0; i < m_n; ++i)
        {
            y[i] = y[i] - m_theta * m_work[i]; // y = (A − θ I) P x
        }
        jd_project<T>(m_u, m_nd, m_n, y.data(), m_work, m_c); // m_work = P y
        for (crd::usize i = 0; i < m_n; ++i)
        {
            y[i] = m_work[i]; // y = P (A − θ I) P x
        }
        return true;
    }
    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    const crd::hesap::LinearOp<T>* m_a;
    const T*                       m_u;
    crd::u32                       m_nd;
    T                              m_theta;
    T*                             m_work;
    T*                             m_c;
    crd::usize                     m_n;
};

// The projected preconditioner z = (I − ŨŨᵀ) K⁻¹ (I − ŨŨᵀ) x (K⁻¹ = the user preconditioner). Keeps the
// preconditioned Krylov vectors in Ũ⊥, so all FGMRES iterates stay ⊥ Ũ ⇒ t ⊥ Ũ automatically.
template <typename T>
class JdProjectedPrecond final : public crd::hesap::LinearOp<T>
{
public:
    JdProjectedPrecond(const crd::hesap::LinearOp<T>& k, const T* u, crd::u32 nd, T* work, T* c,
                       crd::usize n) noexcept
        : m_k(&k), m_u(u), m_nd(nd), m_work(work), m_c(c), m_n(n)
    {
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        jd_project<T>(m_u, m_nd, m_n, x.data(), m_work, m_c); // m_work = P x
        (void)m_k->apply({m_work, m_n}, y);                   // y = K⁻¹ (P x)
        jd_project<T>(m_u, m_nd, m_n, y.data(), m_work, m_c); // m_work = P y
        for (crd::usize i = 0; i < m_n; ++i)
        {
            y[i] = m_work[i]; // y = P K⁻¹ P x
        }
        return true;
    }
    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    const crd::hesap::LinearOp<T>* m_k;
    const T*                       m_u;
    crd::u32                       m_nd;
    T*                             m_work;
    T*                             m_c;
    crd::usize                     m_n;
};
} // namespace detail

// Compute `opts.nev` extreme eigenpairs of a symmetric (matrix-free) operator `a` via Jacobi-Davidson (JDQR).
// `precond` (optional, K ≈ A⁻¹) preconditions the correction equation; nullptr ⇒ unpreconditioned (the
// mechanism-off baseline). `which` must be Smallest/LargestAlgebraic (the symmetric spectrum ends).
// `result.iterations` reports the TOTAL matvec count (A-applies: inner FGMRES + the per-expansion A·t) — the
// honest "work" metric for the preconditioning mechanism (NOT a cross-library wall-clock claim; that is v6-z).
template <typename T>
[[nodiscard]] EigenResult<T> eigs_sym_jd(const crd::hesap::LinearOp<T>& a, const EigenOptions<T>& opts,
                                         crd::memory::IAllocator* alloc,
                                         const crd::hesap::LinearOp<T>* precond = nullptr)
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>, "eigs_sym_jd: real symmetric");
    namespace dn = crd::hesap::dense;
    using R = T;

    EigenResult<T> result(alloc);
    const crd::u32 n = static_cast<crd::u32>(a.n_rows());
    result.n = n;
    if (n == 0 || opts.nev == 0)
    {
        return result;
    }
    crd::u32 nev = opts.nev;
    if (nev > n)
    {
        nev = n;
    }
    const bool want_smallest = (opts.which == Which::SmallestAlgebraic || opts.which == Which::SmallestMagnitude ||
                               opts.which == Which::SmallestReal);

    // Subspace bounds: grow V up to jmax columns, thick-restart down to jmin.
    crd::u32 jmax = opts.ncv > 0 ? opts.ncv : (2 * nev + 10);
    if (jmax > n)
    {
        jmax = n;
    }
    if (jmax > 256)
    {
        jmax = 256; // bounds the restart index buffer; a 256-dim JD subspace is already very large
    }
    if (jmax < nev + 1 && nev + 1 <= n)
    {
        jmax = nev + 1;
    }
    crd::u32 jmin = nev + 2;
    if (jmin > (jmax >= 2 ? jmax - 1 : 1))
    {
        jmin = (jmax >= 2 ? jmax - 1 : 1);
    }
    if (jmin < 1)
    {
        jmin = 1;
    }

    auto mk = [&](crd::usize cols) {
        crd::containers::Array<T> b(alloc);
        b.resize(static_cast<crd::usize>(n) * cols);
        return b;
    };
    crd::containers::Array<T> vv = mk(jmax);     // subspace V (n × jmax), orthonormal, ⊥ Q
    crd::containers::Array<T> avv = mk(jmax);    // A·V
    crd::containers::Array<T> vtmp = mk(jmax);   // restart-compaction scratch
    crd::containers::Array<T> avtmp = mk(jmax);  // restart-compaction scratch
    crd::containers::Array<T> qu = mk(nev + 1);  // deflation basis Ũ = [locked Q | current u]
    crd::containers::Array<T> u(alloc), au(alloc), r(alloc), rp(alloc), t(alloc), rhs(alloc);
    crd::containers::Array<T> wp(alloc), wq(alloc);
    u.resize(n);
    au.resize(n);
    r.resize(n);
    rp.resize(n);
    t.resize(n);
    rhs.resize(n);
    wp.resize(n);
    wq.resize(n);
    crd::containers::Array<T> cp(alloc), cq(alloc);
    cp.resize(nev + 1);
    cq.resize(nev + 1);

    auto col = [&](crd::containers::Array<T>& b, crd::u32 j) noexcept -> T* {
        return b.data() + static_cast<crd::usize>(j) * n;
    };
    auto ccol = [&](const crd::containers::Array<T>& b, crd::u32 j) noexcept -> const T* {
        return b.data() + static_cast<crd::usize>(j) * n;
    };

    const R eps = std::numeric_limits<R>::epsilon();
    const R drop = static_cast<R>(16) * eps;
    const R tol = opts.effective_tol();

    // Inner correction solve: loose tol + hard cap (a few steps suffice; the cap bounds the clustered case).
    crd::usize inner_m = n < 25 ? static_cast<crd::usize>(n) : 25;
    if (inner_m < 1)
    {
        inner_m = 1;
    }
    iterative::GmresWorkspace<T> ws(alloc, n, inner_m);
    iterative::IterativeOptions<R> iopts;
    iopts.rel_tol = static_cast<R>(0.1); // inexact (one digit) — the classic JD correction tolerance
    iopts.max_iter = inner_m;

    // Orthonormalize column `dst` of V against Q[0..nconv) and V[0..ndone), twice (MGS). Returns post-norm.
    auto orth_v = [&](crd::u32 dst, crd::u32 nconv, crd::u32 ndone) noexcept -> R {
        T* d = col(vv, dst);
        for (int pass = 0; pass < 2; ++pass)
        {
            for (crd::u32 j = 0; j < nconv; ++j)
            {
                const T c = dn::dot<T>({ccol(qu, j), n}, {d, n});
                dn::axpy<T>(-c, {ccol(qu, j), n}, {d, n});
            }
            for (crd::u32 j = 0; j < ndone; ++j)
            {
                const T c = dn::dot<T>({ccol(vv, j), n}, {d, n});
                dn::axpy<T>(-c, {ccol(vv, j), n}, {d, n});
            }
        }
        return dn::nrm2<T>({d, n});
    };

    crd::u64 seed_ctr = 0;
    auto seed_into = [&](crd::u32 dst, crd::u32 nconv, crd::u32 ndone) noexcept -> R {
        T* d = col(vv, dst);
        const crd::u64 s = opts.seed + seed_ctr * 0x100000001B3ULL;
        ++seed_ctr;
        for (crd::u32 i = 0; i < n; ++i)
        {
            d[i] = detail::splitmix_pm1<R>(s, i);
        }
        return orth_v(dst, nconv, ndone);
    };

    // Thick-restart: compress V/AV to the `keep` Ritz vectors `idx[p]` (V_new = V·S_idx, AV_new = AV·S_idx).
    auto restart_keep = [&](const dn::EigSym<T>& es, const crd::u32* idx, crd::u32 keep, crd::u32 mdim) noexcept {
        for (crd::u32 p = 0; p < keep; ++p)
        {
            T* vp = col(vtmp, p);
            T* ap = col(avtmp, p);
            for (crd::u32 i = 0; i < n; ++i)
            {
                vp[i] = T{0};
                ap[i] = T{0};
            }
            for (crd::u32 l = 0; l < mdim; ++l)
            {
                const T cc = es.vectors.at(l, idx[p]);
                dn::axpy<T>(cc, {ccol(vv, l), n}, {vp, n});
                dn::axpy<T>(cc, {ccol(avv, l), n}, {ap, n});
            }
        }
        for (crd::u32 p = 0; p < keep; ++p)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(vv, p)[i] = ccol(vtmp, p)[i];
                col(avv, p)[i] = ccol(avtmp, p)[i];
            }
        }
    };
    // p-th most-wanted Ritz index of the ascending eig_sym output (smallest ⇒ p, largest ⇒ mdim-1-p).
    auto wanted = [&](crd::u32 p, crd::u32 mdim) noexcept -> crd::u32 { return want_smallest ? p : (mdim - 1 - p); };

    result.values.resize(nev);
    result.vectors.resize(static_cast<crd::usize>(n) * nev);
    result.residuals.resize(nev);

    crd::u64 matvecs = 0;
    crd::u32 nconv = 0;

    // ---- seed the first subspace vector ----
    {
        R nr = seed_into(0, 0, 0);
        if (nr <= drop)
        {
            result.nconv = 0;
            result.iterations = 0;
            return result;
        }
        dn::scal<T>(static_cast<T>(R{1} / nr), {col(vv, 0), n});
    }
    (void)a.apply({ccol(vv, 0), n}, {col(avv, 0), n});
    ++matvecs;
    crd::u32 mdim = 1;

    crd::u32 idx[256]; // restart index list (jmax ≤ this in practice; guarded below)

    for (crd::u32 outer = 0; outer < opts.max_restarts; ++outer)
    {
        // ---- Rayleigh-Ritz on the current subspace: M = VᵀAV (symmetric), eig_sym ----
        dn::Symmetric<T> m(alloc, mdim);
        for (crd::u32 i = 0; i < mdim; ++i)
        {
            for (crd::u32 j = 0; j <= i; ++j)
            {
                m.at(i, j) = dn::dot<T>({ccol(vv, i), n}, {ccol(avv, j), n});
            }
        }
        dn::EigSym<T> es = dn::eig_sym<T>(alloc, m);

        const crd::u32 tgt = wanted(0, mdim);
        const R theta = es.values.data()[tgt];

        // u = V·s_tgt ; Au = AV·s_tgt
        for (crd::u32 i = 0; i < n; ++i)
        {
            u[i] = T{0};
            au[i] = T{0};
        }
        for (crd::u32 l = 0; l < mdim; ++l)
        {
            const T cc = es.vectors.at(l, tgt);
            dn::axpy<T>(cc, {ccol(vv, l), n}, {u.data(), n});
            dn::axpy<T>(cc, {ccol(avv, l), n}, {au.data(), n});
        }
        // r = Au − θ·u, then deflate r ⊥ Q (keeps the correction RHS in Ũ⊥).
        for (crd::u32 i = 0; i < n; ++i)
        {
            r[i] = au[i] - static_cast<T>(theta) * u[i];
        }
        if (nconv > 0)
        {
            detail::jd_project<T>(qu.data(), nconv, n, r.data(), rp.data(), cp.data());
            for (crd::u32 i = 0; i < n; ++i)
            {
                r[i] = rp[i];
            }
        }
        const R rnorm = dn::nrm2<T>({r.data(), n});
        const R sc = std::fabs(theta) > R{1} ? std::fabs(theta) : R{1};

        if (rnorm / sc <= tol)
        {
            // ---- lock (θ, u): store the eigenpair, append u to Q ----
            T* qcol = col(qu, nconv);
            for (crd::u32 i = 0; i < n; ++i)
            {
                qcol[i] = u[i];
            }
            result.values[nconv] = crd::hesap::Complex<R>{theta, R{0}};
            crd::u32 imax = 0;
            R vmax = R{0};
            for (crd::u32 i = 0; i < n; ++i)
            {
                const R mv = std::fabs(static_cast<R>(u[i]));
                if (mv > vmax)
                {
                    vmax = mv;
                    imax = i;
                }
            }
            const T sgn = (u[imax] < T{0}) ? T{-1} : T{1};
            T* vj = result.vectors.data() + static_cast<crd::usize>(nconv) * n;
            for (crd::u32 i = 0; i < n; ++i)
            {
                vj[i] = sgn * u[i];
            }
            result.residuals[nconv] = rnorm;
            ++nconv;
            result.iterations = static_cast<crd::u32>(matvecs);
            if (nconv >= nev)
            {
                break;
            }
            // restart keeping the best non-tgt Ritz vectors (⊥ the new Q automatically: orthonormal S ⇒ ⊥ u).
            crd::u32 keep = mdim > 1 ? (mdim - 1 < jmin ? mdim - 1 : jmin) : 0;
            if (keep == 0)
            {
                R nr = seed_into(0, nconv, 0);
                if (nr <= drop)
                {
                    break;
                }
                dn::scal<T>(static_cast<T>(R{1} / nr), {col(vv, 0), n});
                (void)a.apply({ccol(vv, 0), n}, {col(avv, 0), n});
                ++matvecs;
                mdim = 1;
                continue;
            }
            for (crd::u32 p = 0; p < keep; ++p)
            {
                idx[p] = wanted(p + 1, mdim); // skip tgt = wanted(0)
            }
            restart_keep(es, idx, keep, mdim);
            mdim = keep;
            continue;
        }

        // ---- not converged: solve the correction equation for t ⊥ Ũ = [Q | u] ----
        T* qcur = col(qu, nconv);
        for (crd::u32 i = 0; i < n; ++i)
        {
            qcur[i] = u[i]; // Ũ's last column = current u
            rhs[i] = -r[i];
            t[i] = T{0};
        }
        const crd::u32 nd = nconv + 1;
        const detail::JdProjectedOp<T> op(a, qu.data(), nd, static_cast<T>(theta), wp.data(), cp.data(), n);
        if (precond != nullptr)
        {
            const detail::JdProjectedPrecond<T> pre(*precond, qu.data(), nd, wq.data(), cq.data(), n);
            const auto res = iterative::fgmres<T>(op, &pre, {rhs.data(), n}, {t.data(), n}, iopts, ws, alloc);
            matvecs += res.iterations;
        }
        else
        {
            const auto res = iterative::fgmres<T>(op, nullptr, {rhs.data(), n}, {t.data(), n}, iopts, ws, alloc);
            matvecs += res.iterations;
        }

        // ---- restart if the subspace is full (before appending the new direction) ----
        if (mdim >= jmax)
        {
            const crd::u32 keep = jmin;
            for (crd::u32 p = 0; p < keep; ++p)
            {
                idx[p] = wanted(p, mdim);
            }
            restart_keep(es, idx, keep, mdim);
            mdim = keep;
        }

        // ---- orthonormalize t ⊥ [Q | V] and append as the new subspace column ----
        for (crd::u32 i = 0; i < n; ++i)
        {
            col(vv, mdim)[i] = t[i];
        }
        R nt = orth_v(mdim, nconv, mdim);
        if (nt <= drop)
        {
            // correction collapsed into the existing space — reseed a fresh random direction.
            nt = seed_into(mdim, nconv, mdim);
            if (nt <= drop)
            {
                result.iterations = static_cast<crd::u32>(matvecs);
                break; // stagnation
            }
        }
        dn::scal<T>(static_cast<T>(R{1} / nt), {col(vv, mdim), n});
        (void)a.apply({ccol(vv, mdim), n}, {col(avv, mdim), n});
        ++matvecs;
        ++mdim;
        result.iterations = static_cast<crd::u32>(matvecs);
    }

    // ---- finalize: trim to the locked eigenpairs ----
    result.values.resize(nconv);
    result.vectors.resize(static_cast<crd::usize>(n) * nconv);
    result.residuals.resize(nconv);
    result.nconv = nconv;
    result.converged = nconv >= opts.nev;
    return result;
}

} // namespace crd::hesap::eigen
