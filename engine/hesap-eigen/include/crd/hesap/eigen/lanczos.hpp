#pragma once

// lanczos.hpp — Phase 3.1.6 v6-a: SYMMETRIC LANCZOS (the first sparse eigensolver + the v6 substrate).
//
// Matrix-free over LinearOp<T> (real symmetric A). Full-reorthogonalization Lanczos (no restart — v6-b adds
// thick-restart): build an orthonormal Krylov basis V and the tridiagonal T = Vᵀ·A·V, then Rayleigh-Ritz =
// dense `eig_sym(T)` ⇒ Ritz values θ + Ritz vectors S; the wanted eigenvectors are X = V·S.
//
// DETERMINISM MOAT: the recurrence + reorthogonalization + Rayleigh-Ritz run SERIALLY on the calling thread
// (bit-exact KBN-pairwise blas1 reductions); the ONLY parallel step is `a.apply` (the spmv), which is bit-
// exact across workers ⇒ the eigenpairs are bit-identical across {1,2,4,8}. Pinned for reproducibility:
//   • deterministic SplitMix64 counter-RNG start (NOT a timing seed),
//   • fixed-order modified Gram-Schmidt, twice (the moat + stability),
//   • SIGN convention: the largest-magnitude component of each eigenvector is forced positive.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/eigen/eigen_problem.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>
#include <type_traits>

namespace crd::hesap::eigen
{
namespace detail
{
// Deterministic SplitMix64 → a uniform value in [-1, 1). Pure function of (seed, i) ⇒ the moat start is
// reproducible and thread-count independent.
template <typename R> [[nodiscard]] inline R splitmix_pm1(crd::u64 seed, crd::u32 i) noexcept
{
    crd::u64 z = seed + (static_cast<crd::u64>(i) + 1) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    const R u = static_cast<R>(z >> 11) * (static_cast<R>(1) / static_cast<R>(1ULL << 53)); // [0,1)
    return static_cast<R>(2) * u - static_cast<R>(1);
}

// `which` ordering key: returns true iff Ritz value `x` is "more wanted" than `y`.
template <typename R> [[nodiscard]] inline bool more_wanted(Which w, R x, R y) noexcept
{
    switch (w)
    {
    case Which::LargestAlgebraic:
    case Which::LargestReal:
        return x > y;
    case Which::SmallestAlgebraic:
    case Which::SmallestReal:
        return x < y;
    case Which::LargestMagnitude:
        return crd::math::fabs(x) > crd::math::fabs(y);
    case Which::SmallestMagnitude:
        return crd::math::fabs(x) < crd::math::fabs(y);
    }
    return x > y;
}
} // namespace detail

// Compute `opts.nev` eigenpairs of a REAL SYMMETRIC operator `a` (matrix-free). Returns the wanted pairs
// (ordered by `which`) with their true residuals ‖A·x − θ·x‖/‖x‖; `nconv` counts those meeting the tolerance.
template <typename T>
[[nodiscard]] EigenResult<T> eigs_sym(const crd::hesap::LinearOp<T>& a, const EigenOptions<T>& opts,
                                      crd::memory::IAllocator* alloc)
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>,
                  "eigs_sym: v6-a is real symmetric (Hermitian via eig_herm is a follow-on)");
    namespace dn = crd::hesap::dense;
    using R = T; // real symmetric ⇒ RealType<T> == T

    EigenResult<T> result(alloc);
    const crd::u32 n = static_cast<crd::u32>(a.n_rows());
    result.n = n;
    if (n == 0 || opts.nev == 0)
    {
        return result;
    }

    // Subspace dim m (Ritz basis size), clamped to n.
    crd::u32 m = opts.ncv;
    if (m == 0)
    {
        const crd::u32 def = 2 * opts.nev + 1;
        m = def > 20U ? def : 20U;
    }
    if (m < opts.nev)
    {
        m = opts.nev;
    }
    if (m > n)
    {
        m = n;
    }

    crd::containers::Array<T> v(alloc); // n × (m+1) column-major Krylov basis
    v.resize(static_cast<crd::usize>(n) * (static_cast<crd::usize>(m) + 1));
    crd::containers::Array<R> alpha(alloc);
    crd::containers::Array<R> beta(alloc);
    alpha.resize(m);
    beta.resize(m);
    crd::containers::Array<T> w(alloc);
    w.resize(n);
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
    R amax = R{0}; // running max |alpha| — scale for the invariant-subspace break
    crd::u32 mdone = 0;
    R prev_beta = R{0};
    for (crd::u32 j = 0; j < m; ++j)
    {
        T* vj = col(j);
        (void)a.apply({vj, n}, {w.data(), n}); // w = A·vj
        if (j > 0)
        {
            dn::axpy<T>(static_cast<T>(-prev_beta), {col(j - 1), n}, {w.data(), n}); // w -= β_{j-1}·v_{j-1}
        }
        const T aj = dn::dot<T>({w.data(), n}, {vj, n}); // α_j = <w, v_j>
        alpha[j] = aj;
        dn::axpy<T>(-aj, {vj, n}, {w.data(), n}); // w -= α_j·v_j
        // FULL reorthogonalization (modified Gram-Schmidt, twice, fixed order 0..j).
        for (int pass = 0; pass < 2; ++pass)
        {
            for (crd::u32 i = 0; i <= j; ++i)
            {
                const T c = dn::dot<T>({w.data(), n}, {col(i), n});
                dn::axpy<T>(-c, {col(i), n}, {w.data(), n});
            }
        }
        const R bj = dn::nrm2<T>({w.data(), n});
        beta[j] = bj;
        mdone = j + 1;
        const R am = crd::math::fabs(aj);
        amax = am > amax ? am : amax;
        if (bj <= static_cast<R>(16) * eps * (amax > R{0} ? amax : R{1}))
        {
            break; // invariant subspace reached
        }
        T* vnext = col(j + 1);
        for (crd::u32 i = 0; i < n; ++i)
        {
            vnext[i] = w[i];
        }
        dn::scal<T>(static_cast<T>(R{1} / bj), {vnext, n});
        prev_beta = bj;
    }
    result.iterations = mdone;

    // Rayleigh-Ritz: dense eig_sym of the mdone×mdone tridiagonal T.
    dn::Symmetric<T> tri(alloc, mdone);
    for (crd::u32 j = 0; j < mdone; ++j)
    {
        tri.at(j, j) = static_cast<T>(alpha[j]);
        if (j + 1 < mdone)
        {
            tri.at(j + 1, j) = static_cast<T>(beta[j]);
        }
    }
    dn::EigSym<T> es = dn::eig_sym<T>(alloc, tri); // values ascending + Ritz vectors S
    const R* theta = es.values.data();

    // Select k = min(nev, mdone) Ritz indices by `which` (selection sort — k is small).
    const crd::u32 k = opts.nev < mdone ? opts.nev : mdone;
    crd::containers::Array<crd::u32> pick(alloc);
    pick.resize(mdone);
    for (crd::u32 i = 0; i < mdone; ++i)
    {
        pick[i] = i;
    }
    for (crd::u32 s = 0; s < k; ++s)
    {
        crd::u32 best = s;
        for (crd::u32 t = s + 1; t < mdone; ++t)
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

    result.values.resize(k);
    result.residuals.resize(k);
    if (opts.compute_vectors)
    {
        result.vectors.resize(static_cast<crd::usize>(n) * k);
    }
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> ax(alloc);
    x.resize(n);
    ax.resize(n);

    const R tol = opts.effective_tol();
    crd::u32 nconv = 0;
    for (crd::u32 s = 0; s < k; ++s)
    {
        const crd::u32 idx = pick[s];
        const R th = theta[idx];
        result.values[s] = crd::hesap::Complex<R>{th, R{0}};

        // x = V · S[:,idx]  (Ritz vector recovery).
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = T{0};
        }
        for (crd::u32 jj = 0; jj < mdone; ++jj)
        {
            dn::axpy<T>(es.vectors.at(jj, idx), {col(jj), n}, {x.data(), n});
        }
        const R xn = dn::nrm2<T>({x.data(), n});
        if (xn > R{0})
        {
            dn::scal<T>(static_cast<T>(R{1} / xn), {x.data(), n});
        }
        // SIGN convention: force the largest-magnitude component positive (moat — kills trivial flips).
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
        // True residual ‖A·x − θ·x‖ / ‖x‖ (‖x‖ == 1).
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
