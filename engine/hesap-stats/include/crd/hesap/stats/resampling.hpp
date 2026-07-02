#pragma once

// v12-o — Resampling inference (crd-hesap-stats). Bootstrap (percentile / basic / BCa / studentized) · block bootstrap
// (time series) · jackknife (+ delete-d) · permutation tests · cross-validation utilities. The RNG-driven resamples
// ride the counter-based Threefry generator, so a fixed seed is bit-reproducible AND thread-partition-invariant (the
// determinism moat scipy/R/MATLAB cannot offer). The deterministic core (jackknife, the CI math, exact permutation)
// gates bit-for-bit vs scipy/statsmodels; the random bootstrap is validated against scipy at large B within
// Monte-Carlo tolerance. Gold: scipy.stats.bootstrap/permutation_test · R boot/bootci · MATLAB bootci/jackknife.

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/special/erf.hpp>       // ndtri (probit), erfc (normal cdf for BCa)
#include <crd/hesap/stats/bitgen.hpp>      // bounded — Lemire unbiased index (one multiply, not a division)
#include <crd/hesap/stats/descriptive.hpp> // Real, mean, quantile_sorted (numpy method 7) — SANITY 8 reuse
#include <crd/hesap/stats/philox.hpp>      // PhiloxRng — counter-based, u32-native (the fast bootstrap index path)
#include <crd/hesap/stats/threefry.hpp>    // ThreefryRng — counter-based (permutation / CV shuffles)
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::stats
{

template <Real T> struct CiResult
{
    T low;
    T high;
};

template <Real T> struct JackknifeResult
{
    T estimate; // jackknife mean of the leave-one-out statistics
    T se;       // jackknife standard error
    T bias;     // jackknife bias estimate (n-1)(theta_jack - theta_hat)
};

template <Real T> struct PermutationResult
{
    T statistic;
    T pvalue;
};

namespace detail
{
// Standard-normal CDF for the BCa adjustment.
template <Real T> [[nodiscard]] inline T resample_norm_cdf(T x)
{
    return static_cast<T>(0.5) * special::erfc(-x / crd::math::sqrt(static_cast<T>(2)));
}

// Lemire's unbiased bounded index using a u32 word (one 64-bit multiply; division only in the ~0-probability reject
// branch). The fast path for bootstrap indices, where the bound (sample size) fits in 32 bits.
template <typename G> [[nodiscard]] inline crd::u32 bounded_u32(G& g, crd::u32 bound) noexcept
{
    crd::u64 m = static_cast<crd::u64>(g.next_u32()) * static_cast<crd::u64>(bound);
    crd::u32 lo = static_cast<crd::u32>(m);
    if (lo < bound)
    {
        const crd::u32 t =
            (crd::u32{0} - bound) % bound; // 2^32 mod bound (0-bound wraps; avoids C4146 unary-minus-on-unsigned)
        while (lo < t)
        {
            m = static_cast<crd::u64>(g.next_u32()) * static_cast<crd::u64>(bound);
            lo = static_cast<crd::u32>(m);
        }
    }
    return static_cast<crd::u32>(m >> 32);
}

// Advance a sorted k-combination idx[0..k) drawn from [0,N) to the next one; false when exhausted.
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

// ───────────────────────────── Jackknife ─────────────────────────────

// Leave-one-out statistics: out[i] = stat(data with element i removed). Stat is a callable ConstSpan<T> -> T.
template <Real T, typename Stat>
[[nodiscard]] crd::containers::Array<T> jackknife_values(crd::containers::ConstSpan<T> data, Stat stat,
                                                         crd::memory::IAllocator* alloc)
{
    const crd::usize n = data.size();
    crd::containers::Array<T> loo(alloc);
    crd::containers::Array<T> out(alloc);
    loo.resize(n - 1);
    out.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        crd::usize k = 0;
        for (crd::usize j = 0; j < n; ++j)
        {
            if (j != i)
            {
                loo[k++] = data[j];
            }
        }
        out[i] = stat(crd::containers::ConstSpan<T>{loo.data(), n - 1});
    }
    return out;
}

// Jackknife estimate, standard error, and bias for a statistic. scipy: each piece matches the leave-one-out formulas.
template <Real T, typename Stat>
[[nodiscard]] JackknifeResult<T> jackknife(crd::containers::ConstSpan<T> data, Stat stat,
                                           crd::memory::IAllocator* alloc)
{
    const crd::usize n = data.size();
    const T theta_hat = stat(data);
    const auto jv = jackknife_values(data, stat, alloc);
    T tj = static_cast<T>(0);
    for (T v : jv)
    {
        tj += v;
    }
    tj /= static_cast<T>(n);
    T s2 = static_cast<T>(0);
    for (T v : jv)
    {
        const T e = v - tj;
        s2 += e * e;
    }
    const T se = crd::math::sqrt(static_cast<T>(n - 1) / static_cast<T>(n) * s2);
    const T bias = static_cast<T>(n - 1) * (tj - theta_hat);
    return {tj, se, bias};
}

// Delete-d jackknife (all C(n,d) subsets): out estimate/se over the C(n,d) leave-d-out statistics. se uses the
// Shao-Wu scaling sqrt((n-d)/(d C(n,d)) sum (theta_S - mean)^2). Feasible only when C(n,d) is small.
template <Real T, typename Stat>
[[nodiscard]] JackknifeResult<T> jackknife_delete_d(crd::containers::ConstSpan<T> data, Stat stat, crd::usize d,
                                                    crd::memory::IAllocator* alloc)
{
    const crd::usize n = data.size();
    const T theta_hat = stat(data);
    const crd::usize keep = n - d; // we KEEP (n-d) elements per subset
    crd::containers::Array<crd::usize> idx(alloc);
    crd::containers::Array<T> sub(alloc);
    idx.resize(keep);
    sub.resize(keep);
    for (crd::usize i = 0; i < keep; ++i)
    {
        idx[i] = i;
    }
    crd::containers::Array<T> stats(alloc);
    do
    {
        for (crd::usize i = 0; i < keep; ++i)
        {
            sub[i] = data[idx[i]];
        }
        stats.push_back(stat(crd::containers::ConstSpan<T>{sub.data(), keep}));
    } while (detail::next_combination(crd::containers::Span<crd::usize>{idx.data(), keep}, n));
    const crd::usize m = stats.size();
    T mean_s = static_cast<T>(0);
    for (T v : stats)
    {
        mean_s += v;
    }
    mean_s /= static_cast<T>(m);
    T s2 = static_cast<T>(0);
    for (T v : stats)
    {
        const T e = v - mean_s;
        s2 += e * e;
    }
    const T se = crd::math::sqrt(static_cast<T>(keep) / (static_cast<T>(d) * static_cast<T>(m)) * s2);
    const T bias = static_cast<T>(keep) * (mean_s - theta_hat) / static_cast<T>(d);
    return {mean_s, se, bias};
}

// ───────────────────────────── Bootstrap CI extraction (deterministic given the distribution) ───────────

// Percentile CI: the alpha/2 and 1-alpha/2 quantiles of the bootstrap distribution (numpy method 7).
template <Real T>
[[nodiscard]] CiResult<T> bootstrap_ci_percentile(crd::containers::ConstSpan<T> boot_dist, T alpha,
                                                  crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> s(alloc);
    s.resize(boot_dist.size());
    for (crd::usize i = 0; i < boot_dist.size(); ++i)
    {
        s[i] = boot_dist[i];
    }
    crd::containers::sort(s.data(), s.data() + s.size());
    const crd::containers::Span<const T> ss{s.data(), s.size()};
    return {quantile_sorted<T>(ss, alpha / static_cast<T>(2), 7),
            quantile_sorted<T>(ss, static_cast<T>(1) - alpha / static_cast<T>(2), 7)};
}

// Basic (reverse-percentile) CI: 2*theta_hat - {upper, lower} percentile.
template <Real T>
[[nodiscard]] CiResult<T> bootstrap_ci_basic(T theta_hat, crd::containers::ConstSpan<T> boot_dist, T alpha,
                                             crd::memory::IAllocator* alloc)
{
    const auto pc = bootstrap_ci_percentile(boot_dist, alpha, alloc);
    return {static_cast<T>(2) * theta_hat - pc.high, static_cast<T>(2) * theta_hat - pc.low};
}

// Bias-corrected and accelerated (BCa) CI. z0 = probit(fraction of boot_dist below theta_hat); acceleration from the
// jackknife skewness; percentiles taken at the BCa-adjusted alphas. scipy.stats.bootstrap(method='BCa').
template <Real T>
[[nodiscard]] CiResult<T> bootstrap_ci_bca(T theta_hat, crd::containers::ConstSpan<T> boot_dist,
                                           crd::containers::ConstSpan<T> jack_vals, T alpha,
                                           crd::memory::IAllocator* alloc)
{
    crd::usize cnt = 0;
    for (T v : boot_dist)
    {
        if (v < theta_hat)
        {
            ++cnt;
        }
    }
    const T z0 = special::ndtri(static_cast<T>(cnt) / static_cast<T>(boot_dist.size()));
    T jm = static_cast<T>(0);
    for (T v : jack_vals)
    {
        jm += v;
    }
    jm /= static_cast<T>(jack_vals.size());
    T num = static_cast<T>(0);
    T den = static_cast<T>(0);
    for (T v : jack_vals)
    {
        const T dd = jm - v;
        num += dd * dd * dd;
        den += dd * dd;
    }
    const T acc = num / (static_cast<T>(6) * crd::math::pow(den, static_cast<T>(1.5)));
    const auto adj = [&](T al)
    {
        const T za = special::ndtri(al);
        return detail::resample_norm_cdf(z0 + (z0 + za) / (static_cast<T>(1) - acc * (z0 + za)));
    };
    crd::containers::Array<T> s(alloc);
    s.resize(boot_dist.size());
    for (crd::usize i = 0; i < boot_dist.size(); ++i)
    {
        s[i] = boot_dist[i];
    }
    crd::containers::sort(s.data(), s.data() + s.size());
    const crd::containers::Span<const T> ss{s.data(), s.size()};
    return {quantile_sorted<T>(ss, adj(alpha / static_cast<T>(2)), 7),
            quantile_sorted<T>(ss, adj(static_cast<T>(1) - alpha / static_cast<T>(2)), 7)};
}

// Studentized (bootstrap-t) CI: pivot t* = (boot_stat - theta_hat)/boot_se; CI = theta_hat - se_hat * {q_{1-a/2},
// q_{a/2}} of t*. Needs a per-resample standard error (boot_se), typically from a nested bootstrap.
template <Real T>
[[nodiscard]] CiResult<T> bootstrap_ci_studentized(T theta_hat, T se_hat, crd::containers::ConstSpan<T> boot_stat,
                                                   crd::containers::ConstSpan<T> boot_se, T alpha,
                                                   crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> tstar(alloc);
    tstar.resize(boot_stat.size());
    for (crd::usize i = 0; i < boot_stat.size(); ++i)
    {
        tstar[i] = (boot_stat[i] - theta_hat) / boot_se[i];
    }
    crd::containers::sort(tstar.data(), tstar.data() + tstar.size());
    const crd::containers::Span<const T> ts{tstar.data(), tstar.size()};
    const T qlo = quantile_sorted<T>(ts, alpha / static_cast<T>(2), 7);
    const T qhi = quantile_sorted<T>(ts, static_cast<T>(1) - alpha / static_cast<T>(2), 7);
    return {theta_hat - se_hat * qhi, theta_hat - se_hat * qlo};
}

// ───────────────────────────── Permutation test (two independent samples) ─────────────────────────────

// Two-sided permutation test for two independent samples. statistic is a callable (ConstSpan<T> a, ConstSpan<T> b)->T.
// Exact (all C(N,na) label assignments) when that count <= max_exact, else Monte-Carlo with n_mc Threefry shuffles.
// scipy.stats.permutation_test(permutation_type='independent', alternative='two-sided').
template <Real T, typename Stat>
[[nodiscard]] PermutationResult<T> permutation_test_ind(crd::containers::ConstSpan<T> a,
                                                        crd::containers::ConstSpan<T> b, Stat stat,
                                                        crd::memory::IAllocator* alloc, crd::usize max_exact = 200000,
                                                        crd::usize n_mc = 100000, crd::u64 seed = 0)
{
    const crd::usize na = a.size();
    const crd::usize nb = b.size();
    const crd::usize n = na + nb;
    crd::containers::Array<T> pool(alloc);
    pool.reserve(n);
    for (T v : a)
    {
        pool.push_back(v);
    }
    for (T v : b)
    {
        pool.push_back(v);
    }
    const T obs = stat(a, b);
    const T aobs = (obs < static_cast<T>(0)) ? -obs : obs;
    const T eps = aobs * static_cast<T>(1e-9) + static_cast<T>(1e-300);
    crd::containers::Array<T> ga(alloc);
    crd::containers::Array<T> gb(alloc);
    ga.resize(na);
    gb.resize(nb);
    // exact feasibility: C(n, na) without overflow
    double total_d = 1.0;
    for (crd::usize i = 0; i < na; ++i)
    {
        total_d *= static_cast<double>(n - i) / static_cast<double>(i + 1);
    }
    const auto eval_split = [&](const crd::usize* idx)
    {
        // idx[0..na) selects group a from the pool; the complement is group b
        crd::usize ia = 0;
        crd::usize ib = 0;
        crd::usize p = 0;
        for (crd::usize sel = 0; sel < na; ++sel)
        {
            while (p < idx[sel])
            {
                gb[ib++] = pool[p++];
            }
            ga[ia++] = pool[p++];
        }
        while (p < n)
        {
            gb[ib++] = pool[p++];
        }
        const T st = stat(crd::containers::ConstSpan<T>{ga.data(), na}, crd::containers::ConstSpan<T>{gb.data(), nb});
        return (st < static_cast<T>(0) ? -st : st) >= aobs - eps;
    };
    if (total_d <= static_cast<double>(max_exact))
    {
        crd::containers::Array<crd::usize> idx(alloc);
        idx.resize(na);
        for (crd::usize i = 0; i < na; ++i)
        {
            idx[i] = i;
        }
        crd::usize total = 0;
        crd::usize count = 0;
        do
        {
            ++total;
            if (eval_split(idx.data()))
            {
                ++count;
            }
        } while (detail::next_combination(crd::containers::Span<crd::usize>{idx.data(), na}, n));
        return {obs, static_cast<T>(count) / static_cast<T>(total)};
    }
    // Monte-Carlo: random label shuffles via Threefry (partition-invariant per shuffle index)
    crd::containers::Array<crd::usize> perm(alloc);
    perm.resize(n);
    crd::usize count = 1; // include the observed (scipy's (#ge + 1)/(n_mc + 1))
    for (crd::usize r = 0; r < n_mc; ++r)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            perm[i] = i;
        }
        ThreefryRng rng(seed, r);
        for (crd::usize i = n; i > 1; --i) // Fisher-Yates with a counter-RNG
        {
            const crd::usize j = static_cast<crd::usize>(bounded(rng, static_cast<crd::u64>(i)));
            const crd::usize t = perm[i - 1];
            perm[i - 1] = perm[j];
            perm[j] = t;
        }
        crd::usize ia = 0;
        crd::usize ib = 0;
        for (crd::usize i = 0; i < n; ++i)
        {
            if (i < na)
            {
                ga[ia++] = pool[perm[i]];
            }
            else
            {
                gb[ib++] = pool[perm[i]];
            }
        }
        const T st = stat(crd::containers::ConstSpan<T>{ga.data(), na}, crd::containers::ConstSpan<T>{gb.data(), nb});
        if ((st < static_cast<T>(0) ? -st : st) >= aobs - eps)
        {
            ++count;
        }
    }
    return {obs, static_cast<T>(count) / static_cast<T>(n_mc + 1)};
}

// ───────────────────────────── RNG bootstrap (the resampling) ─────────────────────────────

enum class BootMethod
{
    Percentile,
    Basic,
    Bca
};

// Bootstrap distribution: B resampled statistics. Resample r draws n indices with replacement using Threefry stream r
// — so it depends only on (seed, r), never on thread assignment: a fixed seed is bit-reproducible AND
// thread-partition-invariant (the determinism moat). Stat is a callable ConstSpan<T> -> T.
template <Real T, typename Stat>
[[nodiscard]] crd::containers::Array<T> bootstrap_distribution(crd::containers::ConstSpan<T> data, Stat stat,
                                                               crd::usize n_resamples, crd::u64 seed,
                                                               crd::memory::IAllocator* alloc)
{
    const crd::usize n = data.size();
    crd::containers::Array<T> out(alloc);
    crd::containers::Array<T> resample(alloc);
    out.resize(n_resamples);
    resample.resize(n);
    for (crd::usize r = 0; r < n_resamples; ++r)
    {
        PhiloxRng rng(seed, static_cast<crd::u64>(r));
        for (crd::usize i = 0; i < n; ++i)
        {
            resample[i] = data[detail::bounded_u32(rng, static_cast<crd::u32>(n))];
        }
        out[r] = stat(crd::containers::ConstSpan<T>{resample.data(), n});
    }
    return out;
}

// Full bootstrap confidence interval (percentile / basic / BCa) at confidence level 1-alpha.
template <Real T, typename Stat>
[[nodiscard]] CiResult<T> bootstrap_ci(crd::containers::ConstSpan<T> data, Stat stat, crd::usize n_resamples,
                                       BootMethod method, T alpha, crd::u64 seed, crd::memory::IAllocator* alloc)
{
    const T theta_hat = stat(data);
    const auto dist = bootstrap_distribution(data, stat, n_resamples, seed, alloc);
    const auto ds = crd::containers::ConstSpan<T>{dist.data(), dist.size()};
    if (method == BootMethod::Percentile)
    {
        return bootstrap_ci_percentile(ds, alpha, alloc);
    }
    if (method == BootMethod::Basic)
    {
        return bootstrap_ci_basic(theta_hat, ds, alpha, alloc);
    }
    const auto jv = jackknife_values(data, stat, alloc);
    return bootstrap_ci_bca(theta_hat, ds, crd::containers::ConstSpan<T>{jv.data(), jv.size()}, alpha, alloc);
}

// Moving-block bootstrap for stationary time series: each resample concatenates ceil(n/block_len) blocks of
// consecutive observations (block starts drawn uniformly), truncated to n. R `tsboot`/`boot::tsboot`.
template <Real T, typename Stat>
[[nodiscard]] crd::containers::Array<T> block_bootstrap_distribution(crd::containers::ConstSpan<T> data, Stat stat,
                                                                     crd::usize n_resamples, crd::usize block_len,
                                                                     crd::u64 seed, crd::memory::IAllocator* alloc)
{
    const crd::usize n = data.size();
    const crd::usize n_blocks = (n + block_len - 1) / block_len;
    const crd::u64 n_starts = static_cast<crd::u64>(n - block_len + 1);
    crd::containers::Array<T> out(alloc);
    crd::containers::Array<T> resample(alloc);
    out.resize(n_resamples);
    resample.resize(n);
    for (crd::usize r = 0; r < n_resamples; ++r)
    {
        PhiloxRng rng(seed, static_cast<crd::u64>(r));
        crd::usize pos = 0;
        for (crd::usize blk = 0; blk < n_blocks && pos < n; ++blk)
        {
            const crd::usize start = static_cast<crd::usize>(detail::bounded_u32(rng, static_cast<crd::u32>(n_starts)));
            for (crd::usize k = 0; k < block_len && pos < n; ++k)
            {
                resample[pos++] = data[start + k];
            }
        }
        out[r] = stat(crd::containers::ConstSpan<T>{resample.data(), n});
    }
    return out;
}

// ───────────────────────────── Cross-validation utilities ─────────────────────────────

// K-fold assignment: fold[i] in [0,k) for each of n samples, a balanced shuffled partition (Threefry(seed) shuffle,
// round-robin to folds). k == n gives leave-one-out. Every index appears in exactly one fold; reproducible per seed.
[[nodiscard]] inline crd::containers::Array<crd::usize> kfold_indices(crd::usize n, crd::usize k, crd::u64 seed,
                                                                      crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::usize> perm(alloc);
    perm.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        perm[i] = i;
    }
    ThreefryRng rng(seed, 0);
    for (crd::usize i = n; i > 1; --i)
    {
        const crd::usize j = static_cast<crd::usize>(bounded(rng, static_cast<crd::u64>(i)));
        const crd::usize t = perm[i - 1];
        perm[i - 1] = perm[j];
        perm[j] = t;
    }
    crd::containers::Array<crd::usize> fold(alloc);
    fold.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        fold[perm[i]] = i % k;
    }
    return fold;
}

} // namespace crd::hesap::stats
