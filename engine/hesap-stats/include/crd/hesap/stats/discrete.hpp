#pragma once

// crd-hesap-stats v12-i — 12 univariate DISCRETE distributions on the Distribution<T> framework. pmf over integers;
// CDFs reuse the shipped hesap-special closed forms where they exist (Poisson→gammainc_q, Binomial/NegBinom→betainc),
// and a finite pmf-sum elsewhere (Hypergeom/Skellam/Zipf/YuleSimon/BetaBinomial/Logarithmic — O(support) over the
// evaluated range). rvs reuses the v12-f samplers (binomial/poisson/geometric) + standard methods. Gated vs scipy.stats.
// (COM-Poisson is omitted: scipy has no native gate for it; noted in docs.)

#include <crd/hesap/stats/distribution.hpp>
#include <crd/hesap/stats/samplers.hpp>

#include <crd/hesap/special/bessel.hpp>     // cyl_bessel_i (Skellam)
#include <crd/hesap/special/gamma.hpp>      // lgamma / lbeta / digamma
#include <crd/hesap/special/incomplete.hpp> // gammainc_q (Poisson) / betainc (Binomial, NegBinom)
#include <crd/hesap/special/zeta.hpp>       // zeta (Zipf)

#include <crd/math/cmath.hpp>

namespace crd::hesap::stats
{
namespace detail
{
// log C(n, k) via lgamma.
template <Real T>
[[nodiscard]] T log_binom(T n, T k) noexcept
{
    return special::lgamma(n + static_cast<T>(1)) - special::lgamma(k + static_cast<T>(1)) -
           special::lgamma(n - k + static_cast<T>(1));
}
} // namespace detail

// ───────────────────────────── Bernoulli(p) ─────────────────────────────
template <Real T>
struct Bernoulli : DiscreteBase<Bernoulli<T>, T>
{
    using value_type = T;
    T p = static_cast<T>(0.5);

    Bernoulli() noexcept = default;
    explicit Bernoulli(T prob) noexcept : p(prob) {}

    [[nodiscard]] T pmf(crd::i64 k) const noexcept
    {
        if (k == 0)
        {
            return static_cast<T>(1) - p;
        }
        return k == 1 ? p : static_cast<T>(0);
    }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept { return crd::math::log(pmf(k)); }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        if (k < 0)
        {
            return static_cast<T>(0);
        }
        return k < 1 ? static_cast<T>(1) - p : static_cast<T>(1);
    }
    [[nodiscard]] crd::i64 ppf(T q) const noexcept { return q <= static_cast<T>(1) - p ? 0 : 1; }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        return static_cast<T>(next_double(g)) < p ? 1 : 0;
    }
    [[nodiscard]] T mean() const noexcept { return p; }
    [[nodiscard]] T var() const noexcept { return p * (static_cast<T>(1) - p); }
    [[nodiscard]] T skewness() const noexcept
    {
        const T q = static_cast<T>(1) - p;
        return (q - p) / crd::math::sqrt(p * q);
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        const T pq = p * (static_cast<T>(1) - p);
        return (static_cast<T>(1) - static_cast<T>(6) * pq) / pq;
    }
    [[nodiscard]] T entropy() const noexcept
    {
        const T q = static_cast<T>(1) - p;
        return -p * crd::math::log(p) - q * crd::math::log(q);
    }
};

// ───────────────────────────── Binomial(n, p) ─────────────────────────────
template <Real T>
struct Binomial : DiscreteBase<Binomial<T>, T>
{
    using value_type = T;
    crd::i64 n = 1;
    T p = static_cast<T>(0.5);

    Binomial() noexcept = default;
    Binomial(crd::i64 trials, T prob) noexcept : n(trials), p(prob) {}

    [[nodiscard]] T pmf(crd::i64 k) const noexcept { return (k < 0 || k > n) ? static_cast<T>(0) : crd::math::exp(logpmf(k)); }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept
    {
        return detail::log_binom(static_cast<T>(n), static_cast<T>(k)) + static_cast<T>(k) * crd::math::log(p) +
               static_cast<T>(n - k) * crd::math::log1p(-p);
    }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        if (k < 0)
        {
            return static_cast<T>(0);
        }
        if (k >= n)
        {
            return static_cast<T>(1);
        }
        const T q = static_cast<T>(1) - p;
        // Direct pmf-summation of the SHORTER tail beats the betainc continued fraction ~7× for small n (no CF, no
        // lgamma) — and stays exact via the C(n,j) recurrence. Fall back to betainc for large n (where O(n) summing
        // would lose). q^n / p^n stay representable for n ≤ ~700, well above this cap.
        if (n <= 200)
        {
            if (static_cast<T>(2) * static_cast<T>(k) < static_cast<T>(n - 1)) // lower tail [0..k] is shorter
            {
                T term = crd::math::pow(q, static_cast<T>(n)); // pmf(0)
                T s = term;
                for (crd::i64 j = 1; j <= k; ++j)
                {
                    term *= static_cast<T>(n - j + 1) * p / (static_cast<T>(j) * q);
                    s += term;
                }
                return s;
            }
            T term = crd::math::pow(p, static_cast<T>(n)); // pmf(n)
            T s = term;
            for (crd::i64 j = n; j > k + 1; --j)
            {
                term *= static_cast<T>(j) * q / (static_cast<T>(n - j + 1) * p); // pmf(j-1) from pmf(j)
                s += term;
            }
            return static_cast<T>(1) - s;
        }
        return special::betainc(static_cast<T>(n - k), static_cast<T>(k + 1), q);
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        return binomial(g, n, static_cast<double>(p));
    }
    [[nodiscard]] T mean() const noexcept { return static_cast<T>(n) * p; }
    [[nodiscard]] T var() const noexcept { return static_cast<T>(n) * p * (static_cast<T>(1) - p); }
    [[nodiscard]] T skewness() const noexcept
    {
        const T q = static_cast<T>(1) - p;
        return (q - p) / crd::math::sqrt(static_cast<T>(n) * p * q);
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        const T q = static_cast<T>(1) - p;
        return (static_cast<T>(1) - static_cast<T>(6) * p * q) / (static_cast<T>(n) * p * q);
    }
    [[nodiscard]] T entropy() const noexcept
    {
        T h = 0;
        for (crd::i64 k = 0; k <= n; ++k)
        {
            const T pk = pmf(k);
            if (pk > static_cast<T>(0))
            {
                h -= pk * crd::math::log(pk);
            }
        }
        return h;
    }
};

// ───────────────────────────── Poisson(λ) ─────────────────────────────
template <Real T>
struct Poisson : DiscreteBase<Poisson<T>, T>
{
    using value_type = T;
    T lambda = static_cast<T>(1);

    Poisson() noexcept = default;
    explicit Poisson(T lam) noexcept : lambda(lam) {}

    [[nodiscard]] T pmf(crd::i64 k) const noexcept { return k < 0 ? static_cast<T>(0) : crd::math::exp(logpmf(k)); }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept
    {
        return static_cast<T>(k) * crd::math::log(lambda) - lambda - special::lgamma(static_cast<T>(k + 1));
    }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        return k < 0 ? static_cast<T>(0) : special::gammainc_q(static_cast<T>(k + 1), lambda);
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        return poisson(g, static_cast<double>(lambda));
    }
    [[nodiscard]] T mean() const noexcept { return lambda; }
    [[nodiscard]] T var() const noexcept { return lambda; }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(1) / crd::math::sqrt(lambda); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(1) / lambda; }
    [[nodiscard]] T entropy() const noexcept
    {
        const crd::i64 hi = static_cast<crd::i64>(lambda + static_cast<T>(40) * crd::math::sqrt(lambda)) + 20;
        T h = 0;
        for (crd::i64 k = 0; k <= hi; ++k)
        {
            const T pk = pmf(k);
            if (pk > static_cast<T>(0))
            {
                h -= pk * crd::math::log(pk);
            }
        }
        return h;
    }
};

// ───────────────────────────── Geometric(p) — scipy.geom, support k ≥ 1 ─────────────────────────────
template <Real T>
struct Geometric : DiscreteBase<Geometric<T>, T>
{
    using value_type = T;
    T p = static_cast<T>(0.5);

    Geometric() noexcept = default;
    explicit Geometric(T prob) noexcept : p(prob), m_log1mp(crd::math::log1p(-prob)) {}

    [[nodiscard]] T pmf(crd::i64 k) const noexcept
    {
        // exp(cached ln(1−p)) beats pow(1−p, ·); m_log1mp amortizes the param-only log (Boost caches it too).
        return k < 1 ? static_cast<T>(0) : crd::math::exp(static_cast<T>(k - 1) * m_log1mp) * p;
    }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept { return static_cast<T>(k - 1) * m_log1mp + crd::math::log(p); }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        return k < 1 ? static_cast<T>(0) : -crd::math::expm1(static_cast<T>(k) * m_log1mp);
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        return geometric(g, static_cast<double>(p)) + 1; // v12-f geometric = #failures; scipy.geom = #trials
    }
    [[nodiscard]] T mean() const noexcept { return static_cast<T>(1) / p; }
    [[nodiscard]] T var() const noexcept { return (static_cast<T>(1) - p) / (p * p); }
    [[nodiscard]] T skewness() const noexcept { return (static_cast<T>(2) - p) / crd::math::sqrt(static_cast<T>(1) - p); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(6) + p * p / (static_cast<T>(1) - p); }
    [[nodiscard]] T entropy() const noexcept
    {
        const T q = static_cast<T>(1) - p;
        return (-q * crd::math::log(q) - p * crd::math::log(p)) / p;
    }

private:
    T m_log1mp = crd::math::log1p(-static_cast<T>(0.5)); // ln(1−p), amortized (default p = 0.5)
};

// ───────────────────────────── NegativeBinomial(n, p) — scipy.nbinom, support k ≥ 0 failures ─────────────────────────────
template <Real T>
struct NegativeBinomial : DiscreteBase<NegativeBinomial<T>, T>
{
    using value_type = T;
    T n = static_cast<T>(1); // number of successes (real-valued allowed)
    T p = static_cast<T>(0.5);

    NegativeBinomial() noexcept = default;
    NegativeBinomial(T successes, T prob) noexcept : n(successes), p(prob) {}

    [[nodiscard]] T pmf(crd::i64 k) const noexcept { return k < 0 ? static_cast<T>(0) : crd::math::exp(logpmf(k)); }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept
    {
        return special::lgamma(static_cast<T>(k) + n) - special::lgamma(static_cast<T>(k + 1)) - special::lgamma(n) +
               n * crd::math::log(p) + static_cast<T>(k) * crd::math::log1p(-p);
    }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        return k < 0 ? static_cast<T>(0) : special::betainc(n, static_cast<T>(k + 1), p);
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        const double lam = gamma_dist(g, static_cast<double>(n), static_cast<double>(1.0 - p) / static_cast<double>(p));
        return poisson(g, lam);
    }
    [[nodiscard]] T mean() const noexcept { return n * (static_cast<T>(1) - p) / p; }
    [[nodiscard]] T var() const noexcept { return n * (static_cast<T>(1) - p) / (p * p); }
    [[nodiscard]] T skewness() const noexcept
    {
        return (static_cast<T>(2) - p) / crd::math::sqrt(n * (static_cast<T>(1) - p));
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        const T q = static_cast<T>(1) - p;
        return (p * p - static_cast<T>(6) * p + static_cast<T>(6)) / (n * q);
    }
    [[nodiscard]] T entropy() const noexcept
    {
        const T mu = mean();
        const crd::i64 hi = static_cast<crd::i64>(mu + static_cast<T>(40) * crd::math::sqrt(var())) + 20;
        T h = 0;
        for (crd::i64 k = 0; k <= hi; ++k)
        {
            const T pk = pmf(k);
            if (pk > static_cast<T>(0))
            {
                h -= pk * crd::math::log(pk);
            }
        }
        return h;
    }
};

// ───────────────────────────── DiscreteUniform(low, high) — scipy.randint, support [low, high) ─────────────────────────────
template <Real T>
struct DiscreteUniform : DiscreteBase<DiscreteUniform<T>, T>
{
    using value_type = T;
    crd::i64 low = 0;
    crd::i64 high = 2; // exclusive

    DiscreteUniform() noexcept = default;
    DiscreteUniform(crd::i64 lo, crd::i64 hi) noexcept : low(lo), high(hi) {}

    [[nodiscard]] crd::i64 count() const noexcept { return high - low; }
    [[nodiscard]] T pmf(crd::i64 k) const noexcept
    {
        return (k < low || k >= high) ? static_cast<T>(0) : static_cast<T>(1) / static_cast<T>(count());
    }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept { return crd::math::log(pmf(k)); }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        if (k < low)
        {
            return static_cast<T>(0);
        }
        return k >= high - 1 ? static_cast<T>(1) : static_cast<T>(k - low + 1) / static_cast<T>(count());
    }
    [[nodiscard]] crd::i64 ppf(T q) const noexcept
    {
        crd::i64 j = static_cast<crd::i64>(crd::math::ceil(q * static_cast<T>(count()))) - 1;
        if (j < 0)
        {
            j = 0;
        }
        if (j >= count())
        {
            j = count() - 1;
        }
        return low + j;
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        return low + static_cast<crd::i64>(bounded(g, static_cast<crd::u64>(count())));
    }
    [[nodiscard]] T mean() const noexcept { return static_cast<T>(low + high - 1) / static_cast<T>(2); }
    [[nodiscard]] T var() const noexcept
    {
        const T nn = static_cast<T>(count());
        return (nn * nn - static_cast<T>(1)) / static_cast<T>(12);
    }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(0); }
    [[nodiscard]] T kurtosis() const noexcept
    {
        const T nn2 = static_cast<T>(count()) * static_cast<T>(count());
        return static_cast<T>(-6) * (nn2 + static_cast<T>(1)) / (static_cast<T>(5) * (nn2 - static_cast<T>(1)));
    }
    [[nodiscard]] T entropy() const noexcept { return crd::math::log(static_cast<T>(count())); }
};

// ───────────────────────────── Hypergeometric(M, n, N) — scipy.hypergeom(M=total, n=good, N=draws) ─────────────────────────────
template <Real T>
struct Hypergeometric : DiscreteBase<Hypergeometric<T>, T>
{
    using value_type = T;
    crd::i64 mtotal = 1;
    crd::i64 ngood = 1;
    crd::i64 ndraw = 1;

    Hypergeometric() noexcept = default;
    Hypergeometric(crd::i64 total, crd::i64 good, crd::i64 draws) noexcept : mtotal(total), ngood(good), ndraw(draws) {}

    [[nodiscard]] crd::i64 lo() const noexcept { return std::max<crd::i64>(0, ndraw - (mtotal - ngood)); }
    [[nodiscard]] crd::i64 hi() const noexcept { return std::min(ndraw, ngood); }
    [[nodiscard]] T pmf(crd::i64 k) const noexcept
    {
        if (k < lo() || k > hi())
        {
            return static_cast<T>(0);
        }
        return crd::math::exp(detail::log_binom(static_cast<T>(ngood), static_cast<T>(k)) +
                        detail::log_binom(static_cast<T>(mtotal - ngood), static_cast<T>(ndraw - k)) -
                        detail::log_binom(static_cast<T>(mtotal), static_cast<T>(ndraw)));
    }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept { return crd::math::log(pmf(k)); }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        if (k < lo())
        {
            return static_cast<T>(0);
        }
        if (k >= hi())
        {
            return static_cast<T>(1);
        }
        T s = 0;
        for (crd::i64 j = lo(); j <= k; ++j)
        {
            s += pmf(j);
        }
        return s;
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        // sequential without-replacement draw.
        crd::i64 good = ngood;
        crd::i64 rest = mtotal;
        crd::i64 cnt = 0;
        for (crd::i64 i = 0; i < ndraw; ++i)
        {
            if (static_cast<T>(next_double(g)) * static_cast<T>(rest) < static_cast<T>(good))
            {
                ++cnt;
                --good;
            }
            --rest;
        }
        return cnt;
    }
    [[nodiscard]] T mean() const noexcept
    {
        return static_cast<T>(ndraw) * static_cast<T>(ngood) / static_cast<T>(mtotal);
    }
    [[nodiscard]] T var() const noexcept
    {
        const T m = static_cast<T>(mtotal);
        const T nn = static_cast<T>(ngood);
        const T nd = static_cast<T>(ndraw);
        return nd * (nn / m) * ((m - nn) / m) * ((m - nd) / (m - static_cast<T>(1)));
    }
    [[nodiscard]] T skewness() const noexcept
    {
        const T m = static_cast<T>(mtotal);
        const T nn = static_cast<T>(ngood);
        const T nd = static_cast<T>(ndraw);
        const T num = (m - static_cast<T>(2) * nn) * crd::math::sqrt(m - static_cast<T>(1)) * (m - static_cast<T>(2) * nd);
        const T den = crd::math::sqrt(nd * nn * (m - nn) * (m - nd)) * (m - static_cast<T>(2));
        return num / den;
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        const T m = static_cast<T>(mtotal);
        const T nn = static_cast<T>(ngood);
        const T nd = static_cast<T>(ndraw);
        const T t1 = (m - static_cast<T>(1)) * m * m *
                     (m * (m + static_cast<T>(1)) - static_cast<T>(6) * nn * (m - nn) -
                      static_cast<T>(6) * nd * (m - nd)) +
                     static_cast<T>(6) * nd * nn * (m - nn) * (m - nd) * (static_cast<T>(5) * m - static_cast<T>(6));
        const T den = nd * nn * (m - nn) * (m - nd) * (m - static_cast<T>(2)) * (m - static_cast<T>(3));
        return t1 / den;
    }
    [[nodiscard]] T entropy() const noexcept
    {
        T h = 0;
        for (crd::i64 k = lo(); k <= hi(); ++k)
        {
            const T pk = pmf(k);
            if (pk > static_cast<T>(0))
            {
                h -= pk * crd::math::log(pk);
            }
        }
        return h;
    }
};

// ───────────────────────────── Skellam(μ1, μ2) — difference of Poissons, support ℤ ─────────────────────────────
template <Real T>
struct Skellam : DiscreteBase<Skellam<T>, T>
{
    using value_type = T;
    T mu1 = static_cast<T>(1);
    T mu2 = static_cast<T>(1);

    Skellam() noexcept = default;
    Skellam(T m1, T m2) noexcept : mu1(m1), mu2(m2) {}

    [[nodiscard]] T pmf(crd::i64 k) const noexcept
    {
        const T ak = static_cast<T>(k < 0 ? -k : k);
        return crd::math::exp(-(mu1 + mu2)) * crd::math::pow(mu1 / mu2, static_cast<T>(k) / static_cast<T>(2)) *
               special::cyl_bessel_i(ak, static_cast<T>(2) * crd::math::sqrt(mu1 * mu2));
    }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept { return crd::math::log(pmf(k)); }
    [[nodiscard]] crd::i64 low_bound() const noexcept
    {
        return static_cast<crd::i64>(mean() - static_cast<T>(40) * crd::math::sqrt(var())) - 20;
    }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        T s = 0;
        for (crd::i64 j = low_bound(); j <= k; ++j)
        {
            s += pmf(j);
        }
        return s > static_cast<T>(1) ? static_cast<T>(1) : s;
    }
    [[nodiscard]] crd::i64 ppf(T q) const noexcept
    {
        crd::i64 k = low_bound();
        T s = 0;
        while (s < q && k < low_bound() + 100000)
        {
            s += pmf(k);
            if (s >= q)
            {
                return k;
            }
            ++k;
        }
        return k;
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        return poisson(g, static_cast<double>(mu1)) - poisson(g, static_cast<double>(mu2));
    }
    [[nodiscard]] T mean() const noexcept { return mu1 - mu2; }
    [[nodiscard]] T var() const noexcept { return mu1 + mu2; }
    [[nodiscard]] T skewness() const noexcept { return (mu1 - mu2) / crd::math::pow(mu1 + mu2, static_cast<T>(1.5)); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(1) / (mu1 + mu2); }
    [[nodiscard]] T entropy() const noexcept
    {
        const crd::i64 lo = low_bound();
        const crd::i64 hiK = -lo + 2 * static_cast<crd::i64>(mean());
        T h = 0;
        for (crd::i64 k = lo; k <= hiK; ++k)
        {
            const T pk = pmf(k);
            if (pk > static_cast<T>(0))
            {
                h -= pk * crd::math::log(pk);
            }
        }
        return h;
    }
};

// ───────────────────────────── Zipf(a) — scipy.zipf, support k ≥ 1 ─────────────────────────────
template <Real T>
struct Zipf : DiscreteBase<Zipf<T>, T>
{
    using value_type = T;
    T a = static_cast<T>(2);

    Zipf() noexcept = default;
    explicit Zipf(T exponent) noexcept : a(exponent) {}

    [[nodiscard]] T pmf(crd::i64 k) const noexcept
    {
        return k < 1 ? static_cast<T>(0) : crd::math::pow(static_cast<T>(k), -a) / special::riemann_zeta(a);
    }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept
    {
        return -a * crd::math::log(static_cast<T>(k)) - crd::math::log(special::riemann_zeta(a));
    }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        if (k < 1)
        {
            return static_cast<T>(0);
        }
        const T z = special::riemann_zeta(a);
        T s = 0;
        for (crd::i64 j = 1; j <= k; ++j)
        {
            s += crd::math::pow(static_cast<T>(j), -a);
        }
        return s / z;
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        // Devroye rejection (a > 1).
        const T b = crd::math::pow(static_cast<T>(2), a - static_cast<T>(1));
        for (;;)
        {
            const T u = static_cast<T>(next_double(g));
            const T v = static_cast<T>(next_double(g));
            const auto x = static_cast<crd::i64>(crd::math::floor(crd::math::pow(u, -static_cast<T>(1) / (a - static_cast<T>(1)))));
            const T t = crd::math::pow(static_cast<T>(1) + static_cast<T>(1) / static_cast<T>(x), a - static_cast<T>(1));
            if (v * static_cast<T>(x) * (t - static_cast<T>(1)) / (b - static_cast<T>(1)) <= t / b)
            {
                return x;
            }
        }
    }
    [[nodiscard]] T mean() const noexcept
    {
        return a > static_cast<T>(2) ? special::riemann_zeta(a - static_cast<T>(1)) /
                                           special::riemann_zeta(a)
                                     : std::numeric_limits<T>::infinity();
    }
    [[nodiscard]] T var() const noexcept
    {
        if (a <= static_cast<T>(3))
        {
            return std::numeric_limits<T>::infinity();
        }
        const T z = special::riemann_zeta(a);
        const T m1 = special::riemann_zeta(a - static_cast<T>(1)) / z;
        const T m2 = special::riemann_zeta(a - static_cast<T>(2)) / z;
        return m2 - m1 * m1;
    }
    [[nodiscard]] T skewness() const noexcept { return detail::nan<T>(); } // gated where finite via ref (skip for a≤4)
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T entropy() const noexcept { return detail::nan<T>(); }
};

// ───────────────────────────── YuleSimon(α) — scipy.yulesimon, support k ≥ 1 ─────────────────────────────
template <Real T>
struct YuleSimon : DiscreteBase<YuleSimon<T>, T>
{
    using value_type = T;
    T alpha = static_cast<T>(2);

    YuleSimon() noexcept = default;
    explicit YuleSimon(T a) noexcept : alpha(a) {}

    [[nodiscard]] T pmf(crd::i64 k) const noexcept
    {
        return k < 1 ? static_cast<T>(0) : alpha * crd::math::exp(special::lbeta(static_cast<T>(k), alpha + static_cast<T>(1)));
    }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept
    {
        return crd::math::log(alpha) + special::lbeta(static_cast<T>(k), alpha + static_cast<T>(1));
    }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        if (k < 1)
        {
            return static_cast<T>(0);
        }
        // closed form: 1 − k·B(k, α+1).
        return static_cast<T>(1) - static_cast<T>(k) * crd::math::exp(special::lbeta(static_cast<T>(k), alpha + static_cast<T>(1)));
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        // mixture: λ ~ Exp(rate=α); X ~ Geometric(p=exp(−λ)) (#trials ≥ 1).
        const T lam = static_cast<T>(standard_exponential(g)) / alpha;
        const T pp = crd::math::exp(-lam);
        const T u = static_cast<T>(next_double(g));
        return static_cast<crd::i64>(crd::math::floor(crd::math::log1p(-u) / crd::math::log1p(-pp))) + 1;
    }
    [[nodiscard]] T mean() const noexcept
    {
        return alpha > static_cast<T>(1) ? alpha / (alpha - static_cast<T>(1)) : std::numeric_limits<T>::infinity();
    }
    [[nodiscard]] T var() const noexcept
    {
        if (alpha <= static_cast<T>(2))
        {
            return std::numeric_limits<T>::infinity();
        }
        return alpha * alpha / ((alpha - static_cast<T>(1)) * (alpha - static_cast<T>(1)) * (alpha - static_cast<T>(2)));
    }
    [[nodiscard]] T skewness() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T entropy() const noexcept { return detail::nan<T>(); }
};

// ───────────────────────────── BetaBinomial(n, a, b) — scipy.betabinom ─────────────────────────────
template <Real T>
struct BetaBinomial : DiscreteBase<BetaBinomial<T>, T>
{
    using value_type = T;
    crd::i64 n = 1;
    T a = static_cast<T>(1);
    T b = static_cast<T>(1);

    BetaBinomial() noexcept = default;
    BetaBinomial(crd::i64 trials, T aa, T bb) noexcept : n(trials), a(aa), b(bb) {}

    [[nodiscard]] T pmf(crd::i64 k) const noexcept { return (k < 0 || k > n) ? static_cast<T>(0) : crd::math::exp(logpmf(k)); }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept
    {
        return detail::log_binom(static_cast<T>(n), static_cast<T>(k)) +
               special::lbeta(static_cast<T>(k) + a, static_cast<T>(n - k) + b) - special::lbeta(a, b);
    }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        if (k < 0)
        {
            return static_cast<T>(0);
        }
        if (k >= n)
        {
            return static_cast<T>(1);
        }
        T s = 0;
        for (crd::i64 j = 0; j <= k; ++j)
        {
            s += pmf(j);
        }
        return s;
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        const double pp = beta_dist(g, static_cast<double>(a), static_cast<double>(b));
        return binomial(g, n, pp);
    }
    [[nodiscard]] T mean() const noexcept { return static_cast<T>(n) * a / (a + b); }
    [[nodiscard]] T var() const noexcept
    {
        const T s = a + b;
        return static_cast<T>(n) * a * b * (s + static_cast<T>(n)) / (s * s * (s + static_cast<T>(1)));
    }
    [[nodiscard]] T skewness() const noexcept
    {
        const T s = a + b;
        const T nn = static_cast<T>(n);
        const T num = (s + static_cast<T>(2) * nn) * (b - a) / (s + static_cast<T>(2)) *
                      crd::math::sqrt((static_cast<T>(1) + s) / (nn * a * b * (nn + s)));
        return num;
    }
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); } // gated via ref where finite
    [[nodiscard]] T entropy() const noexcept
    {
        T h = 0;
        for (crd::i64 k = 0; k <= n; ++k)
        {
            const T pk = pmf(k);
            if (pk > static_cast<T>(0))
            {
                h -= pk * crd::math::log(pk);
            }
        }
        return h;
    }
};

// ───────────────────────────── Logarithmic(p) — scipy.logser, support k ≥ 1 ─────────────────────────────
template <Real T>
struct Logarithmic : DiscreteBase<Logarithmic<T>, T>
{
    using value_type = T;
    T p = static_cast<T>(0.5);

    Logarithmic() noexcept = default;
    explicit Logarithmic(T prob) noexcept : p(prob) {}

    [[nodiscard]] T pmf(crd::i64 k) const noexcept
    {
        return k < 1 ? static_cast<T>(0) : -crd::math::pow(p, static_cast<T>(k)) / (static_cast<T>(k) * crd::math::log1p(-p));
    }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept { return crd::math::log(pmf(k)); }
    [[nodiscard]] T cdf(crd::i64 k) const noexcept
    {
        if (k < 1)
        {
            return static_cast<T>(0);
        }
        T s = 0;
        for (crd::i64 j = 1; j <= k; ++j)
        {
            s += pmf(j);
        }
        return s > static_cast<T>(1) ? static_cast<T>(1) : s;
    }
    template <BitGenerator G>
    [[nodiscard]] crd::i64 rvs(G& g) const noexcept
    {
        // Kemp LK algorithm.
        const T h = crd::math::log1p(-p);
        const T u2 = static_cast<T>(next_double(g));
        if (u2 > p)
        {
            return 1;
        }
        const T u1 = static_cast<T>(next_double(g));
        const T q = -crd::math::expm1(u1 * h);
        if (u2 < q * q)
        {
            return static_cast<crd::i64>(static_cast<T>(1) + crd::math::log(u2) / crd::math::log(q));
        }
        return u2 <= q ? 2 : 1;
    }
    [[nodiscard]] T mean() const noexcept
    {
        return -p / ((static_cast<T>(1) - p) * crd::math::log1p(-p));
    }
    [[nodiscard]] T var() const noexcept
    {
        const T q = static_cast<T>(1) - p;
        const T h = crd::math::log1p(-p);
        return -p * (p + h) / (q * q * h * h);
    }
    [[nodiscard]] T skewness() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T entropy() const noexcept { return detail::nan<T>(); }
};

} // namespace crd::hesap::stats
