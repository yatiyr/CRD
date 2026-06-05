#pragma once

// arnoldi.hpp — Phase 3.1.6 v6-c: NONSYMMETRIC ARNOLDI (the general eigenproblem A·x = λ·x, A not symmetric).
//
// Build an orthonormal Krylov basis V and an upper-HESSENBERG H = Vᵀ·A·V (full Gram-Schmidt, twice), then
// Rayleigh-Ritz = the dense nonsymmetric eigensolver `dense::eig(H)` ⇒ complex Ritz values θ + small Ritz
// vectors S. A real A yields COMPLEX-CONJUGATE eigenpairs. The Arnoldi residual estimate ‖A·(V·s) − θ·(V·s)‖
// = β_m·|s[m-1]| is cheap (no full eigenvector). This v6-c FIRST piece returns the wanted complex eigenvalues
// + residual estimates; the Krylov-Schur restart (Stewart 2001 ≡ IRAM, deterministic) and the complex
// eigenvector recovery are the v6-c completion.
//
// MOAT: serial Arnoldi + serial dense Rayleigh-Ritz; only `a.apply` (spmv) is parallel + bit-exact ⇒ the
// complex Ritz values are bit-identical across {1,2,4,8} workers.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/eig_nonsym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/eigen/lanczos.hpp> // detail::splitmix_pm1 + the spec
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::hesap::eigen
{
namespace detail
{
template <typename R> [[nodiscard]] inline R cmag(crd::hesap::Complex<R> z) noexcept
{
    return std::sqrt(z.re * z.re + z.im * z.im);
}

// `which` ordering for a COMPLEX Ritz value (nonsymmetric). Algebraic == real part.
template <typename R>
[[nodiscard]] inline bool more_wanted_c(Which w, crd::hesap::Complex<R> x, crd::hesap::Complex<R> y) noexcept
{
    switch (w)
    {
    case Which::LargestMagnitude:
        return cmag(x) > cmag(y);
    case Which::SmallestMagnitude:
        return cmag(x) < cmag(y);
    case Which::LargestReal:
    case Which::LargestAlgebraic:
        return x.re > y.re;
    case Which::SmallestReal:
    case Which::SmallestAlgebraic:
        return x.re < y.re;
    }
    return cmag(x) > cmag(y);
}
} // namespace detail

// Compute `opts.nev` eigenvalues of a general (nonsymmetric) REAL operator `a` (matrix-free) via Arnoldi.
// Returns the wanted COMPLEX eigenvalues (ordered by `which`) + the Arnoldi residual estimates; `nconv`
// counts those meeting the tolerance. (Eigenvectors + Krylov-Schur restart = the v6-c completion.)
template <typename T>
[[nodiscard]] EigenResult<T> eigs_nonsym(const crd::hesap::LinearOp<T>& a, const EigenOptions<T>& opts,
                                         crd::memory::IAllocator* alloc)
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>,
                  "eigs_nonsym: v6-c is real nonsymmetric (complex A is a follow-on)");
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
    if (m < opts.nev + 1)
    {
        m = opts.nev + 1;
    }
    if (m > n)
    {
        m = n;
    }

    crd::containers::Array<T> v(alloc); // n × (m+1)
    v.resize(static_cast<crd::usize>(n) * (static_cast<crd::usize>(m) + 1));
    crd::containers::Array<T> w(alloc);
    w.resize(n);
    auto col = [&](crd::u32 j) noexcept -> T* { return v.data() + static_cast<crd::usize>(j) * n; };

    dn::Matrix<T> h(alloc, m, m); // upper Hessenberg (zeroed below)
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
    R amax = R{0};
    R beta_m = R{0};
    crd::u32 mdone = 0;
    for (crd::u32 j = 0; j < m; ++j)
    {
        T* vj = col(j);
        (void)a.apply({vj, n}, {w.data(), n}); // w = A·v_j
        // Full Gram-Schmidt orthogonalization (twice; accumulate the projections into H).
        for (int pass = 0; pass < 2; ++pass)
        {
            for (crd::u32 i = 0; i <= j; ++i)
            {
                const T c = dn::dot<T>({col(i), n}, {w.data(), n});
                h.at(i, j) += c;
                dn::axpy<T>(-c, {col(i), n}, {w.data(), n});
            }
        }
        R beta = dn::nrm2<T>({w.data(), n});
        const R hm = std::fabs(static_cast<R>(h.at(j, j)));
        amax = hm > amax ? hm : amax;
        mdone = j + 1;
        beta_m = beta;
        if (beta <= static_cast<R>(16) * eps * (amax > R{0} ? amax : R{1}))
        {
            break; // invariant subspace
        }
        if (j + 1 < m)
        {
            h.at(j + 1, j) = static_cast<T>(beta); // Hessenberg subdiagonal
        }
        T* vnext = col(j + 1);
        for (crd::u32 i = 0; i < n; ++i)
        {
            vnext[i] = w[i];
        }
        dn::scal<T>(static_cast<T>(R{1} / beta), {vnext, n});
    }
    result.iterations = mdone;

    // Rayleigh-Ritz: dense nonsymmetric eigendecomposition of the leading mdone×mdone Hessenberg.
    dn::Matrix<T> hsub(alloc, mdone, mdone);
    for (crd::u32 i = 0; i < mdone; ++i)
    {
        for (crd::u32 j = 0; j < mdone; ++j)
        {
            hsub.at(i, j) = h.at(i, j);
        }
    }
    dn::EigNonsym<T> es = dn::eig<T>(alloc, hsub); // complex values + complex Ritz vectors (Schur order)
    const crd::u32 mm = mdone;

    // Select the nev wanted by `which` (selection sort on the complex Ritz values).
    crd::containers::Array<crd::u32> pick(alloc);
    pick.resize(mm);
    for (crd::u32 i = 0; i < mm; ++i)
    {
        pick[i] = i;
    }
    const crd::u32 k = opts.nev < mm ? opts.nev : mm;
    for (crd::u32 s = 0; s < k; ++s)
    {
        crd::u32 best = s;
        for (crd::u32 t = s + 1; t < mm; ++t)
        {
            if (detail::more_wanted_c<R>(opts.which, es.values.data()[pick[t]], es.values.data()[pick[best]]))
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
    const R tol = opts.effective_tol();
    crd::u32 nconv = 0;
    for (crd::u32 s = 0; s < k; ++s)
    {
        const crd::u32 idx = pick[s];
        const C th = es.values.data()[idx];
        result.values[s] = th;
        // Arnoldi residual estimate β_m·|s[m-1]| (s = the idx-th Ritz vector of H).
        const C slast = es.vectors.at(mm - 1, idx);
        const R rest = beta_m * detail::cmag<R>(slast);
        result.residuals[s] = rest;
        const R scale = detail::cmag<R>(th);
        if (rest <= tol * (scale > R{1} ? scale : R{1}))
        {
            ++nconv;
        }
    }
    result.nconv = nconv;
    result.converged = nconv >= opts.nev;
    return result;
}

} // namespace crd::hesap::eigen
