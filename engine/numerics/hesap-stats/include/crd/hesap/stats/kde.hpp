#pragma once

// v12-p — Kernel density estimation (crd-hesap-stats). Gaussian + Epanechnikov kernels with Scott / Silverman / rule-of-
// thumb / leave-one-out-CV bandwidth selection. The Gaussian path matches scipy.stats.gaussian_kde bit-for-bit (it is a
// deterministic sum of kernels); bandwidth selectors match scipy/statsmodels formulas. Gold: scipy.stats.gaussian_kde ·
// statsmodels KDEUnivariate · MATLAB ksdensity · R density.

#include <crd/hesap/stats/descriptive.hpp> // Real, mean, variance, quantile_sorted (IQR for the rule-of-thumb)

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::hesap::stats
{

enum class KdeKernel
{
    Gaussian,
    Epanechnikov
};

// scipy.stats.gaussian_kde bandwidth FACTORS (1-D): the effective bandwidth is factor * std(ddof=1).
template <Real T> [[nodiscard]] T kde_scott_factor(crd::usize n)
{
    return crd::math::pow(static_cast<T>(n), static_cast<T>(-0.2)); // n^(-1/(d+4)), d=1
}
template <Real T> [[nodiscard]] T kde_silverman_factor(crd::usize n)
{
    return crd::math::pow(static_cast<T>(n) * static_cast<T>(0.75), static_cast<T>(-0.2)); // (n(d+2)/4)^(-1/(d+4))
}

// Effective Scott / Silverman bandwidth (scipy gaussian_kde): factor * sample std.
template <Real T> [[nodiscard]] T kde_bandwidth_scott(crd::containers::ConstSpan<T> data)
{
    return kde_scott_factor<T>(data.size()) * crd::math::sqrt(variance(data, 1));
}
template <Real T> [[nodiscard]] T kde_bandwidth_silverman(crd::containers::ConstSpan<T> data)
{
    return kde_silverman_factor<T>(data.size()) * crd::math::sqrt(variance(data, 1));
}

// Rule-of-thumb Silverman bandwidth (statsmodels bw_silverman): 0.9 min(std, IQR/1.349) n^(-1/5).
template <Real T>
[[nodiscard]] T bw_silverman_rot(crd::containers::ConstSpan<T> data, crd::memory::IAllocator* alloc)
{
    const crd::usize n = data.size();
    crd::containers::Array<T> s(alloc);
    s.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        s[i] = data[i];
    }
    crd::containers::sort(s.data(), s.data() + n);
    const crd::containers::Span<const T> ss{s.data(), n};
    const T iqr = quantile_sorted<T>(ss, static_cast<T>(0.75), 7) - quantile_sorted<T>(ss, static_cast<T>(0.25), 7);
    const T sd = crd::math::sqrt(variance(data, 1));
    const T spread = (iqr / static_cast<T>(1.349) < sd) ? iqr / static_cast<T>(1.349) : sd;
    return static_cast<T>(0.9) * spread * crd::math::pow(static_cast<T>(n), static_cast<T>(-0.2));
}

// Kernel value (unit bandwidth): Gaussian standard pdf or Epanechnikov 0.75(1-u^2) on |u|<1.
template <Real T> [[nodiscard]] inline T kde_kernel(T u, KdeKernel kernel)
{
    if (kernel == KdeKernel::Gaussian)
    {
        constexpr T inv_sqrt_2pi = static_cast<T>(0.398942280401432677939946059934);
        return inv_sqrt_2pi * crd::math::exp(static_cast<T>(-0.5) * u * u);
    }
    const T a = u < static_cast<T>(0) ? -u : u;
    return (a < static_cast<T>(1)) ? static_cast<T>(0.75) * (static_cast<T>(1) - u * u) : static_cast<T>(0);
}

// KDE density estimate at x with bandwidth h: (1/(n h)) sum_i K((x - data_i)/h).
template <Real T>
[[nodiscard]] T kde_eval(crd::containers::ConstSpan<T> data, T x, T h, KdeKernel kernel = KdeKernel::Gaussian)
{
    T acc = static_cast<T>(0);
    for (T xi : data)
    {
        acc += kde_kernel((x - xi) / h, kernel);
    }
    return acc / (static_cast<T>(data.size()) * h);
}

// Leave-one-out maximum-likelihood CV bandwidth over a uniform grid [lo, hi] (Gaussian kernel): returns the grid point
// maximizing sum_i log( mean_{j != i} K((x_i - x_j)/h)/h ).
template <Real T>
[[nodiscard]] T bw_cv(crd::containers::ConstSpan<T> data, T lo, T hi, crd::usize steps,
                      crd::memory::IAllocator* /*alloc*/)
{
    const crd::usize n = data.size();
    T best_h = lo;
    T best_ll = std::numeric_limits<T>::lowest();
    for (crd::usize g = 0; g < steps; ++g)
    {
        const T h = lo + static_cast<T>(g) * (hi - lo) / static_cast<T>(steps - 1);
        T ll = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            T acc = static_cast<T>(0);
            for (crd::usize j = 0; j < n; ++j)
            {
                if (j != i)
                {
                    acc += kde_kernel((data[i] - data[j]) / h, KdeKernel::Gaussian);
                }
            }
            ll += crd::math::log(acc / (static_cast<T>(n - 1) * h));
        }
        if (ll > best_ll)
        {
            best_ll = ll;
            best_h = h;
        }
    }
    return best_h;
}

} // namespace crd::hesap::stats
