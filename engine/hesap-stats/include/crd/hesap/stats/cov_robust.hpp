#pragma once

// v12-p — Robust / shrinkage covariance (crd-hesap-stats). Ledoit-Wolf & OAS analytic shrinkage toward a scaled
// identity, plus the exact Minimum-Covariance-Determinant (exhaustive h-subset enumeration → the gold-standard MCD that
// FastMCD approximates). Matrices are p x p row-major. Gold: sklearn.covariance.{ledoit_wolf,oas,MinCovDet} · MATLAB
// robustcov. Deterministic (exact MCD), so gates are tolerance-exact.

#include <crd/hesap/stats/descriptive.hpp> // Real

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::hesap::stats
{

template <Real T> struct ShrinkageResult
{
    crd::containers::Array<T> cov; // p*p row-major shrunk covariance
    T shrinkage;
};
template <Real T> struct McdResult
{
    crd::containers::Array<T> location; // p
    crd::containers::Array<T> cov;       // p*p row-major (sample cov, ddof=1, of the best subset)
    T determinant;
};

namespace detail
{
// Empirical covariance of X (n x p row-major) into cov (p*p row-major), divisor `denom` (n for MLE, n-1 for sample).
template <Real T>
inline void empirical_cov(crd::containers::ConstSpan<T> x, crd::usize n, crd::usize p, T denom, T* mean, T* cov)
{
    for (crd::usize j = 0; j < p; ++j)
    {
        T m = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            m += x[i * p + j];
        }
        mean[j] = m / static_cast<T>(n);
    }
    for (crd::usize a = 0; a < p; ++a)
    {
        for (crd::usize b = a; b < p; ++b)
        {
            T s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                s += (x[i * p + a] - mean[a]) * (x[i * p + b] - mean[b]);
            }
            cov[a * p + b] = cov[b * p + a] = s / denom;
        }
    }
}

// Determinant of a p x p row-major matrix via Gaussian elimination with partial pivoting (A is overwritten).
template <Real T> [[nodiscard]] inline T det_lu(T* a, crd::usize p)
{
    T det = static_cast<T>(1);
    for (crd::usize k = 0; k < p; ++k)
    {
        crd::usize piv = k;
        T best = a[k * p + k] < static_cast<T>(0) ? -a[k * p + k] : a[k * p + k];
        for (crd::usize i = k + 1; i < p; ++i)
        {
            const T v = a[i * p + k] < static_cast<T>(0) ? -a[i * p + k] : a[i * p + k];
            if (v > best)
            {
                best = v;
                piv = i;
            }
        }
        if (best == static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        if (piv != k)
        {
            for (crd::usize j = 0; j < p; ++j)
            {
                const T tmp = a[k * p + j];
                a[k * p + j] = a[piv * p + j];
                a[piv * p + j] = tmp;
            }
            det = -det;
        }
        det *= a[k * p + k];
        for (crd::usize i = k + 1; i < p; ++i)
        {
            const T f = a[i * p + k] / a[k * p + k];
            for (crd::usize j = k; j < p; ++j)
            {
                a[i * p + j] -= f * a[k * p + j];
            }
        }
    }
    return det;
}

[[nodiscard]] inline bool next_combination(crd::containers::Span<crd::usize> idx, crd::usize n)
{
    const crd::usize k = idx.size();
    crd::usize i = k;
    while (i > 0)
    {
        --i;
        if (idx[i] != i + n - k)
        {
            ++idx[i];
            for (crd::usize j = i + 1; j < k; ++j)
            {
                idx[j] = idx[j - 1] + 1;
            }
            return true;
        }
    }
    return false;
}
} // namespace detail

// Ledoit-Wolf shrinkage covariance (shrinkage toward mu*I). sklearn.covariance.ledoit_wolf.
template <Real T>
[[nodiscard]] ShrinkageResult<T> ledoit_wolf(crd::containers::ConstSpan<T> x, crd::usize n, crd::usize p,
                                             crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> mean(alloc);
    crd::containers::Array<T> emp(alloc);
    mean.resize(p);
    emp.resize(p * p);
    detail::empirical_cov(x, n, p, static_cast<T>(n), mean.data(), emp.data()); // MLE (/n)
    T trace = static_cast<T>(0);
    T delta_ = static_cast<T>(0);
    for (crd::usize a = 0; a < p; ++a)
    {
        trace += emp[a * p + a];
    }
    for (crd::usize i = 0; i < p * p; ++i)
    {
        delta_ += emp[i] * emp[i]; // ||emp_cov||_F^2
    }
    const T mu = trace / static_cast<T>(p);
    // beta_ = sum of entries of (X2^T X2), X2 = centered-X squared elementwise
    T beta_ = static_cast<T>(0);
    for (crd::usize a = 0; a < p; ++a)
    {
        for (crd::usize b = 0; b < p; ++b)
        {
            T s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T xa = (x[i * p + a] - mean[a]) * (x[i * p + a] - mean[a]);
                const T xb = (x[i * p + b] - mean[b]) * (x[i * p + b] - mean[b]);
                s += xa * xb;
            }
            beta_ += s;
        }
    }
    T beta = (static_cast<T>(1) / (static_cast<T>(p) * static_cast<T>(n))) * (beta_ / static_cast<T>(n) - delta_);
    T delta = (delta_ - static_cast<T>(2) * mu * trace + static_cast<T>(p) * mu * mu) / static_cast<T>(p);
    if (beta > delta)
    {
        beta = delta;
    }
    const T shrinkage = (beta == static_cast<T>(0)) ? static_cast<T>(0) : beta / delta;
    crd::containers::Array<T> cov(alloc);
    cov.resize(p * p);
    for (crd::usize i = 0; i < p * p; ++i)
    {
        cov[i] = (static_cast<T>(1) - shrinkage) * emp[i];
    }
    for (crd::usize a = 0; a < p; ++a)
    {
        cov[a * p + a] += shrinkage * mu;
    }
    return {static_cast<crd::containers::Array<T>&&>(cov), shrinkage};
}

// Oracle Approximating Shrinkage covariance. sklearn.covariance.oas.
template <Real T>
[[nodiscard]] ShrinkageResult<T> oas(crd::containers::ConstSpan<T> x, crd::usize n, crd::usize p,
                                     crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> mean(alloc);
    crd::containers::Array<T> emp(alloc);
    mean.resize(p);
    emp.resize(p * p);
    detail::empirical_cov(x, n, p, static_cast<T>(n), mean.data(), emp.data());
    T trace = static_cast<T>(0);
    T sumsq = static_cast<T>(0);
    for (crd::usize a = 0; a < p; ++a)
    {
        trace += emp[a * p + a];
    }
    for (crd::usize i = 0; i < p * p; ++i)
    {
        sumsq += emp[i] * emp[i];
    }
    const T mu = trace / static_cast<T>(p);
    const T alpha = sumsq / static_cast<T>(p * p); // mean(emp_cov^2)
    const T num = alpha + mu * mu;
    const T den = static_cast<T>(n + 1) * (alpha - (mu * mu) / static_cast<T>(p));
    T shrinkage = (den == static_cast<T>(0)) ? static_cast<T>(1) : num / den;
    if (shrinkage > static_cast<T>(1))
    {
        shrinkage = static_cast<T>(1);
    }
    crd::containers::Array<T> cov(alloc);
    cov.resize(p * p);
    for (crd::usize i = 0; i < p * p; ++i)
    {
        cov[i] = (static_cast<T>(1) - shrinkage) * emp[i];
    }
    for (crd::usize a = 0; a < p; ++a)
    {
        cov[a * p + a] += shrinkage * mu;
    }
    return {static_cast<crd::containers::Array<T>&&>(cov), shrinkage};
}

// Exact Minimum-Covariance-Determinant: the h-subset (h = floor((n+p+1)/2)) whose sample covariance (ddof=1) has the
// smallest determinant; returns its mean + covariance. The gold-standard MCD (FastMCD approximates it).
template <Real T>
[[nodiscard]] McdResult<T> mcd_exact(crd::containers::ConstSpan<T> x, crd::usize n, crd::usize p,
                                     crd::memory::IAllocator* alloc)
{
    const crd::usize h = (n + p + 1) / 2;
    crd::containers::Array<crd::usize> idx(alloc);
    crd::containers::Array<T> sub(alloc);
    crd::containers::Array<T> mean(alloc);
    crd::containers::Array<T> cov(alloc);
    crd::containers::Array<T> work(alloc);
    idx.resize(h);
    sub.resize(h * p);
    mean.resize(p);
    cov.resize(p * p);
    work.resize(p * p);
    for (crd::usize i = 0; i < h; ++i)
    {
        idx[i] = i;
    }
    T best_det = std::numeric_limits<T>::infinity();
    crd::containers::Array<crd::usize> best(alloc);
    best.resize(h);
    do
    {
        for (crd::usize i = 0; i < h; ++i)
        {
            for (crd::usize j = 0; j < p; ++j)
            {
                sub[i * p + j] = x[idx[i] * p + j];
            }
        }
        detail::empirical_cov(crd::containers::ConstSpan<T>{sub.data(), h * p}, h, p, static_cast<T>(h - 1),
                              mean.data(), cov.data());
        for (crd::usize i = 0; i < p * p; ++i)
        {
            work[i] = cov[i];
        }
        const T d = detail::det_lu(work.data(), p);
        if (d < best_det)
        {
            best_det = d;
            for (crd::usize i = 0; i < h; ++i)
            {
                best[i] = idx[i];
            }
        }
    } while (detail::next_combination(crd::containers::Span<crd::usize>{idx.data(), h}, n));
    for (crd::usize i = 0; i < h; ++i)
    {
        for (crd::usize j = 0; j < p; ++j)
        {
            sub[i * p + j] = x[best[i] * p + j];
        }
    }
    crd::containers::Array<T> loc(alloc);
    crd::containers::Array<T> bcov(alloc);
    loc.resize(p);
    bcov.resize(p * p);
    detail::empirical_cov(crd::containers::ConstSpan<T>{sub.data(), h * p}, h, p, static_cast<T>(h - 1), loc.data(),
                          bcov.data());
    return {static_cast<crd::containers::Array<T>&&>(loc), static_cast<crd::containers::Array<T>&&>(bcov), best_det};
}

} // namespace crd::hesap::stats
