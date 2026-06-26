#pragma once

// svds.hpp — Phase 3.1.6 v6-h: sparse SVD via Golub-Kahan-Lanczos (GKL) BIDIAGONALIZATION + thick restart
// (the deterministic equivalent of Baglama-Reichel IRLBA 2005). Computes the LARGEST `opts.nev` singular
// triplets (σ_i, u_i, v_i) of a rectangular m×n sparse operator A — the sparse SVD. (Distinct from principle
// #10 randomized SVD.) parity + the determinism moat (no ARPACK/PROPACK/svds carries it).
//
// GKL builds A·V_k = U_k·B_k (upper-bidiagonal B_k) and Aᵀ·U_k = V_k·B_kᵀ + β·v_{k+1}·eₖᵀ via the recurrence
//   wⱼ = A·vⱼ − β_{j-1}·u_{j-1};  αⱼ = ‖wⱼ‖;  uⱼ = wⱼ/αⱼ
//   zⱼ = Aᵀ·uⱼ − αⱼ·vⱼ;          βⱼ = ‖zⱼ‖;  v_{j+1} = zⱼ/βⱼ
// The singular values of the tiny B_k approximate those of A; the dense `svd` of B_k gives U_B/Σ/V_B, and the
// triplets recover as σ=Σ, u=U_k·U_B, v=V_k·V_B. The SVD sign is COUPLED (σ≥0 links u,v): u,v come STRAIGHT
// from the dense `svd`'s already-pinned U_B/V_B — NO independent re-sign-pin (it would break A·v = σ·u). FULL
// reorthogonalization is BOTH-SIDED (each new u against U, each new v against V, MGS twice) — one-sided reorthog
// leaks orthogonality and spawns ghost singular values.
//
// THICK RESTART (Baglama-Reichel augmented): after a cycle of `ncv` GKL steps, SVD B = P·Σ·Qᵀ, keep the j
// largest triplets Ũ_j=U·P_j, Ṽ_j=V·Q_j (A·Ṽ_j = Ũ_j·Σ_j — a DIAGONAL kept block), record the spike couplings
// ρ_i = β_last·P[last][i] (Aᵀ·Ũ_j = Ṽ_j·Σ_j + v_res·ρᵀ), and continue GKL from the residual v_res. The restarted
// B is diag(Σ_j) + a spike column ρ + a trailing bidiagonal — converging the wanted triplets at bounded ncv ≪
// min(m,n). Deterministic (the implicit-restart bulge-chase is replaced by this equivalent) ⇒ moat-safe.
//
// SCOPE: LARGEST singular triplets. SMALLEST is DEFERRED — GKL converges the bottom of the spectrum glacially;
// it needs shift-invert on the normal equations or harmonic extraction (a separate mechanism, like v6-f's
// deferred harmonic-Ritz interior).
//
// MOAT: deterministic counter-RNG start + fixed-order both-sided reorthog + deterministic dense `svd` + the
// deterministic thick restart. Only A·v / Aᵀ·u are parallel (bit-exact via the SELL spmv) ⇒ the triplets are
// bit-identical across {1,2,4,8} workers.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/svd.hpp>
#include <crd/hesap/eigen/eigen_problem.hpp>
#include <crd/hesap/eigen/lanczos.hpp> // detail::splitmix_pm1
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>
#include <type_traits>

namespace crd::hesap::eigen
{

// The sparse-SVD result. Singular values are real + descending; the k-th triplet is (values[k], U[:,k], V[:,k])
// with A·V[:,k] ≈ values[k]·U[:,k]. U is m×nconv and V is n×nconv (column-major) — distinct lengths, so this is
// NOT an EigenResult. residuals[k] = √(‖A·v−σ·u‖² + ‖Aᵀ·u−σ·v‖²) (the TRUE two-sided residual).
template <typename T> struct SvdResult
{
    using R = crd::hesap::dense::RealType<T>;

    crd::containers::Array<R> values;        // singular values, descending (length nconv)
    crd::containers::Array<T> left_vectors;  // U: m × nconv, column-major
    crd::containers::Array<T> right_vectors; // V: n × nconv, column-major
    crd::containers::Array<R> residuals;     // two-sided residual (length nconv)
    crd::u32                  m = 0;
    crd::u32                  n = 0;
    crd::u32                  nconv = 0;
    crd::u32                  iterations = 0; // restart cycles
    bool                      converged = false;

    explicit SvdResult(crd::memory::IAllocator* alloc) noexcept
        : values(alloc), left_vectors(alloc), right_vectors(alloc), residuals(alloc)
    {
    }
};

// Compute the LARGEST `opts.nev` singular triplets of a rectangular operator `a` (n_rows = m, n_cols = n) via
// thick-restart GKL bidiagonalization (IRLBA). `a.apply` = A·x (n→m); `a.apply_adjoint` = Aᵀ·x (m→n).
template <typename T>
[[nodiscard]] SvdResult<T> svds(const crd::hesap::LinearOp<T>& a, const EigenOptions<T>& opts,
                                crd::memory::IAllocator* alloc)
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>, "svds: real (f32/f64)");
    namespace dn = crd::hesap::dense;
    using R = T;

    SvdResult<T> result(alloc);
    const crd::u32 m = static_cast<crd::u32>(a.n_rows());
    const crd::u32 n = static_cast<crd::u32>(a.n_cols());
    result.m = m;
    result.n = n;
    if (m == 0 || n == 0 || opts.nev == 0)
    {
        return result;
    }
    const crd::u32 mn = m < n ? m : n;
    crd::u32 nsv = opts.nev;
    if (nsv > mn)
    {
        nsv = mn;
    }
    crd::u32 ncv = opts.ncv > 0 ? opts.ncv : (2 * nsv + 10 < 20 ? 20 : 2 * nsv + 10);
    if (ncv > mn)
    {
        ncv = mn;
    }
    if (ncv < nsv + 1 && nsv + 1 <= mn)
    {
        ncv = nsv + 1;
    }
    // Thick-restart keep count: nev + buffer, bounded so at least one new column is added per cycle.
    crd::u32 jkeep = 2 * nsv;
    if (jkeep > ncv - 1)
    {
        jkeep = ncv - 1;
    }
    if (jkeep < nsv)
    {
        jkeep = nsv;
    }

    crd::containers::Array<T> vv(alloc);   // V: n × (ncv+1), column-major
    crd::containers::Array<T> uu(alloc);   // U: m × ncv, column-major
    crd::containers::Array<T> utmp(alloc); // restart combination scratch (m × ncv)
    crd::containers::Array<T> vtmp(alloc); // restart combination scratch (n × ncv)
    crd::containers::Array<T> vres(alloc); // residual right vector at restart (n)
    crd::containers::Array<R> alpha(alloc);
    crd::containers::Array<R> beta(alloc);
    crd::containers::Array<R> sig(alloc);  // kept singular values (the restart diagonal block)
    crd::containers::Array<R> rho(alloc);  // spike couplings ρ_i
    crd::containers::Array<T> av(alloc);   // A·v   (m)
    crd::containers::Array<T> atu(alloc);  // Aᵀ·u  (n)
    vv.resize(static_cast<crd::usize>(n) * (ncv + 1));
    uu.resize(static_cast<crd::usize>(m) * ncv);
    utmp.resize(static_cast<crd::usize>(m) * ncv);
    vtmp.resize(static_cast<crd::usize>(n) * ncv);
    vres.resize(n);
    alpha.resize(ncv);
    beta.resize(ncv);
    sig.resize(jkeep);
    rho.resize(jkeep);
    av.resize(m);
    atu.resize(n);

    auto vcol = [&](crd::u32 j) noexcept -> T* { return vv.data() + static_cast<crd::usize>(j) * n; };
    auto ucol = [&](crd::u32 j) noexcept -> T* { return uu.data() + static_cast<crd::usize>(j) * m; };

    const R eps = std::numeric_limits<R>::epsilon();
    const R drop = static_cast<R>(16) * eps;
    const R tol = opts.effective_tol();

    // MGS-twice orthogonalize `x` (length len) against the first `cnt` columns of `buf` (each length len).
    auto reorthog = [&](T* x, const T* buf, crd::u32 cnt, crd::u32 len) noexcept {
        for (int pass = 0; pass < 2; ++pass)
        {
            for (crd::u32 i = 0; i < cnt; ++i)
            {
                const T* bi = buf + static_cast<crd::usize>(i) * len;
                const T c = dn::dot<T>({bi, len}, {x, len});
                dn::axpy<T>(-c, {bi, len}, {x, len});
            }
        }
    };

    // v_0 = deterministic random unit (n).
    {
        T* v0 = vcol(0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            v0[i] = detail::splitmix_pm1<R>(opts.seed, i);
        }
        const R nr = dn::nrm2<T>({v0, n});
        if (nr <= drop)
        {
            return result;
        }
        dn::scal<T>(static_cast<T>(R{1} / nr), {v0, n});
    }

    // Run GKL columns [jsplit, ncv) — both-sided full reorthogonalization. The first step at `jsplit` is the
    // AUGMENTED step (subtract the spike coupling ρ) when jsplit>0; cycle-0 (jsplit==0) is the plain start.
    // Returns kk = number of α's produced (== ncv unless an invariant subspace breaks it short).
    auto gkl = [&](crd::u32 jsplit) noexcept -> crd::u32 {
        crd::u32 kk = jsplit;
        for (crd::u32 l = jsplit; l < ncv; ++l)
        {
            (void)a.apply({vcol(l), n}, {av.data(), m}); // w = A·v_l
            if (l == jsplit && jsplit > 0)
            {
                for (crd::u32 i = 0; i < jsplit; ++i) // augmented: w -= Σ ρ_i u_i
                {
                    dn::axpy<T>(-static_cast<T>(rho[i]), {ucol(i), m}, {av.data(), m});
                }
            }
            else if (l > 0)
            {
                dn::axpy<T>(-static_cast<T>(beta[l - 1]), {ucol(l - 1), m}, {av.data(), m});
            }
            reorthog(av.data(), uu.data(), l, m);
            alpha[l] = dn::nrm2<T>({av.data(), m});
            if (alpha[l] <= drop)
            {
                break;
            }
            dn::scal<T>(static_cast<T>(R{1} / alpha[l]), {av.data(), m});
            for (crd::u32 i = 0; i < m; ++i)
            {
                ucol(l)[i] = av[i];
            }
            ++kk;
            (void)a.apply_adjoint({ucol(l), m}, {atu.data(), n}); // z = Aᵀ·u_l − α_l·v_l
            dn::axpy<T>(-static_cast<T>(alpha[l]), {vcol(l), n}, {atu.data(), n});
            reorthog(atu.data(), vv.data(), l + 1, n);
            beta[l] = dn::nrm2<T>({atu.data(), n});
            if (beta[l] <= drop)
            {
                break; // invariant subspace
            }
            dn::scal<T>(static_cast<T>(R{1} / beta[l]), {atu.data(), n});
            for (crd::u32 i = 0; i < n; ++i)
            {
                vcol(l + 1)[i] = atu[i];
            }
        }
        return kk;
    };

    // Build the kk×kk projected matrix B: kept diagonal block diag(sig) + spike column ρ at col jsplit + the
    // trailing upper-bidiagonal (α on diag, β on superdiag) from column jsplit on. jsplit==0 ⇒ pure bidiagonal.
    auto build_b = [&](crd::u32 kk, crd::u32 jsplit) {
        dn::Matrix<T> b(alloc, kk, kk);
        for (crd::u32 i = 0; i < jsplit; ++i)
        {
            b.at(i, i) = static_cast<T>(sig[i]);
            b.at(i, jsplit) = static_cast<T>(rho[i]);
        }
        for (crd::u32 l = jsplit; l < kk; ++l)
        {
            b.at(l, l) = static_cast<T>(alpha[l]);
            if (l > jsplit)
            {
                b.at(l - 1, l) = static_cast<T>(beta[l - 1]);
            }
        }
        return b;
    };

    crd::u32 jsplit = 0;
    crd::u32 kk = gkl(0);
    crd::u32 cycles = 0;
    if (kk == 0)
    {
        return result;
    }

    const crd::u32 max_cycles = opts.max_restarts < 1 ? 1 : opts.max_restarts;
    dn::SVD<T> bs = dn::svd<T>(alloc, build_b(kk, jsplit));
    for (;;)
    {
        ++cycles;
        // Cheap GKL convergence estimate for the nsv wanted: ‖Aᵀu−σv‖ = β_last·|last comp of left sing vec|.
        bool converged = kk >= nsv;
        const R blast = kk > 0 ? beta[kk - 1] : R{0};
        for (crd::u32 i = 0; i < nsv && i < kk; ++i)
        {
            const R sg = bs.s.data()[i];
            const R est = crd::math::fabs(static_cast<R>(blast)) * crd::math::fabs(static_cast<R>(bs.u.at(kk - 1, i)));
            const R sc = sg > R{1} ? sg : R{1};
            if (est > tol * sc)
            {
                converged = false;
            }
        }
        if (converged || cycles >= max_cycles || kk < ncv)
        {
            break; // (kk<ncv ⇒ an invariant subspace closed the cycle early — done)
        }

        // ---- thick restart: keep the jkeep largest triplets, record spike ρ, continue from the residual ----
        const crd::u32 jk = jkeep < kk ? jkeep : (kk > 0 ? kk - 1 : 0);
        if (jk == 0)
        {
            break;
        }
        for (crd::u32 p = 0; p < jk; ++p) // Ũ = U·P[:,p], Ṽ = V·Q[:,p]
        {
            T* up = utmp.data() + static_cast<crd::usize>(p) * m;
            T* vp = vtmp.data() + static_cast<crd::usize>(p) * n;
            for (crd::u32 i = 0; i < m; ++i)
            {
                up[i] = T{0};
            }
            for (crd::u32 i = 0; i < n; ++i)
            {
                vp[i] = T{0};
            }
            for (crd::u32 r = 0; r < kk; ++r)
            {
                dn::axpy<T>(bs.u.at(r, p), {ucol(r), m}, {up, m});
                dn::axpy<T>(bs.v.at(r, p), {vcol(r), n}, {vp, n});
            }
            sig[p] = bs.s.data()[p];
            rho[p] = static_cast<R>(blast) * static_cast<R>(bs.u.at(kk - 1, p)); // spike coupling
        }
        for (crd::u32 i = 0; i < n; ++i) // save the residual right vector v_res = V[:,kk]
        {
            vres[i] = vcol(kk)[i];
        }
        for (crd::u32 p = 0; p < jk; ++p) // install the kept basis
        {
            for (crd::u32 i = 0; i < m; ++i)
            {
                ucol(p)[i] = (utmp.data() + static_cast<crd::usize>(p) * m)[i];
            }
            for (crd::u32 i = 0; i < n; ++i)
            {
                vcol(p)[i] = (vtmp.data() + static_cast<crd::usize>(p) * n)[i];
            }
        }
        for (crd::u32 i = 0; i < n; ++i) // the residual becomes column jk
        {
            vcol(jk)[i] = vres[i];
        }
        jsplit = jk;
        kk = gkl(jsplit);
        if (kk <= jsplit)
        {
            // continuation produced nothing new — converge on what we have
            bs = dn::svd<T>(alloc, build_b(kk, jsplit));
            break;
        }
        bs = dn::svd<T>(alloc, build_b(kk, jsplit));
    }
    result.iterations = cycles;

    // ---- recover the nsv largest triplets: σ = bs.s; u = U_k·bs.u[:,i]; v = V_k·bs.v[:,i] (signs from svd) ----
    crd::u32 nout = nsv < kk ? nsv : kk;
    result.values.resize(nout);
    result.left_vectors.resize(static_cast<crd::usize>(m) * nout);
    result.right_vectors.resize(static_cast<crd::usize>(n) * nout);
    result.residuals.resize(nout);
    crd::u32 nconv = 0;
    for (crd::u32 i = 0; i < nout; ++i)
    {
        const R sigma = bs.s.data()[i];
        T* uo = result.left_vectors.data() + static_cast<crd::usize>(i) * m;
        T* vo = result.right_vectors.data() + static_cast<crd::usize>(i) * n;
        for (crd::u32 r = 0; r < m; ++r)
        {
            uo[r] = T{0};
        }
        for (crd::u32 r = 0; r < n; ++r)
        {
            vo[r] = T{0};
        }
        for (crd::u32 r = 0; r < kk; ++r)
        {
            dn::axpy<T>(bs.u.at(r, i), {ucol(r), m}, {uo, m}); // u = Σ_r U_k[:,r]·bs.u[r,i]
            dn::axpy<T>(bs.v.at(r, i), {vcol(r), n}, {vo, n}); // v = Σ_r V_k[:,r]·bs.v[r,i]
        }
        result.values[i] = sigma;
        // TRUE two-sided residual √(‖A·v−σ·u‖² + ‖Aᵀ·u−σ·v‖²).
        (void)a.apply({vo, n}, {av.data(), m});
        (void)a.apply_adjoint({uo, m}, {atu.data(), n});
        R r2 = R{0};
        for (crd::u32 r = 0; r < m; ++r)
        {
            const R e = static_cast<R>(av[r]) - sigma * static_cast<R>(uo[r]);
            r2 += e * e;
        }
        for (crd::u32 r = 0; r < n; ++r)
        {
            const R e = static_cast<R>(atu[r]) - sigma * static_cast<R>(vo[r]);
            r2 += e * e;
        }
        const R rn = crd::math::sqrt(r2);
        result.residuals[i] = rn;
        const R sc = sigma > R{1} ? sigma : R{1};
        if (rn / sc <= tol)
        {
            ++nconv;
        }
    }
    result.nconv = nconv;
    result.converged = (nconv >= nsv) && (nout >= nsv);
    return result;
}

} // namespace crd::hesap::eigen
