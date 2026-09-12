#pragma once

// crd-hesap-stats v12-m — descriptive sample statistics over a data span. Moments follow scipy.stats conventions
// (skewness = Fisher-Pearson g1, biased; kurtosis = Fisher excess, biased), so they gate bit-for-bit against
// scipy. Quantiles (R types 1-9 + Harrell-Davis), ECDF, histograms, weighted variants, and covariance/correlation
// matrices land in this file as v12-m proceeds. Gold: scipy.stats · R quantile(type=) · MATLAB quantile/prctile.

#include <crd/hesap/stats/distribution.hpp> // the Real concept + the crd::hesap::stats namespace

#include <crd/hesap/special/gamma.hpp>      // lbeta — for the Harrell-Davis beta weights
#include <crd/hesap/special/incomplete.hpp> // betainc

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp> // crd stable_sort (the no-std-sort guard forbids std::sort)
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::stats
{

// ───────────────────────────── moments (two-pass, numerically stable) ─────────────────────────────
template <Real T>
[[nodiscard]] T mean(crd::containers::Span<const T> x) noexcept
{
    T s = static_cast<T>(0);
    for (T v : x)
    {
        s += v;
    }
    return s / static_cast<T>(x.size());
}

// var with delta-degrees-of-freedom: ddof=0 → population (numpy default), ddof=1 → sample (unbiased).
template <Real T>
[[nodiscard]] T variance(crd::containers::Span<const T> x, int ddof = 0) noexcept
{
    const T m = mean(x);
    T s2 = static_cast<T>(0);
    for (T v : x)
    {
        const T d = v - m;
        s2 += d * d;
    }
    return s2 / static_cast<T>(x.size() - static_cast<crd::usize>(ddof));
}

template <Real T>
[[nodiscard]] T stddev(crd::containers::Span<const T> x, int ddof = 0) noexcept
{
    return crd::math::sqrt(variance(x, ddof));
}

// Fisher-Pearson g1 = m3 / m2^{3/2} (biased) — scipy.stats.skew default.
template <Real T>
[[nodiscard]] T skewness(crd::containers::Span<const T> x) noexcept
{
    const T m = mean(x);
    T m2 = static_cast<T>(0);
    T m3 = static_cast<T>(0);
    for (T v : x)
    {
        const T d = v - m;
        const T d2 = d * d;
        m2 += d2;
        m3 += d2 * d;
    }
    const T n = static_cast<T>(x.size());
    m2 /= n;
    m3 /= n;
    return m3 / (m2 * crd::math::sqrt(m2));
}

// Fisher excess kurtosis = m4 / m2² − 3 (biased) — scipy.stats.kurtosis default.
template <Real T>
[[nodiscard]] T kurtosis(crd::containers::Span<const T> x) noexcept
{
    const T m = mean(x);
    T m2 = static_cast<T>(0);
    T m4 = static_cast<T>(0);
    for (T v : x)
    {
        const T d = v - m;
        const T d2 = d * d;
        m2 += d2;
        m4 += d2 * d2;
    }
    const T n = static_cast<T>(x.size());
    m2 /= n;
    m4 /= n;
    return m4 / (m2 * m2) - static_cast<T>(3);
}

// ───────────────────────────── quantiles (Hyndman-Fan / R types 1-9 = numpy methods) ─────────────────────────────
// `x` is ASCENDING-sorted. p in [0,1]. type in 1..9. Continuous (4-9): 0-indexed virtual index
// v = p·(n−α−β+1) + α − 1, linear-interpolate. Discontinuous (1,2): v = n·p − 1, fix γ. Type 3 = nearest even
// order statistic (round half to even). Matches numpy.quantile(method=) / R quantile(type=) bit-for-bit.
template <Real T>
[[nodiscard]] T quantile_sorted(crd::containers::Span<const T> x, T p, int type) noexcept
{
    const crd::isize n = static_cast<crd::isize>(x.size());
    if (n <= 1 || p <= static_cast<T>(0))
    {
        return x[0];
    }
    if (p >= static_cast<T>(1))
    {
        return x[static_cast<crd::usize>(n - 1)];
    }
    const double pd = static_cast<double>(p);
    const double nd = static_cast<double>(n);
    if (type == 3) // closest observation, round half to even (1-indexed)
    {
        crd::isize j = static_cast<crd::isize>(crd::math::nearbyint(nd * pd));
        j = j < 1 ? 1 : (j > n ? n : j);
        return x[static_cast<crd::usize>(j - 1)];
    }
    double v = 0.0;
    if (type == 1 || type == 2)
    {
        v = nd * pd - 1.0;
    }
    else
    {
        double a = 1.0;
        double b = 1.0; // type 7 (linear, default)
        switch (type)
        {
        case 4: a = 0.0; b = 1.0; break;
        case 5: a = 0.5; b = 0.5; break;
        case 6: a = 0.0; b = 0.0; break;
        case 8: a = 1.0 / 3.0; b = 1.0 / 3.0; break;
        case 9: a = 3.0 / 8.0; b = 3.0 / 8.0; break;
        default: break; // 7
        }
        v = pd * (nd - a - b + 1.0) + a - 1.0;
    }
    const crd::isize j0 = static_cast<crd::isize>(crd::math::floor(v));
    double g = v - static_cast<double>(j0);
    if (type == 1)
    {
        g = g > 0.0 ? 1.0 : 0.0;
    }
    else if (type == 2)
    {
        g = g > 0.0 ? 1.0 : 0.5;
    }
    if (j0 < 0)
    {
        return x[0];
    }
    if (j0 >= n - 1)
    {
        return x[static_cast<crd::usize>(n - 1)];
    }
    const crd::usize j = static_cast<crd::usize>(j0);
    return x[j] + static_cast<T>(g) * (x[j + 1] - x[j]);
}

// Convenience: sorts a copy (crd stable_sort) then evaluates. For many quantiles, sort once + use quantile_sorted.
template <Real T>
[[nodiscard]] T quantile(crd::containers::Span<const T> x, T p, int type, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> s(alloc);
    s.resize(x.size());
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        s[i] = x[i];
    }
    crd::containers::stable_sort(s.data(), s.data() + s.size(), alloc);
    return quantile_sorted<T>(crd::containers::Span<const T>{s.data(), s.size()}, p, type);
}

// Harrell-Davis quantile — a smooth, efficient estimator: a beta-weighted sum of order statistics,
// W_i = I(i/n; a, b) − I((i−1)/n; a, b), a = (n+1)p, b = (n+1)(1−p), I = regularized incomplete beta. `x` sorted.
// Gold: scipy.stats.mstats.hdquantiles. Reuses the shipped special::betainc (SANITY 8).
template <Real T>
[[nodiscard]] T harrell_davis_sorted(crd::containers::Span<const T> x, T p) noexcept
{
    const crd::usize n = x.size();
    const T a = static_cast<T>(n + 1) * p;
    const T b = static_cast<T>(n + 1) * (static_cast<T>(1) - p);
    const T lb = special::lbeta(a, b);
    T q = static_cast<T>(0);
    T prev = static_cast<T>(0); // I(0; a, b) = 0
    for (crd::usize i = 1; i <= n; ++i)
    {
        const T cur = special::betainc(a, b, static_cast<T>(i) / static_cast<T>(n), lb);
        q += (cur - prev) * x[i - 1];
        prev = cur;
    }
    return q;
}

// ───────────────────────────── covariance / correlation matrices ─────────────────────────────
// `data` is p variables × n observations, row-major (row i = variable i). `out` is p×p row-major.
template <Real T>
void covariance_matrix(crd::containers::Span<const T> data, crd::usize p, crd::usize n, crd::containers::Span<T> out,
                       int ddof = 1) noexcept
{
    const T denom = static_cast<T>(n - static_cast<crd::usize>(ddof));
    for (crd::usize i = 0; i < p; ++i)
    {
        T mi = static_cast<T>(0);
        for (crd::usize k = 0; k < n; ++k)
        {
            mi += data[i * n + k];
        }
        mi /= static_cast<T>(n);
        for (crd::usize j = 0; j < p; ++j)
        {
            T mj = static_cast<T>(0);
            for (crd::usize k = 0; k < n; ++k)
            {
                mj += data[j * n + k];
            }
            mj /= static_cast<T>(n);
            T s = static_cast<T>(0);
            for (crd::usize k = 0; k < n; ++k)
            {
                s += (data[i * n + k] - mi) * (data[j * n + k] - mj);
            }
            out[i * p + j] = s / denom;
        }
    }
}

template <Real T>
void correlation_matrix(crd::containers::Span<const T> data, crd::usize p, crd::usize n,
                        crd::containers::Span<T> out) noexcept
{
    for (crd::usize i = 0; i < p; ++i)
    {
        T mi = static_cast<T>(0);
        for (crd::usize k = 0; k < n; ++k)
        {
            mi += data[i * n + k];
        }
        mi /= static_cast<T>(n);
        for (crd::usize j = 0; j < p; ++j)
        {
            T mj = static_cast<T>(0);
            for (crd::usize k = 0; k < n; ++k)
            {
                mj += data[j * n + k];
            }
            mj /= static_cast<T>(n);
            T sij = static_cast<T>(0);
            T sii = static_cast<T>(0);
            T sjj = static_cast<T>(0);
            for (crd::usize k = 0; k < n; ++k)
            {
                const T di = data[i * n + k] - mi;
                const T dj = data[j * n + k] - mj;
                sij += di * dj;
                sii += di * di;
                sjj += dj * dj;
            }
            out[i * p + j] = sij / crd::math::sqrt(sii * sjj); // Pearson; ddof cancels
        }
    }
}

// ───────────────────────────── robust / order-based / weighted / ECDF / histogram ─────────────────────────────
template <Real T>
[[nodiscard]] T median_sorted(crd::containers::Span<const T> x) noexcept
{
    return quantile_sorted<T>(x, static_cast<T>(0.5), 7);
}

template <Real T>
[[nodiscard]] T iqr_sorted(crd::containers::Span<const T> x) noexcept // Q3−Q1 (type 7) — scipy.stats.iqr
{
    return quantile_sorted<T>(x, static_cast<T>(0.75), 7) - quantile_sorted<T>(x, static_cast<T>(0.25), 7);
}

// scipy.stats.trim_mean: drop ⌊prop·n⌋ from each tail, mean the rest. x ascending-sorted.
template <Real T>
[[nodiscard]] T trimmed_mean_sorted(crd::containers::Span<const T> x, T proportiontocut) noexcept
{
    const crd::usize n = x.size();
    const crd::usize cut = static_cast<crd::usize>(static_cast<double>(proportiontocut) * static_cast<double>(n));
    T s = static_cast<T>(0);
    for (crd::usize i = cut; i < n - cut; ++i)
    {
        s += x[i];
    }
    return s / static_cast<T>(n - 2 * cut);
}

// Median absolute deviation = scale·median(|x − median(x)|). scipy.stats.median_abs_deviation (default scale=1).
template <Real T>
[[nodiscard]] T median_abs_deviation(crd::containers::Span<const T> sorted_x, crd::memory::IAllocator* alloc,
                                     T scale = static_cast<T>(1))
{
    const T med = median_sorted(sorted_x);
    crd::containers::Array<T> dev(alloc);
    dev.resize(sorted_x.size());
    for (crd::usize i = 0; i < sorted_x.size(); ++i)
    {
        dev[i] = crd::math::fabs(sorted_x[i] - med);
    }
    crd::containers::stable_sort(dev.data(), dev.data() + dev.size(), alloc);
    return scale * median_sorted(crd::containers::Span<const T>{dev.data(), dev.size()});
}

template <Real T>
[[nodiscard]] T weighted_mean(crd::containers::Span<const T> x, crd::containers::Span<const T> w) noexcept
{
    T sw = static_cast<T>(0);
    T swx = static_cast<T>(0);
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        sw += w[i];
        swx += w[i] * x[i];
    }
    return swx / sw;
}

// Reliability-weighted variance: Σ w(x−x̄_w)² / Σ w (biased) — matches np.average((x−wm)², weights=w).
template <Real T>
[[nodiscard]] T weighted_variance(crd::containers::Span<const T> x, crd::containers::Span<const T> w) noexcept
{
    const T m = weighted_mean(x, w);
    T sw = static_cast<T>(0);
    T swd = static_cast<T>(0);
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        const T d = x[i] - m;
        sw += w[i];
        swd += w[i] * d * d;
    }
    return swd / sw;
}

// Empirical CDF: fraction of (sorted) data ≤ q.
template <Real T>
[[nodiscard]] T ecdf_sorted(crd::containers::Span<const T> sorted_x, T q) noexcept
{
    crd::usize c = 0;
    for (T v : sorted_x)
    {
        if (v <= q)
        {
            ++c;
        }
    }
    return static_cast<T>(c) / static_cast<T>(sorted_x.size());
}

// Histogram bin rules: Sturges (count), Freedman-Diaconis & Scott (width). Match numpy's underlying formulas.
[[nodiscard]] inline crd::usize sturges_bins(crd::usize n) noexcept
{
    return static_cast<crd::usize>(crd::math::ceil(crd::math::log2(static_cast<double>(n)))) + 1;
}
template <Real T>
[[nodiscard]] T fd_bin_width_sorted(crd::containers::Span<const T> x) noexcept // 2·IQR/n^{1/3}
{
    return static_cast<T>(2) * iqr_sorted(x) / crd::math::cbrt(static_cast<T>(x.size()));
}
template <Real T>
[[nodiscard]] T scott_bin_width(crd::containers::Span<const T> x) noexcept // (24√π/n)^{1/3}·σ
{
    return crd::math::cbrt(static_cast<T>(24) * crd::math::sqrt(detail::kPi<T>) / static_cast<T>(x.size())) *
           stddev(x, 0);
}

} // namespace crd::hesap::stats
