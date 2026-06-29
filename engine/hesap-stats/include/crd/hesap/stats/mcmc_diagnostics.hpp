#pragma once

// v12-q — MCMC convergence diagnostics (crd-hesap-stats). Rank-normalized split-R-hat (Vehtari et al. 2021) ·
// bulk effective sample size (Stan/Geyer initial-positive+monotone) · autocorrelation · Geweke z-score. Deterministic
// given the chains → gated bit-for-bit vs ArviZ az.rhat / az.ess on reproducible chains. Gold: ArviZ · Stan.

#include <crd/hesap/stats/descriptive.hpp> // Real, mean, variance
#include <crd/hesap/special/erf.hpp>       // ndtri (rank normalization)

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::stats
{

// Autocorrelation at a given lag (biased estimator, ArviZ/FFT normalization): sum_t (x_t-m)(x_{t+k}-m) / sum_t (x_t-m)^2.
template <Real T> [[nodiscard]] T autocorr(crd::containers::ConstSpan<T> x, crd::usize lag)
{
    const crd::usize n = x.size();
    const T m = mean(x);
    T c0 = static_cast<T>(0);
    for (crd::usize t = 0; t < n; ++t)
    {
        const T d = x[t] - m;
        c0 += d * d;
    }
    T ck = static_cast<T>(0);
    for (crd::usize t = 0; t + lag < n; ++t)
    {
        ck += (x[t] - m) * (x[t + lag] - m);
    }
    return ck / c0;
}

// Geweke z-score: standardized difference of the mean over the first `fa` and last `fb` fractions of a chain.
template <Real T>
[[nodiscard]] T geweke(crd::containers::ConstSpan<T> x, T fa = static_cast<T>(0.1), T fb = static_cast<T>(0.5))
{
    const crd::usize n = x.size();
    const crd::usize n1 = static_cast<crd::usize>(fa * static_cast<T>(n));
    const crd::usize n2 = static_cast<crd::usize>(fb * static_cast<T>(n));
    const crd::containers::ConstSpan<T> first{x.data(), n1};
    const crd::containers::ConstSpan<T> last{x.data() + (n - n2), n2};
    const T m1 = mean(first);
    const T m2 = mean(last);
    const T v1 = variance(first, 1);
    const T v2 = variance(last, 1);
    return (m1 - m2) / crd::math::sqrt(v1 / static_cast<T>(n1) + v2 / static_cast<T>(n2));
}

namespace detail
{
// Average ranks (1-based) of x → out.
template <Real T>
void average_ranks(crd::containers::ConstSpan<T> x, crd::containers::Span<T> out, crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    crd::containers::Array<crd::usize> idx(alloc);
    idx.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        idx[i] = i;
    }
    crd::containers::sort(idx.data(), idx.data() + n, [&](crd::usize a, crd::usize b) { return x[a] < x[b]; });
    crd::usize i = 0;
    while (i < n)
    {
        crd::usize j = i;
        while (j + 1 < n && x[idx[j + 1]] == x[idx[i]])
        {
            ++j;
        }
        const T avg = static_cast<T>(i + j + 2) / static_cast<T>(2); // mean of ranks i+1..j+1
        for (crd::usize k = i; k <= j; ++k)
        {
            out[idx[k]] = avg;
        }
        i = j + 1;
    }
}

// Rank-normalize (z-scale) a flat array in place: z = ndtri((rank - 3/8)/(N - 1/4)) — Blom plotting position.
template <Real T> void z_scale(crd::containers::Span<T> flat, crd::memory::IAllocator* alloc)
{
    const crd::usize n = flat.size();
    crd::containers::Array<T> r(alloc);
    r.resize(n);
    average_ranks(crd::containers::ConstSpan<T>{flat.data(), n}, crd::containers::Span<T>{r.data(), n}, alloc);
    const T nn = static_cast<T>(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        flat[i] = special::ndtri((r[i] - static_cast<T>(0.375)) / (nn + static_cast<T>(0.25)));
    }
}

// Classic Gelman-Rubin R-hat on `m` chains of length `n` (row-major).
template <Real T> [[nodiscard]] T classic_rhat(crd::containers::ConstSpan<T> data, crd::usize m, crd::usize n)
{
    T sum_m = static_cast<T>(0);
    T sum_m2 = static_cast<T>(0);
    T w = static_cast<T>(0);
    for (crd::usize c = 0; c < m; ++c)
    {
        T s = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            s += data[c * n + i];
        }
        const T mc = s / static_cast<T>(n);
        T v = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T d = data[c * n + i] - mc;
            v += d * d;
        }
        v /= static_cast<T>(n - 1);
        sum_m += mc;
        sum_m2 += mc * mc;
        w += v;
    }
    w /= static_cast<T>(m);
    const T grand = sum_m / static_cast<T>(m);
    const T var_means = (sum_m2 - static_cast<T>(m) * grand * grand) / static_cast<T>(m - 1);
    const T var_hat = static_cast<T>(n - 1) / static_cast<T>(n) * w + static_cast<T>(n) * var_means / static_cast<T>(n);
    return crd::math::sqrt(var_hat / w);
}

// Build the split chains (2m chains of length n/2) from `chains` (m x n row-major), RAW (no rank normalization).
template <Real T>
[[nodiscard]] crd::containers::Array<T> build_split(crd::containers::ConstSpan<T> chains, crd::usize m, crd::usize n,
                                                    crd::memory::IAllocator* alloc)
{
    const crd::usize nh = n / 2;
    crd::containers::Array<T> split(alloc);
    split.resize(2 * m * nh);
    for (crd::usize c = 0; c < m; ++c)
    {
        for (crd::usize h = 0; h < 2; ++h)
        {
            for (crd::usize i = 0; i < nh; ++i)
            {
                split[(2 * c + h) * nh + i] = chains[c * n + h * nh + i];
            }
        }
    }
    return split;
}

template <Real T> [[nodiscard]] T median_copy(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> t(alloc);
    t.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        t[i] = x[i];
    }
    crd::containers::sort(t.data(), t.data() + n);
    return (n & 1U) ? t[n / 2] : static_cast<T>(0.5) * (t[n / 2 - 1] + t[n / 2]);
}
} // namespace detail

// Rank-normalized split R-hat (ArviZ default, method='rank') = max(bulk, tail-folded). chains: m x n row-major.
template <Real T>
[[nodiscard]] T rhat(crd::containers::ConstSpan<T> chains, crd::usize m, crd::usize n, crd::memory::IAllocator* alloc)
{
    const crd::usize nh = n / 2;
    const auto raw = detail::build_split(chains, m, n, alloc);
    const crd::usize sz = raw.size();
    crd::containers::Array<T> zb(alloc);
    zb.resize(sz);
    for (crd::usize i = 0; i < sz; ++i)
    {
        zb[i] = raw[i];
    }
    detail::z_scale(crd::containers::Span<T>{zb.data(), sz}, alloc);
    const T r_bulk = detail::classic_rhat(crd::containers::ConstSpan<T>{zb.data(), sz}, 2 * m, nh);
    const T med = detail::median_copy(crd::containers::ConstSpan<T>{raw.data(), sz}, alloc);
    crd::containers::Array<T> zf(alloc);
    zf.resize(sz);
    for (crd::usize i = 0; i < sz; ++i)
    {
        const T d = raw[i] - med;
        zf[i] = d < static_cast<T>(0) ? -d : d;
    }
    detail::z_scale(crd::containers::Span<T>{zf.data(), sz}, alloc);
    const T r_tail = detail::classic_rhat(crd::containers::ConstSpan<T>{zf.data(), sz}, 2 * m, nh);
    return r_bulk > r_tail ? r_bulk : r_tail;
}

// Bulk effective sample size (Stan/ArviZ default). Rank-normalized split chains + Geyer initial-positive + monotone
// autocorrelation truncation.
template <Real T>
[[nodiscard]] T ess_bulk(crd::containers::ConstSpan<T> chains, crd::usize m, crd::usize n,
                         crd::memory::IAllocator* alloc)
{
    const crd::usize nh = n / 2;
    const crd::usize mc = 2 * m;
    auto split = detail::build_split(chains, m, n, alloc);
    detail::z_scale(crd::containers::Span<T>{split.data(), split.size()}, alloc);
    crd::containers::Array<T> cmean(alloc);
    crd::containers::Array<T> acov(alloc);
    cmean.resize(mc);
    acov.resize(mc * nh);
    for (crd::usize c = 0; c < mc; ++c)
    {
        T s = static_cast<T>(0);
        for (crd::usize i = 0; i < nh; ++i)
        {
            s += split[c * nh + i];
        }
        const T m0 = s / static_cast<T>(nh);
        cmean[c] = m0;
        for (crd::usize lag = 0; lag < nh; ++lag)
        {
            T a = static_cast<T>(0);
            for (crd::usize t = 0; t + lag < nh; ++t)
            {
                a += (split[c * nh + t] - m0) * (split[c * nh + t + lag] - m0);
            }
            acov[c * nh + lag] = a / static_cast<T>(nh); // biased autocovariance
        }
    }
    T w = static_cast<T>(0);
    for (crd::usize c = 0; c < mc; ++c)
    {
        w += acov[c * nh + 0] * static_cast<T>(nh) / static_cast<T>(nh - 1); // unbiased chain variances
    }
    w /= static_cast<T>(mc);
    T sm = static_cast<T>(0);
    T sm2 = static_cast<T>(0);
    for (crd::usize c = 0; c < mc; ++c)
    {
        sm += cmean[c];
        sm2 += cmean[c] * cmean[c];
    }
    const T grand = sm / static_cast<T>(mc);
    const T var_means = (sm2 - static_cast<T>(mc) * grand * grand) / static_cast<T>(mc - 1);
    const T var_plus = static_cast<T>(nh - 1) / static_cast<T>(nh) * w + var_means;
    const auto rho = [&](crd::usize t) {
        T a = static_cast<T>(0);
        for (crd::usize c = 0; c < mc; ++c)
        {
            a += acov[c * nh + t];
        }
        a /= static_cast<T>(mc);
        return static_cast<T>(1) - (w - a) / var_plus;
    };
    crd::containers::Array<T> rh(alloc);
    rh.resize(nh);
    rh[0] = static_cast<T>(1);
    T rho_even = static_cast<T>(1);
    T rho_odd = rho(1);
    rh[1] = rho_odd;
    int t = 1;
    while (t < static_cast<int>(nh) - 3 && (rho_even + rho_odd) > static_cast<T>(0))
    {
        rho_even = rho(static_cast<crd::usize>(t + 1));
        rho_odd = rho(static_cast<crd::usize>(t + 2));
        if (rho_even + rho_odd >= static_cast<T>(0))
        {
            rh[t + 1] = rho_even;
            rh[t + 2] = rho_odd;
        }
        t += 2;
    }
    int max_t = t - 2;
    if (rho_even > static_cast<T>(0))
    {
        rh[static_cast<crd::usize>(max_t + 1)] = rho_even;
    }
    // Geyer initial-monotone: paired sums non-increasing
    t = 1;
    while (t <= max_t - 2)
    {
        if (rh[t + 1] + rh[t + 2] > rh[t - 1] + rh[t])
        {
            rh[t + 1] = (rh[t - 1] + rh[t]) / static_cast<T>(2);
            rh[t + 2] = rh[t + 1];
        }
        t += 2;
    }
    const T ess_n = static_cast<T>(mc) * static_cast<T>(nh);
    T tau = -static_cast<T>(1) + rh[static_cast<crd::usize>(max_t + 1)]; // -1 + rho[max_t+1]
    for (int k = 0; k <= max_t; ++k)
    {
        tau += static_cast<T>(2) * rh[static_cast<crd::usize>(k)];
    }
    const T floor_tau = static_cast<T>(1) / (crd::math::log(ess_n) / crd::math::log(static_cast<T>(10)));
    if (tau < floor_tau)
    {
        tau = floor_tau;
    }
    return ess_n / tau;
}

} // namespace crd::hesap::stats
