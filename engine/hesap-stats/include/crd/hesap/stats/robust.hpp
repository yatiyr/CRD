#pragma once

// v12-p — Robust estimators (crd-hesap-stats). Theil-Sen slope · Hodges-Lehmann location · Huber & Tukey-biweight
// M-estimators (IRLS, MAD scale) · Huber proposal-2 joint location/scale · MCD covariance · Ledoit-Wolf / OAS shrinkage.
// Gold: scipy.stats.theilslopes · statsmodels RLM / robust.scale.huber · sklearn MinCovDet / LedoitWolf / OAS · MATLAB
// robustcov. Deterministic throughout (MCD uses exact subset enumeration), so gates are bit-for-bit / tolerance-exact.

#include <crd/hesap/stats/descriptive.hpp> // Real, mean, variance, detail::kPi
#include <crd/hesap/special/erf.hpp>       // erfc (normal cdf for the Huber proposal-2 gamma)

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::stats
{

template <Real T> struct SlopeResult
{
    T slope;
    T intercept;
};
template <Real T> struct LocationScale
{
    T location;
    T scale;
};

namespace detail
{
template <Real T> [[nodiscard]] inline T median_of_sorted(crd::containers::ConstSpan<T> s)
{
    const crd::usize n = s.size();
    return (n & 1U) ? s[n / 2] : static_cast<T>(0.5) * (s[n / 2 - 1] + s[n / 2]);
}
template <Real T> [[nodiscard]] inline T median_inplace(crd::containers::Span<T> v)
{
    crd::containers::sort(v.data(), v.data() + v.size());
    return median_of_sorted<T>(crd::containers::ConstSpan<T>{v.data(), v.size()});
}
// statsmodels MAD: median(|x - median(x)|) / 0.6744897501960817.
template <Real T> [[nodiscard]] inline T mad_scale(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> t(alloc);
    t.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        t[i] = x[i];
    }
    const T med = median_inplace(crd::containers::Span<T>{t.data(), n});
    for (crd::usize i = 0; i < n; ++i)
    {
        const T d = x[i] - med;
        t[i] = d < static_cast<T>(0) ? -d : d;
    }
    return median_inplace(crd::containers::Span<T>{t.data(), n}) / static_cast<T>(0.6744897501960817);
}
} // namespace detail

// Theil-Sen robust line. slope = median of pairwise slopes; intercept = median(y) - slope*median(x).
// scipy.stats.theilslopes.
template <Real T>
[[nodiscard]] SlopeResult<T> theil_sen(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                       crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> slopes(alloc);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = i + 1; j < n; ++j)
        {
            if (x[j] != x[i])
            {
                slopes.push_back((y[j] - y[i]) / (x[j] - x[i]));
            }
        }
    }
    const T slope = detail::median_inplace(crd::containers::Span<T>{slopes.data(), slopes.size()});
    crd::containers::Array<T> xc(alloc);
    crd::containers::Array<T> yc(alloc);
    xc.resize(n);
    yc.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xc[i] = x[i];
        yc[i] = y[i];
    }
    const T mx = detail::median_inplace(crd::containers::Span<T>{xc.data(), n});
    const T my = detail::median_inplace(crd::containers::Span<T>{yc.data(), n});
    return {slope, my - slope * mx};
}

// Hodges-Lehmann location: median of the Walsh averages (x_i + x_j)/2, i <= j.
template <Real T>
[[nodiscard]] T hodges_lehmann(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> w(alloc);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = i; j < n; ++j)
        {
            w.push_back(static_cast<T>(0.5) * (x[i] + x[j]));
        }
    }
    return detail::median_inplace(crd::containers::Span<T>{w.data(), w.size()});
}

namespace detail
{
// Shared IRLS location M-estimator: weight functor w(u) over u=(x-mu)/scale, MAD scale re-estimated each iteration.
template <Real T, typename Weight>
[[nodiscard]] T mestimate_location(crd::containers::ConstSpan<T> x, Weight w, crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    T mu = mean(x); // OLS init (X = ones)
    crd::containers::Array<T> ar(alloc);
    ar.resize(n);
    for (int it = 0; it < 100; ++it)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            const T d = x[i] - mu;
            ar[i] = d < static_cast<T>(0) ? -d : d;
        }
        // scale = median(|resid|)/0.6745 — residuals centered at the fit (statsmodels RLM convention, center=0)
        const T s = median_inplace(crd::containers::Span<T>{ar.data(), n}) / static_cast<T>(0.6744897501960817);
        if (s == static_cast<T>(0))
        {
            break;
        }
        T sw = static_cast<T>(0);
        T swx = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T wt = w((x[i] - mu) / s);
            sw += wt;
            swx += wt * x[i];
        }
        const T nmu = swx / sw;
        const T dm = nmu - mu;
        mu = nmu;
        if ((dm < static_cast<T>(0) ? -dm : dm) < static_cast<T>(1e-13))
        {
            break;
        }
    }
    return mu;
}
} // namespace detail

// Huber M-estimator of location (c=1.345). statsmodels RLM(y, ones, M=HuberT()).
template <Real T>
[[nodiscard]] T huber_location(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc,
                               T c = static_cast<T>(1.345))
{
    return detail::mestimate_location(
        x,
        [c](T u) {
            const T a = u < static_cast<T>(0) ? -u : u;
            return a <= c ? static_cast<T>(1) : c / a;
        },
        alloc);
}

// Tukey-biweight M-estimator of location (c=4.685). statsmodels RLM(y, ones, M=TukeyBiweight()).
template <Real T>
[[nodiscard]] T tukey_location(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc,
                               T c = static_cast<T>(4.685))
{
    return detail::mestimate_location(
        x,
        [c](T u) {
            const T a = u < static_cast<T>(0) ? -u : u;
            if (a > c)
            {
                return static_cast<T>(0);
            }
            const T r = u / c;
            const T t = static_cast<T>(1) - r * r;
            return t * t;
        },
        alloc);
}

// Huber proposal-2 joint location/scale (Venables-Ripley). Winsorized-mean location + a clipped-residual scale with the
// gamma consistency constant; iterated to convergence. statsmodels.robust.scale.huber (default c=1.5).
template <Real T>
[[nodiscard]] LocationScale<T> huber_proposal2(crd::containers::ConstSpan<T> a, crd::memory::IAllocator* alloc,
                                               T c = static_cast<T>(1.5))
{
    const crd::usize n = a.size();
    const T phi_c = crd::math::exp(-c * c / static_cast<T>(2)) / crd::math::sqrt(static_cast<T>(2) * detail::kPi<T>);
    const T cdf_c = static_cast<T>(0.5) * special::erfc(-c / crd::math::sqrt(static_cast<T>(2)));
    const T tmp = static_cast<T>(2) * cdf_c - static_cast<T>(1);
    const T gamma = tmp + c * c * (static_cast<T>(1) - tmp) - static_cast<T>(2) * c * phi_c;

    crd::containers::Array<T> t(alloc);
    t.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        t[i] = a[i];
    }
    T mu = detail::median_inplace(crd::containers::Span<T>{t.data(), n});
    T scale = detail::mad_scale(a, alloc); // standard MAD init (center = median)

    for (int it = 0; it < 50; ++it)
    {
        const T lo = mu - c * scale;
        const T hi = mu + c * scale;
        T sw = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T x = a[i];
            sw += (x < lo) ? lo : ((x > hi) ? hi : x); // Winsorized mean
        }
        const T nmu = sw / static_cast<T>(n);
        crd::usize card = 0;
        T scale_num = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T u = (a[i] - mu) / scale;
            if ((u < static_cast<T>(0) ? -u : u) <= c)
            {
                ++card;
                const T d = a[i] - nmu;
                scale_num += d * d;
            }
        }
        // n-1 in the gamma term (df correction for the estimated location); n in the count term — statsmodels convention
        const T scale_denom = static_cast<T>(n - 1) * gamma - static_cast<T>(n - card) * c * c;
        const T nscale = crd::math::sqrt(scale_num / scale_denom);
        const T dmu = nmu - mu;
        const T dsc = nscale - scale;
        mu = nmu;
        scale = nscale;
        if ((dmu < static_cast<T>(0) ? -dmu : dmu) < static_cast<T>(1e-12) &&
            (dsc < static_cast<T>(0) ? -dsc : dsc) < static_cast<T>(1e-12))
        {
            break;
        }
    }
    return {mu, scale};
}

} // namespace crd::hesap::stats
