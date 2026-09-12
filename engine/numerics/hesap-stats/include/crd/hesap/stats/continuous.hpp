#pragma once

// crd-hesap-stats v12-h — ~25 univariate CONTINUOUS distributions on the Distribution<T> framework. Each is a small
// value type (params + the full pdf/logpdf/cdf/sf/ppf/rvs + moments/entropy surface). CDFs/PPFs reuse the shipped
// hesap-special incomplete-gamma/beta/erf inverses (SANITY rule 8); rvs reuses the v12-f samplers. f32/f64 templated.
// scipy.stats loc/scale conventions throughout (gated <1e-12 vs scipy in test_continuous.cpp).

#include <crd/hesap/stats/distribution.hpp>
#include <crd/hesap/stats/samplers.hpp>
#include <crd/hesap/stats/ziggurat.hpp>

#include <crd/hesap/special/bessel.hpp>     // cyl_bessel_i (von Mises / Rice / Nakagami-adjacent)
#include <crd/hesap/special/erf.hpp>        // erf / erfc / erfinv (normal family)
#include <crd/hesap/special/gamma.hpp>      // lgamma / gamma / digamma (moments + entropy)
#include <crd/hesap/special/incomplete.hpp> // gammainc_p/q (+inv), betainc (+inv)
#include <crd/hesap/special/marcum.hpp>     // marcum_q (Rice cdf)

#include <crd/containers/span.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::stats
{
namespace detail
{
// Standard normal cdf Φ(z) = ½ erfc(−z/√2) — shared by Normal / LogNormal / Wald / HalfNormal.
template <Real T>
[[nodiscard]] T phi(T z) noexcept
{
    return static_cast<T>(0.5) * special::erfc(-z / kSqrt2<T>);
}
} // namespace detail

// ───────────────────────────── Normal(μ, σ) ─────────────────────────────
template <Real T>
struct Normal : ContinuousBase<Normal<T>, T>
{
    using value_type = T;
    T mu = static_cast<T>(0);
    T sigma = static_cast<T>(1);

    Normal() noexcept = default;
    Normal(T mean, T stddev) noexcept : mu(mean), sigma(stddev) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        const T z = (x - mu) / sigma;
        return crd::math::exp(static_cast<T>(-0.5) * z * z) / (sigma * detail::kSqrt2Pi<T>);
    }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T z = (x - mu) / sigma;
        return static_cast<T>(-0.5) * z * z - crd::math::log(sigma) - static_cast<T>(0.5) * detail::kLn2Pi<T>;
    }
    [[nodiscard]] T cdf(T x) const noexcept { return detail::phi((x - mu) / sigma); }
    [[nodiscard]] T sf(T x) const noexcept { return static_cast<T>(0.5) * special::erfc((x - mu) / (sigma * detail::kSqrt2<T>)); }
    [[nodiscard]] T ppf(T p) const noexcept { return mu + sigma * special::ndtri(p); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return mu + sigma * static_cast<T>(standard_normal(g));
    }
    [[nodiscard]] T mean() const noexcept { return mu; }
    [[nodiscard]] T var() const noexcept { return sigma * sigma; }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(0); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(0); }
    [[nodiscard]] T entropy() const noexcept
    {
        return static_cast<T>(0.5) * (detail::kLn2Pi<T> + static_cast<T>(1)) + crd::math::log(sigma);
    }
    [[nodiscard]] T mgf(T t) const noexcept { return crd::math::exp(mu * t + static_cast<T>(0.5) * sigma * sigma * t * t); }
    [[nodiscard]] static Normal fit(crd::containers::Span<const T> data) noexcept
    {
        T s = 0;
        T s2 = 0;
        for (T v : data)
        {
            s += v;
            s2 += v * v;
        }
        const T n = static_cast<T>(data.size());
        const T m = s / n;
        return Normal(m, crd::math::sqrt(s2 / n - m * m));
    }
};

// ───────────────────────────── LogNormal(μ, σ) ── (of the underlying normal) ─────────────────────────────
template <Real T>
struct LogNormal : ContinuousBase<LogNormal<T>, T>
{
    using value_type = T;
    T mu = static_cast<T>(0);
    T sigma = static_cast<T>(1);

    LogNormal() noexcept = default;
    LogNormal(T m, T s) noexcept : mu(m), sigma(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T z = (crd::math::log(x) - mu) / sigma;
        return crd::math::exp(static_cast<T>(-0.5) * z * z) / (x * sigma * detail::kSqrt2Pi<T>);
    }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T z = (crd::math::log(x) - mu) / sigma;
        return static_cast<T>(-0.5) * z * z - crd::math::log(x * sigma) - static_cast<T>(0.5) * detail::kLn2Pi<T>;
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x <= static_cast<T>(0) ? static_cast<T>(0) : detail::phi((crd::math::log(x) - mu) / sigma);
    }
    [[nodiscard]] T ppf(T p) const noexcept { return crd::math::exp(mu + sigma * special::ndtri(p)); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return crd::math::exp(mu + sigma * static_cast<T>(standard_normal(g)));
    }
    [[nodiscard]] T mean() const noexcept { return crd::math::exp(mu + static_cast<T>(0.5) * sigma * sigma); }
    [[nodiscard]] T var() const noexcept
    {
        const T s2 = sigma * sigma;
        return (crd::math::exp(s2) - static_cast<T>(1)) * crd::math::exp(static_cast<T>(2) * mu + s2);
    }
    [[nodiscard]] T skewness() const noexcept
    {
        const T e = crd::math::exp(sigma * sigma);
        return (e + static_cast<T>(2)) * crd::math::sqrt(e - static_cast<T>(1));
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        const T e = crd::math::exp(sigma * sigma);
        return e * e * e * e + static_cast<T>(2) * e * e * e + static_cast<T>(3) * e * e - static_cast<T>(6);
    }
    [[nodiscard]] T entropy() const noexcept
    {
        return mu + static_cast<T>(0.5) * (detail::kLn2Pi<T> + static_cast<T>(1)) + crd::math::log(sigma);
    }
};

// ───────────────────────────── Exponential(scale = 1/rate) ─────────────────────────────
template <Real T>
struct Exponential : ContinuousBase<Exponential<T>, T>
{
    using value_type = T;
    T scale = static_cast<T>(1);

    Exponential() noexcept = default;
    explicit Exponential(T s) noexcept : scale(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(0) : crd::math::exp(-x / scale) / scale;
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return -x / scale - crd::math::log(scale); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(0) : -crd::math::expm1(-x / scale);
    }
    [[nodiscard]] T sf(T x) const noexcept { return x < static_cast<T>(0) ? static_cast<T>(1) : crd::math::exp(-x / scale); }
    [[nodiscard]] T ppf(T p) const noexcept { return -scale * crd::math::log1p(-p); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return scale * static_cast<T>(standard_exponential(g));
    }
    [[nodiscard]] T mean() const noexcept { return scale; }
    [[nodiscard]] T var() const noexcept { return scale * scale; }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(2); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(6); }
    [[nodiscard]] T entropy() const noexcept { return static_cast<T>(1) + crd::math::log(scale); }
    [[nodiscard]] T mgf(T t) const noexcept { return static_cast<T>(1) / (static_cast<T>(1) - scale * t); }
    [[nodiscard]] static Exponential fit(crd::containers::Span<const T> data) noexcept
    {
        T s = 0;
        for (T v : data)
        {
            s += v;
        }
        return Exponential(s / static_cast<T>(data.size()));
    }
};

// ───────────────────────────── Gamma(shape a, scale) ─────────────────────────────
template <Real T>
struct Gamma : ContinuousBase<Gamma<T>, T>
{
    using value_type = T;
    T shape = static_cast<T>(1);
    T scale = static_cast<T>(1);

    Gamma() noexcept : m_lg(special::lgamma(shape)) {}
    Gamma(T a, T s) noexcept : shape(a), scale(s), m_lg(special::lgamma(a)) {}

    [[nodiscard]] T pdf(T x) const noexcept { return x < static_cast<T>(0) ? static_cast<T>(0) : crd::math::exp(logpdf(x)); }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        return (shape - static_cast<T>(1)) * crd::math::log(x) - x / scale - shape * crd::math::log(scale) - m_lg;
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(0) : special::gammainc_p(shape, x / scale, m_lg);
    }
    [[nodiscard]] T sf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(1) : special::gammainc_q(shape, x / scale, m_lg);
    }
    [[nodiscard]] T ppf(T p) const noexcept { return scale * special::gammainc_p_inv(shape, p); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return static_cast<T>(gamma_dist(g, static_cast<double>(shape), static_cast<double>(scale)));
    }
    [[nodiscard]] T mean() const noexcept { return shape * scale; }
    [[nodiscard]] T var() const noexcept { return shape * scale * scale; }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(2) / crd::math::sqrt(shape); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(6) / shape; }
    [[nodiscard]] T entropy() const noexcept
    {
        return shape + crd::math::log(scale) + special::lgamma(shape) + (static_cast<T>(1) - shape) * special::digamma(shape);
    }
    [[nodiscard]] T mgf(T t) const noexcept { return crd::math::pow(static_cast<T>(1) - scale * t, -shape); }

private:
    T m_lg = static_cast<T>(0); // lgamma(shape), amortised across cdf/sf/pdf
};

// ───────────────────────────── Beta(a, b) on [0,1] ─────────────────────────────
template <Real T>
struct Beta : ContinuousBase<Beta<T>, T>
{
    using value_type = T;
    T a = static_cast<T>(1);
    T b = static_cast<T>(1);

    Beta() noexcept : m_lbeta(special::lbeta(a, b)) {}
    Beta(T aa, T bb) noexcept : a(aa), b(bb), m_lbeta(special::lbeta(aa, bb)) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        return (x < static_cast<T>(0) || x > static_cast<T>(1)) ? static_cast<T>(0) : crd::math::exp(logpdf(x));
    }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        return (a - static_cast<T>(1)) * crd::math::log(x) + (b - static_cast<T>(1)) * crd::math::log1p(-x) - m_lbeta;
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        return x >= static_cast<T>(1) ? static_cast<T>(1) : special::betainc(a, b, x, m_lbeta);
    }
    [[nodiscard]] T ppf(T p) const noexcept { return special::betainc_inv(a, b, p, m_lbeta); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return static_cast<T>(beta_dist(g, static_cast<double>(a), static_cast<double>(b)));
    }
    [[nodiscard]] T mean() const noexcept { return a / (a + b); }
    [[nodiscard]] T var() const noexcept
    {
        const T s = a + b;
        return a * b / (s * s * (s + static_cast<T>(1)));
    }
    [[nodiscard]] T skewness() const noexcept
    {
        const T s = a + b;
        return static_cast<T>(2) * (b - a) * crd::math::sqrt(s + static_cast<T>(1)) /
               ((s + static_cast<T>(2)) * crd::math::sqrt(a * b));
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        const T s = a + b;
        const T num = static_cast<T>(6) * ((a - b) * (a - b) * (s + static_cast<T>(1)) - a * b * (s + static_cast<T>(2)));
        return num / (a * b * (s + static_cast<T>(2)) * (s + static_cast<T>(3)));
    }
    [[nodiscard]] T entropy() const noexcept
    {
        return m_lbeta - (a - static_cast<T>(1)) * special::digamma(a) - (b - static_cast<T>(1)) * special::digamma(b) +
               (a + b - static_cast<T>(2)) * special::digamma(a + b);
    }

private:
    T m_lbeta = static_cast<T>(0); // lgamma(a)+lgamma(b)−lgamma(a+b), amortised across cdf/ppf/pdf
};

// ───────────────────────────── ChiSquared(k) = Gamma(k/2, 2) ─────────────────────────────
template <Real T>
struct ChiSquared : ContinuousBase<ChiSquared<T>, T>
{
    using value_type = T;
    T k = static_cast<T>(1);

    ChiSquared() noexcept : m_lgh(special::lgamma(static_cast<T>(0.5) * k)) {}
    explicit ChiSquared(T df) noexcept : k(df), m_lgh(special::lgamma(static_cast<T>(0.5) * df)) {}

    [[nodiscard]] T pdf(T x) const noexcept { return x < static_cast<T>(0) ? static_cast<T>(0) : crd::math::exp(logpdf(x)); }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T h = static_cast<T>(0.5) * k;
        return (h - static_cast<T>(1)) * crd::math::log(x) - static_cast<T>(0.5) * x - h * detail::kLn2<T> - m_lgh;
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(0)
                                     : special::gammainc_p(static_cast<T>(0.5) * k, static_cast<T>(0.5) * x, m_lgh);
    }
    [[nodiscard]] T sf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(1)
                                     : special::gammainc_q(static_cast<T>(0.5) * k, static_cast<T>(0.5) * x, m_lgh);
    }
    [[nodiscard]] T ppf(T p) const noexcept { return static_cast<T>(2) * special::gammainc_p_inv(static_cast<T>(0.5) * k, p); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return static_cast<T>(chi_squared(g, static_cast<double>(k)));
    }
    [[nodiscard]] T mean() const noexcept { return k; }
    [[nodiscard]] T var() const noexcept { return static_cast<T>(2) * k; }
    [[nodiscard]] T skewness() const noexcept { return crd::math::sqrt(static_cast<T>(8) / k); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(12) / k; }
    [[nodiscard]] T entropy() const noexcept
    {
        const T h = static_cast<T>(0.5) * k; // ChiSquared = Gamma(k/2, scale=2): a=h, ln(scale)=ln 2
        return h + detail::kLn2<T> + m_lgh + (static_cast<T>(1) - h) * special::digamma(h);
    }

private:
    T m_lgh = static_cast<T>(0); // lgamma(k/2), amortised across cdf/sf/pdf
};

// ───────────────────────────── StudentT(ν) ─────────────────────────────
template <Real T>
struct StudentT : ContinuousBase<StudentT<T>, T>
{
    using value_type = T;
    T nu = static_cast<T>(1);

    StudentT() noexcept : m_lbeta(special::lbeta(static_cast<T>(0.5) * nu, static_cast<T>(0.5))) {}
    explicit StudentT(T df) noexcept : nu(df), m_lbeta(special::lbeta(static_cast<T>(0.5) * df, static_cast<T>(0.5))) {}

    [[nodiscard]] T pdf(T x) const noexcept { return crd::math::exp(logpdf(x)); }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T h = static_cast<T>(0.5) * nu;
        // lgamma((ν+1)/2) − lgamma(ν/2) = lgamma(½) − lbeta(ν/2,½); lgamma(½)=½ln π.
        return static_cast<T>(0.5) * crd::math::log(detail::kPi<T>) - m_lbeta -
               static_cast<T>(0.5) * crd::math::log(nu * detail::kPi<T>) -
               (h + static_cast<T>(0.5)) * crd::math::log1p(x * x / nu);
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        const T z = nu / (nu + x * x);
        const T ib = special::betainc(static_cast<T>(0.5) * nu, static_cast<T>(0.5), z, m_lbeta);
        return x >= static_cast<T>(0) ? static_cast<T>(1) - static_cast<T>(0.5) * ib : static_cast<T>(0.5) * ib;
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        if (p <= static_cast<T>(0))
        {
            return -std::numeric_limits<T>::infinity();
        }
        if (p >= static_cast<T>(1))
        {
            return std::numeric_limits<T>::infinity();
        }
        if (p == static_cast<T>(0.5))
        {
            return static_cast<T>(0);
        }
        // Dedicated t-quantile = Hill (1970, AS 396): a direct closed formula to ~1e-5 (NO betainc iteration — this
        // is what makes scipy's stdtrit fast), then ONE Halley polish on the cdf for full accuracy. Beats stdtrit.
        if (nu >= static_cast<T>(3))
        {
            const bool neg = p < static_cast<T>(0.5);
            const T q = static_cast<T>(2) * (neg ? p : static_cast<T>(1) - p); // two-tailed prob, ≤ 1
            const T a = static_cast<T>(1) / (nu - static_cast<T>(0.5));
            const T b = static_cast<T>(48) / (a * a);
            T c = ((static_cast<T>(20700) * a / b - static_cast<T>(98)) * a - static_cast<T>(16)) * a + static_cast<T>(96.36);
            const T d =
                ((static_cast<T>(94.5) / (b + c) - static_cast<T>(3)) / b + static_cast<T>(1)) *
                crd::math::sqrt(a * detail::kPi<T> * static_cast<T>(0.5)) * nu;
            T y = crd::math::pow(d * q, static_cast<T>(2) / nu);
            if (y > a + static_cast<T>(0.05))
            {
                const T x = special::ndtri(static_cast<T>(1) - static_cast<T>(0.5) * q); // |normal deviate|, P(Z>x)=q/2
                y = x * x;
                if (nu < static_cast<T>(5))
                {
                    c += static_cast<T>(0.3) * (nu - static_cast<T>(4.5)) * (x + static_cast<T>(0.6));
                }
                c = (((static_cast<T>(0.05) * d * x - static_cast<T>(5)) * x - static_cast<T>(7)) * x - static_cast<T>(2)) * x +
                    b + c;
                y = (((((static_cast<T>(0.4) * y + static_cast<T>(6.3)) * y + static_cast<T>(36)) * y + static_cast<T>(94.5)) / c -
                      y - static_cast<T>(3)) /
                         b +
                     static_cast<T>(1)) *
                    x;
                y = a * y * y;
                y = y > static_cast<T>(0.002) ? crd::math::exp(y) - static_cast<T>(1) : static_cast<T>(0.5) * y * y + y;
            }
            else
            {
                y = ((static_cast<T>(1) /
                          (((nu + static_cast<T>(6)) / (nu * y) - static_cast<T>(0.089) * d - static_cast<T>(0.822)) *
                           (nu + static_cast<T>(2)) * static_cast<T>(3)) +
                      static_cast<T>(0.5) / (nu + static_cast<T>(4))) *
                         y -
                     static_cast<T>(1)) *
                        (nu + static_cast<T>(1)) / (nu + static_cast<T>(2)) +
                    static_cast<T>(1) / y;
            }
            T t = crd::math::sqrt(nu * y);
            if (neg)
            {
                t = -t;
            }
            // One Halley polish (cubic) → full precision from Hill's ~1e-5 start.
            const T dpdf = pdf(t);
            if (dpdf > static_cast<T>(0))
            {
                const T u = (cdf(t) - p) / dpdf;
                const T ddd = -(nu + static_cast<T>(1)) * t / (nu + t * t);
                t -= u / (static_cast<T>(1) - static_cast<T>(0.5) * u * ddd);
            }
            return t;
        }
        // ν < 3: Hill's expansion is weakest here; the robust generic beta-inverse handles small ν.
        const bool lower = p < static_cast<T>(0.5);
        const T pp = lower ? static_cast<T>(2) * p : static_cast<T>(2) * (static_cast<T>(1) - p);
        const T z = special::betainc_inv(static_cast<T>(0.5) * nu, static_cast<T>(0.5), pp, m_lbeta);
        const T x = crd::math::sqrt(nu * (static_cast<T>(1) - z) / z);
        return lower ? -x : x;
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const double z = standard_normal(g);
        const double c = chi_squared(g, static_cast<double>(nu));
        return static_cast<T>(z / crd::math::sqrt(c / static_cast<double>(nu)));
    }
    [[nodiscard]] T mean() const noexcept { return nu > static_cast<T>(1) ? static_cast<T>(0) : detail::nan<T>(); }
    [[nodiscard]] T var() const noexcept
    {
        if (nu > static_cast<T>(2))
        {
            return nu / (nu - static_cast<T>(2));
        }
        return nu > static_cast<T>(1) ? std::numeric_limits<T>::infinity() : detail::nan<T>();
    }
    [[nodiscard]] T skewness() const noexcept { return nu > static_cast<T>(3) ? static_cast<T>(0) : detail::nan<T>(); }
    [[nodiscard]] T kurtosis() const noexcept
    {
        if (nu > static_cast<T>(4))
        {
            return static_cast<T>(6) / (nu - static_cast<T>(4));
        }
        return nu > static_cast<T>(2) ? std::numeric_limits<T>::infinity() : detail::nan<T>();
    }
    [[nodiscard]] T entropy() const noexcept
    {
        const T h = static_cast<T>(0.5) * nu;
        return (h + static_cast<T>(0.5)) * (special::digamma(h + static_cast<T>(0.5)) - special::digamma(h)) +
               static_cast<T>(0.5) * crd::math::log(nu) + m_lbeta;
    }

private:
    T m_lbeta = static_cast<T>(0); // lbeta(ν/2, ½), amortised across cdf/ppf/pdf
};

// ───────────────────────────── FisherF(d1, d2) ─────────────────────────────
template <Real T>
struct FisherF : ContinuousBase<FisherF<T>, T>
{
    using value_type = T;
    T d1 = static_cast<T>(1);
    T d2 = static_cast<T>(1);

    FisherF() noexcept : m_lbeta(special::lbeta(static_cast<T>(0.5) * d1, static_cast<T>(0.5) * d2)) {}
    FisherF(T n1, T n2) noexcept
        : d1(n1), d2(n2), m_lbeta(special::lbeta(static_cast<T>(0.5) * n1, static_cast<T>(0.5) * n2))
    {
    }

    [[nodiscard]] T pdf(T x) const noexcept { return x <= static_cast<T>(0) ? static_cast<T>(0) : crd::math::exp(logpdf(x)); }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T a = static_cast<T>(0.5) * d1;
        return a * crd::math::log(d1 / d2) + (a - static_cast<T>(1)) * crd::math::log(x) -
               (a + static_cast<T>(0.5) * d2) * crd::math::log1p(d1 * x / d2) - m_lbeta;
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        return special::betainc(static_cast<T>(0.5) * d1, static_cast<T>(0.5) * d2, d1 * x / (d1 * x + d2), m_lbeta);
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        const T z = special::betainc_inv(static_cast<T>(0.5) * d1, static_cast<T>(0.5) * d2, p, m_lbeta);
        return d2 * z / (d1 * (static_cast<T>(1) - z));
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const double u = chi_squared(g, static_cast<double>(d1)) / static_cast<double>(d1);
        const double v = chi_squared(g, static_cast<double>(d2)) / static_cast<double>(d2);
        return static_cast<T>(u / v);
    }
    [[nodiscard]] T mean() const noexcept
    {
        return d2 > static_cast<T>(2) ? d2 / (d2 - static_cast<T>(2)) : detail::nan<T>();
    }
    [[nodiscard]] T var() const noexcept
    {
        if (d2 <= static_cast<T>(4))
        {
            return detail::nan<T>();
        }
        const T num = static_cast<T>(2) * d2 * d2 * (d1 + d2 - static_cast<T>(2));
        const T den = d1 * (d2 - static_cast<T>(2)) * (d2 - static_cast<T>(2)) * (d2 - static_cast<T>(4));
        return num / den;
    }
    [[nodiscard]] T skewness() const noexcept
    {
        if (d2 <= static_cast<T>(6))
        {
            return detail::nan<T>();
        }
        const T n = (static_cast<T>(2) * d1 + d2 - static_cast<T>(2)) * crd::math::sqrt(static_cast<T>(8) * (d2 - static_cast<T>(4)));
        const T d = (d2 - static_cast<T>(6)) * crd::math::sqrt(d1 * (d1 + d2 - static_cast<T>(2)));
        return n / d;
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        if (d2 <= static_cast<T>(8))
        {
            return detail::nan<T>();
        }
        const T t1 = d1 * (static_cast<T>(5) * d2 - static_cast<T>(22)) * (d1 + d2 - static_cast<T>(2));
        const T t2 = (d2 - static_cast<T>(4)) * (d2 - static_cast<T>(2)) * (d2 - static_cast<T>(2));
        const T den = d1 * (d2 - static_cast<T>(6)) * (d2 - static_cast<T>(8)) * (d1 + d2 - static_cast<T>(2));
        return static_cast<T>(12) * (t1 + t2) / den;
    }
    [[nodiscard]] T entropy() const noexcept
    {
        const T a = static_cast<T>(0.5) * d1;
        const T b = static_cast<T>(0.5) * d2;
        return crd::math::log(d2 / d1) + m_lbeta + (static_cast<T>(1) - a) * special::digamma(a) -
               (static_cast<T>(1) + b) * special::digamma(b) + (a + b) * special::digamma(a + b);
    }

private:
    T m_lbeta = static_cast<T>(0); // lbeta(d1/2, d2/2), amortised across cdf/ppf/pdf
};

// ───────────────────────────── Cauchy(loc, scale) ─────────────────────────────
template <Real T>
struct Cauchy : ContinuousBase<Cauchy<T>, T>
{
    using value_type = T;
    T loc = static_cast<T>(0);
    T scale = static_cast<T>(1);

    Cauchy() noexcept = default;
    Cauchy(T l, T s) noexcept : loc(l), scale(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        const T z = (x - loc) / scale;
        return static_cast<T>(1) / (detail::kPi<T> * scale * (static_cast<T>(1) + z * z));
    }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T z = (x - loc) / scale;
        return -crd::math::log(detail::kPi<T> * scale * (static_cast<T>(1) + z * z));
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return static_cast<T>(0.5) + crd::math::atan((x - loc) / scale) * detail::kInvPi<T>; // *1/π not ÷π
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        return loc + scale * crd::math::tan(detail::kPi<T> * (p - static_cast<T>(0.5)));
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return loc + scale * crd::math::tan(detail::kPi<T> * (static_cast<T>(next_double(g)) - static_cast<T>(0.5)));
    }
    [[nodiscard]] T mean() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T var() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T skewness() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T entropy() const noexcept { return crd::math::log(static_cast<T>(4) * detail::kPi<T> * scale); }
};

// ───────────────────────────── Laplace(loc, scale) ─────────────────────────────
template <Real T>
struct Laplace : ContinuousBase<Laplace<T>, T>
{
    using value_type = T;
    T loc = static_cast<T>(0);
    T scale = static_cast<T>(1);

    Laplace() noexcept = default;
    Laplace(T l, T s) noexcept : loc(l), scale(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        return crd::math::exp(-crd::math::fabs(x - loc) / scale) / (static_cast<T>(2) * scale);
    }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        return -crd::math::fabs(x - loc) / scale - crd::math::log(static_cast<T>(2) * scale);
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        const T z = (x - loc) / scale;
        return z < static_cast<T>(0) ? static_cast<T>(0.5) * crd::math::exp(z)
                                     : static_cast<T>(1) - static_cast<T>(0.5) * crd::math::exp(-z);
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        return p < static_cast<T>(0.5) ? loc + scale * crd::math::log(static_cast<T>(2) * p)
                                       : loc - scale * crd::math::log(static_cast<T>(2) * (static_cast<T>(1) - p));
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const T u = static_cast<T>(next_double(g)) - static_cast<T>(0.5);
        return u < static_cast<T>(0) ? loc + scale * crd::math::log(static_cast<T>(1) + static_cast<T>(2) * u)
                                     : loc - scale * crd::math::log(static_cast<T>(1) - static_cast<T>(2) * u);
    }
    [[nodiscard]] T mean() const noexcept { return loc; }
    [[nodiscard]] T var() const noexcept { return static_cast<T>(2) * scale * scale; }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(0); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(3); }
    [[nodiscard]] T entropy() const noexcept { return static_cast<T>(1) + crd::math::log(static_cast<T>(2) * scale); }
};

// ───────────────────────────── Logistic(loc, scale) ─────────────────────────────
template <Real T>
struct Logistic : ContinuousBase<Logistic<T>, T>
{
    using value_type = T;
    T loc = static_cast<T>(0);
    T scale = static_cast<T>(1);

    Logistic() noexcept = default;
    Logistic(T l, T s) noexcept : loc(l), scale(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        const T e = crd::math::exp(-crd::math::fabs(x - loc) / scale);
        return e / (scale * (static_cast<T>(1) + e) * (static_cast<T>(1) + e));
    }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T z = (x - loc) / scale;
        return -z - crd::math::log(scale) - static_cast<T>(2) * crd::math::log1p(crd::math::exp(-z));
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return static_cast<T>(1) / (static_cast<T>(1) + crd::math::exp(-(x - loc) / scale));
    }
    [[nodiscard]] T ppf(T p) const noexcept { return loc + scale * crd::math::log(p / (static_cast<T>(1) - p)); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const T u = static_cast<T>(next_double(g));
        return loc + scale * crd::math::log(u / (static_cast<T>(1) - u));
    }
    [[nodiscard]] T mean() const noexcept { return loc; }
    [[nodiscard]] T var() const noexcept
    {
        return detail::kPi<T> * detail::kPi<T> * scale * scale / static_cast<T>(3);
    }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(0); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(1.2); }
    [[nodiscard]] T entropy() const noexcept { return crd::math::log(scale) + static_cast<T>(2); }
};

// ───────────────────────────── Weibull(c, scale) — Weibull_min ─────────────────────────────
template <Real T>
struct Weibull : ContinuousBase<Weibull<T>, T>
{
    using value_type = T;
    T c = static_cast<T>(1);
    T scale = static_cast<T>(1);

    Weibull() noexcept = default;
    Weibull(T shape, T s) noexcept : c(shape), scale(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        if (x < static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T z = x / scale;
        return (c / scale) * crd::math::pow(z, c - static_cast<T>(1)) * crd::math::exp(-crd::math::pow(z, c));
    }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T z = x / scale;
        return crd::math::log(c / scale) + (c - static_cast<T>(1)) * crd::math::log(z) - crd::math::pow(z, c);
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(0) : -crd::math::expm1(-crd::math::pow(x / scale, c));
    }
    [[nodiscard]] T sf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(1) : crd::math::exp(-crd::math::pow(x / scale, c));
    }
    [[nodiscard]] T ppf(T p) const noexcept { return scale * crd::math::pow(-crd::math::log1p(-p), static_cast<T>(1) / c); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return scale * crd::math::pow(static_cast<T>(standard_exponential(g)), static_cast<T>(1) / c);
    }
    [[nodiscard]] T mean() const noexcept { return scale * special::gamma(static_cast<T>(1) + static_cast<T>(1) / c); }
    [[nodiscard]] T var() const noexcept
    {
        const T g1 = special::gamma(static_cast<T>(1) + static_cast<T>(1) / c);
        const T g2 = special::gamma(static_cast<T>(1) + static_cast<T>(2) / c);
        return scale * scale * (g2 - g1 * g1);
    }
    [[nodiscard]] T skewness() const noexcept
    {
        const T g1 = special::gamma(static_cast<T>(1) + static_cast<T>(1) / c);
        const T g2 = special::gamma(static_cast<T>(1) + static_cast<T>(2) / c);
        const T g3 = special::gamma(static_cast<T>(1) + static_cast<T>(3) / c);
        const T s = g2 - g1 * g1;
        return (g3 - static_cast<T>(3) * g1 * g2 + static_cast<T>(2) * g1 * g1 * g1) / crd::math::pow(s, static_cast<T>(1.5));
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        const T g1 = special::gamma(static_cast<T>(1) + static_cast<T>(1) / c);
        const T g2 = special::gamma(static_cast<T>(1) + static_cast<T>(2) / c);
        const T g3 = special::gamma(static_cast<T>(1) + static_cast<T>(3) / c);
        const T g4 = special::gamma(static_cast<T>(1) + static_cast<T>(4) / c);
        const T s = g2 - g1 * g1;
        const T num = g4 - static_cast<T>(4) * g1 * g3 + static_cast<T>(6) * g1 * g1 * g2 -
                      static_cast<T>(3) * g1 * g1 * g1 * g1;
        return num / (s * s) - static_cast<T>(3);
    }
    [[nodiscard]] T entropy() const noexcept
    {
        return detail::kEuler<T> * (static_cast<T>(1) - static_cast<T>(1) / c) + crd::math::log(scale / c) + static_cast<T>(1);
    }
};

// ───────────────────────────── Gumbel(loc, scale) — gumbel_r (right-skew) ─────────────────────────────
template <Real T>
struct Gumbel : ContinuousBase<Gumbel<T>, T>
{
    using value_type = T;
    T loc = static_cast<T>(0);
    T scale = static_cast<T>(1);

    Gumbel() noexcept = default;
    Gumbel(T l, T s) noexcept : loc(l), scale(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        const T z = (x - loc) / scale;
        return crd::math::exp(-(z + crd::math::exp(-z))) / scale;
    }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T z = (x - loc) / scale;
        return -(z + crd::math::exp(-z)) - crd::math::log(scale);
    }
    [[nodiscard]] T cdf(T x) const noexcept { return crd::math::exp(-crd::math::exp(-(x - loc) / scale)); }
    [[nodiscard]] T sf(T x) const noexcept { return -crd::math::expm1(-crd::math::exp(-(x - loc) / scale)); }
    [[nodiscard]] T ppf(T p) const noexcept { return loc - scale * crd::math::log(-crd::math::log(p)); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return loc - scale * crd::math::log(static_cast<T>(standard_exponential(g)));
    }
    [[nodiscard]] T mean() const noexcept { return loc + scale * detail::kEuler<T>; }
    [[nodiscard]] T var() const noexcept
    {
        return detail::kPi<T> * detail::kPi<T> * scale * scale / static_cast<T>(6);
    }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(1.1395470994046486); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(2.4); }
    [[nodiscard]] T entropy() const noexcept { return crd::math::log(scale) + detail::kEuler<T> + static_cast<T>(1); }
};

// ───────────────────────────── Pareto(b, scale) — x ≥ scale ─────────────────────────────
template <Real T>
struct Pareto : ContinuousBase<Pareto<T>, T>
{
    using value_type = T;
    T b = static_cast<T>(1);
    T scale = static_cast<T>(1);

    Pareto() noexcept = default;
    Pareto(T shape, T s) noexcept : b(shape), scale(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        return x < scale ? static_cast<T>(0) : b * crd::math::pow(scale, b) / crd::math::pow(x, b + static_cast<T>(1));
    }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        return crd::math::log(b) + b * crd::math::log(scale) - (b + static_cast<T>(1)) * crd::math::log(x);
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x < scale ? static_cast<T>(0) : static_cast<T>(1) - crd::math::pow(scale / x, b);
    }
    [[nodiscard]] T sf(T x) const noexcept { return x < scale ? static_cast<T>(1) : crd::math::pow(scale / x, b); }
    [[nodiscard]] T ppf(T p) const noexcept { return scale * crd::math::pow(static_cast<T>(1) - p, -static_cast<T>(1) / b); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return scale * crd::math::exp(static_cast<T>(standard_exponential(g)) / b);
    }
    [[nodiscard]] T mean() const noexcept
    {
        return b > static_cast<T>(1) ? b * scale / (b - static_cast<T>(1)) : std::numeric_limits<T>::infinity();
    }
    [[nodiscard]] T var() const noexcept
    {
        if (b <= static_cast<T>(2))
        {
            return std::numeric_limits<T>::infinity();
        }
        return scale * scale * b / ((b - static_cast<T>(1)) * (b - static_cast<T>(1)) * (b - static_cast<T>(2)));
    }
    [[nodiscard]] T skewness() const noexcept
    {
        if (b <= static_cast<T>(3))
        {
            return detail::nan<T>();
        }
        return static_cast<T>(2) * (static_cast<T>(1) + b) / (b - static_cast<T>(3)) *
               crd::math::sqrt((b - static_cast<T>(2)) / b);
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        if (b <= static_cast<T>(4))
        {
            return detail::nan<T>();
        }
        const T num = static_cast<T>(6) * (b * b * b + b * b - static_cast<T>(6) * b - static_cast<T>(2));
        return num / (b * (b - static_cast<T>(3)) * (b - static_cast<T>(4)));
    }
    [[nodiscard]] T entropy() const noexcept
    {
        return crd::math::log(scale / b) + static_cast<T>(1) / b + static_cast<T>(1);
    }
};

// ───────────────────────────── Rayleigh(scale) ─────────────────────────────
template <Real T>
struct Rayleigh : ContinuousBase<Rayleigh<T>, T>
{
    using value_type = T;
    T scale = static_cast<T>(1);

    Rayleigh() noexcept = default;
    explicit Rayleigh(T s) noexcept : scale(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        if (x < static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T s2 = scale * scale;
        return (x / s2) * crd::math::exp(static_cast<T>(-0.5) * x * x / s2);
    }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        return crd::math::log(x) - static_cast<T>(2) * crd::math::log(scale) - static_cast<T>(0.5) * x * x / (scale * scale);
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(0)
                                     : -crd::math::expm1(static_cast<T>(-0.5) * x * x / (scale * scale));
    }
    [[nodiscard]] T sf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(1) : crd::math::exp(static_cast<T>(-0.5) * x * x / (scale * scale));
    }
    [[nodiscard]] T ppf(T p) const noexcept { return scale * crd::math::sqrt(static_cast<T>(-2) * crd::math::log1p(-p)); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return scale * crd::math::sqrt(static_cast<T>(2) * static_cast<T>(standard_exponential(g)));
    }
    [[nodiscard]] T mean() const noexcept { return scale * crd::math::sqrt(detail::kPi<T> / static_cast<T>(2)); }
    [[nodiscard]] T var() const noexcept
    {
        return (static_cast<T>(4) - detail::kPi<T>) / static_cast<T>(2) * scale * scale;
    }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(0.6311106578189364); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(0.2450893006876380); }
    [[nodiscard]] T entropy() const noexcept
    {
        return static_cast<T>(1) + crd::math::log(scale / detail::kSqrt2<T>) + static_cast<T>(0.5) * detail::kEuler<T>;
    }
};

// ───────────────────────────── Maxwell(scale a) ─────────────────────────────
template <Real T>
struct Maxwell : ContinuousBase<Maxwell<T>, T>
{
    using value_type = T;
    T scale = static_cast<T>(1);

    Maxwell() noexcept = default;
    explicit Maxwell(T a) noexcept : scale(a) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        if (x < static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T a3 = scale * scale * scale;
        const T c = crd::math::sqrt(static_cast<T>(2) / detail::kPi<T>);
        return c * x * x / a3 * crd::math::exp(static_cast<T>(-0.5) * x * x / (scale * scale));
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        if (x < static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T z = x / scale;
        return special::gammainc_p(static_cast<T>(1.5), static_cast<T>(0.5) * z * z);
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        return scale * crd::math::sqrt(static_cast<T>(2) * special::gammainc_p_inv(static_cast<T>(1.5), p));
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return scale * crd::math::sqrt(static_cast<T>(2) * static_cast<T>(gamma_dist(g, 1.5, 1.0)));
    }
    [[nodiscard]] T mean() const noexcept { return static_cast<T>(2) * scale * crd::math::sqrt(static_cast<T>(2) / detail::kPi<T>); }
    [[nodiscard]] T var() const noexcept
    {
        return scale * scale * (static_cast<T>(3) * detail::kPi<T> - static_cast<T>(8)) / detail::kPi<T>;
    }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(0.4856928280495921); }
    [[nodiscard]] T kurtosis() const noexcept
    {
        const T pi = detail::kPi<T>;
        const T den = static_cast<T>(3) * pi - static_cast<T>(8);
        return (static_cast<T>(-12) * pi * pi + static_cast<T>(160) * pi - static_cast<T>(384)) / (den * den);
    }
    [[nodiscard]] T entropy() const noexcept
    {
        return crd::math::log(scale) + static_cast<T>(0.5) * detail::kLn2Pi<T> + detail::kEuler<T> - static_cast<T>(0.5);
    }
};

// ───────────────────────────── Uniform(a, b) ─────────────────────────────
template <Real T>
struct Uniform : ContinuousBase<Uniform<T>, T>
{
    using value_type = T;
    T a = static_cast<T>(0);
    T b = static_cast<T>(1);

    Uniform() noexcept = default;
    Uniform(T lo, T hi) noexcept : a(lo), b(hi) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        return (x < a || x > b) ? static_cast<T>(0) : static_cast<T>(1) / (b - a);
    }
    [[nodiscard]] T logpdf([[maybe_unused]] T x) const noexcept { return -crd::math::log(b - a); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        if (x < a)
        {
            return static_cast<T>(0);
        }
        return x > b ? static_cast<T>(1) : (x - a) / (b - a);
    }
    [[nodiscard]] T ppf(T p) const noexcept { return a + p * (b - a); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return a + static_cast<T>(next_double(g)) * (b - a);
    }
    [[nodiscard]] T mean() const noexcept { return static_cast<T>(0.5) * (a + b); }
    [[nodiscard]] T var() const noexcept { return (b - a) * (b - a) / static_cast<T>(12); }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(0); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(-1.2); }
    [[nodiscard]] T entropy() const noexcept { return crd::math::log(b - a); }
    [[nodiscard]] T mgf(T t) const noexcept
    {
        return t == static_cast<T>(0) ? static_cast<T>(1) : (crd::math::exp(t * b) - crd::math::exp(t * a)) / (t * (b - a));
    }
};

// ───────────────────────────── HalfNormal(scale σ) ─────────────────────────────
template <Real T>
struct HalfNormal : ContinuousBase<HalfNormal<T>, T>
{
    using value_type = T;
    T sigma = static_cast<T>(1);

    HalfNormal() noexcept = default;
    explicit HalfNormal(T s) noexcept : sigma(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        if (x < static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T z = x / sigma;
        return crd::math::sqrt(static_cast<T>(2) / detail::kPi<T>) / sigma * crd::math::exp(static_cast<T>(-0.5) * z * z);
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(0) : special::erf(x / (sigma * detail::kSqrt2<T>));
    }
    [[nodiscard]] T sf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(1) : special::erfc(x / (sigma * detail::kSqrt2<T>));
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        return sigma * special::ndtri(static_cast<T>(0.5) * (static_cast<T>(1) + p)); // σ√2·erfinv(p)
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return sigma * crd::math::fabs(static_cast<T>(standard_normal(g)));
    }
    [[nodiscard]] T mean() const noexcept { return sigma * crd::math::sqrt(static_cast<T>(2) / detail::kPi<T>); }
    [[nodiscard]] T var() const noexcept
    {
        return sigma * sigma * (static_cast<T>(1) - static_cast<T>(2) / detail::kPi<T>);
    }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(0.9952717464311565); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(0.8691773036059736); }
    [[nodiscard]] T entropy() const noexcept
    {
        return static_cast<T>(0.5) * crd::math::log(detail::kPi<T> * sigma * sigma / static_cast<T>(2)) + static_cast<T>(0.5);
    }
};

// ───────────────────────────── HalfCauchy(scale) ─────────────────────────────
template <Real T>
struct HalfCauchy : ContinuousBase<HalfCauchy<T>, T>
{
    using value_type = T;
    T scale = static_cast<T>(1);

    HalfCauchy() noexcept = default;
    explicit HalfCauchy(T s) noexcept : scale(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        if (x < static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T z = x / scale;
        return static_cast<T>(2) / (detail::kPi<T> * scale * (static_cast<T>(1) + z * z));
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(0) : static_cast<T>(2) / detail::kPi<T> * crd::math::atan(x / scale);
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        return scale * crd::math::tan(detail::kPi<T> * p / static_cast<T>(2));
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return scale * crd::math::fabs(crd::math::tan(detail::kPi<T> * (static_cast<T>(next_double(g)) - static_cast<T>(0.5))));
    }
    [[nodiscard]] T mean() const noexcept { return std::numeric_limits<T>::infinity(); }
    [[nodiscard]] T var() const noexcept { return std::numeric_limits<T>::infinity(); }
    [[nodiscard]] T skewness() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T entropy() const noexcept { return crd::math::log(static_cast<T>(2) * detail::kPi<T> * scale); }
};

// ───────────────────────────── Triangular(a, c=mode, b) ─────────────────────────────
template <Real T>
struct Triangular : ContinuousBase<Triangular<T>, T>
{
    using value_type = T;
    T a = static_cast<T>(0);
    T cmode = static_cast<T>(0.5);
    T b = static_cast<T>(1);

    Triangular() noexcept = default;
    Triangular(T lo, T mode, T hi) noexcept : a(lo), cmode(mode), b(hi) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        if (x < a || x > b)
        {
            return static_cast<T>(0);
        }
        if (x < cmode)
        {
            return static_cast<T>(2) * (x - a) / ((b - a) * (cmode - a));
        }
        return static_cast<T>(2) * (b - x) / ((b - a) * (b - cmode));
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        if (x <= a)
        {
            return static_cast<T>(0);
        }
        if (x >= b)
        {
            return static_cast<T>(1);
        }
        if (x < cmode)
        {
            return (x - a) * (x - a) / ((b - a) * (cmode - a));
        }
        return static_cast<T>(1) - (b - x) * (b - x) / ((b - a) * (b - cmode));
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        const T fc = (cmode - a) / (b - a);
        if (p < fc)
        {
            return a + crd::math::sqrt(p * (b - a) * (cmode - a));
        }
        return b - crd::math::sqrt((static_cast<T>(1) - p) * (b - a) * (b - cmode));
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return ppf(static_cast<T>(next_double(g)));
    }
    [[nodiscard]] T mean() const noexcept { return (a + b + cmode) / static_cast<T>(3); }
    [[nodiscard]] T var() const noexcept
    {
        return (a * a + b * b + cmode * cmode - a * b - a * cmode - b * cmode) / static_cast<T>(18);
    }
    [[nodiscard]] T skewness() const noexcept
    {
        const T s2 = a * a + b * b + cmode * cmode - a * b - a * cmode - b * cmode;
        const T num = detail::kSqrt2<T> * (a + b - static_cast<T>(2) * cmode) *
                      (static_cast<T>(2) * a - b - cmode) * (a - static_cast<T>(2) * b + cmode);
        return num / (static_cast<T>(5) * crd::math::pow(s2, static_cast<T>(1.5)));
    }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(-0.6); }
    [[nodiscard]] T entropy() const noexcept
    {
        return static_cast<T>(0.5) + crd::math::log((b - a) / static_cast<T>(2));
    }
};

// ───────────────────────────── InverseGamma(a, scale) ─────────────────────────────
template <Real T>
struct InverseGamma : ContinuousBase<InverseGamma<T>, T>
{
    using value_type = T;
    T a = static_cast<T>(1);
    T scale = static_cast<T>(1);

    InverseGamma() noexcept : m_lg(special::lgamma(a)) {}
    InverseGamma(T shape, T s) noexcept : a(shape), scale(s), m_lg(special::lgamma(shape)) {}

    [[nodiscard]] T pdf(T x) const noexcept { return x <= static_cast<T>(0) ? static_cast<T>(0) : crd::math::exp(logpdf(x)); }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        return a * crd::math::log(scale) - (a + static_cast<T>(1)) * crd::math::log(x) - scale / x - m_lg;
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x <= static_cast<T>(0) ? static_cast<T>(0) : special::gammainc_q(a, scale / x, m_lg);
    }
    [[nodiscard]] T sf(T x) const noexcept
    {
        return x <= static_cast<T>(0) ? static_cast<T>(1) : special::gammainc_p(a, scale / x, m_lg);
    }
    [[nodiscard]] T ppf(T p) const noexcept { return scale / special::gammainc_q_inv(a, p); }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return scale / static_cast<T>(gamma_dist(g, static_cast<double>(a), 1.0));
    }
    [[nodiscard]] T mean() const noexcept
    {
        return a > static_cast<T>(1) ? scale / (a - static_cast<T>(1)) : std::numeric_limits<T>::infinity();
    }
    [[nodiscard]] T var() const noexcept
    {
        if (a <= static_cast<T>(2))
        {
            return std::numeric_limits<T>::infinity();
        }
        return scale * scale / ((a - static_cast<T>(1)) * (a - static_cast<T>(1)) * (a - static_cast<T>(2)));
    }
    [[nodiscard]] T skewness() const noexcept
    {
        return a > static_cast<T>(3) ? static_cast<T>(4) * crd::math::sqrt(a - static_cast<T>(2)) / (a - static_cast<T>(3))
                                     : detail::nan<T>();
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        if (a <= static_cast<T>(4))
        {
            return detail::nan<T>();
        }
        return static_cast<T>(6) * (static_cast<T>(5) * a - static_cast<T>(11)) /
               ((a - static_cast<T>(3)) * (a - static_cast<T>(4)));
    }
    [[nodiscard]] T entropy() const noexcept
    {
        return a + crd::math::log(scale) + m_lg - (static_cast<T>(1) + a) * special::digamma(a);
    }

private:
    T m_lg = static_cast<T>(0); // lgamma(a), amortised across cdf/sf/pdf
};

// ───────────────────────────── Nakagami(m, scale Ω=spread) ─────────────────────────────
template <Real T>
struct Nakagami : ContinuousBase<Nakagami<T>, T>
{
    using value_type = T;
    T m = static_cast<T>(1);
    T scale = static_cast<T>(1);

    Nakagami() noexcept = default;
    Nakagami(T shape, T s) noexcept : m(shape), scale(s) {}

    [[nodiscard]] T pdf(T x) const noexcept { return x < static_cast<T>(0) ? static_cast<T>(0) : crd::math::exp(logpdf(x)); }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T z = x / scale;
        return crd::math::log(static_cast<T>(2)) + m * crd::math::log(m) - special::lgamma(m) +
               (static_cast<T>(2) * m - static_cast<T>(1)) * crd::math::log(z) - m * z * z - crd::math::log(scale);
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        if (x < static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T z = x / scale;
        return special::gammainc_p(m, m * z * z);
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        return scale * crd::math::sqrt(special::gammainc_p_inv(m, p) / m);
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const double y = gamma_dist(g, static_cast<double>(m), 1.0 / static_cast<double>(m));
        return scale * static_cast<T>(crd::math::sqrt(y));
    }
    [[nodiscard]] T mean() const noexcept
    {
        return scale * special::gamma(m + static_cast<T>(0.5)) / special::gamma(m) / crd::math::sqrt(m);
    }
    [[nodiscard]] T var() const noexcept
    {
        const T r = special::gamma(m + static_cast<T>(0.5)) / special::gamma(m);
        return scale * scale * (static_cast<T>(1) - r * r / m);
    }
    [[nodiscard]] T skewness() const noexcept
    {
        const T r = special::gamma(m + static_cast<T>(0.5)) / special::gamma(m) / crd::math::sqrt(m);
        const T v = static_cast<T>(1) - r * r;
        return r * (static_cast<T>(1) - static_cast<T>(4) * m * v) / (static_cast<T>(2) * m * crd::math::pow(v, static_cast<T>(1.5)));
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        // computed from the ref (closed form messy); gated numerically.
        const T r = special::gamma(m + static_cast<T>(0.5)) / special::gamma(m) / crd::math::sqrt(m);
        const T v = static_cast<T>(1) - r * r;
        const T num = static_cast<T>(-6) * r * r * r * r * m + (static_cast<T>(8) * m - static_cast<T>(2)) * r * r -
                      static_cast<T>(2) * m + static_cast<T>(1);
        return num / (m * v * v); // gated vs scipy ref
    }
    [[nodiscard]] T entropy() const noexcept
    {
        return special::lgamma(m) - (m - static_cast<T>(0.5)) * special::digamma(m) -
               static_cast<T>(0.5) * crd::math::log(m) + m - detail::kLn2<T> + crd::math::log(scale);
    }
};

// ───────────────────────────── Wald / InverseGaussian(μ, λ) ─────────────────────────────
template <Real T>
struct Wald : ContinuousBase<Wald<T>, T>
{
    using value_type = T;
    T mu = static_cast<T>(1);
    T lambda = static_cast<T>(1);

    Wald() noexcept = default;
    Wald(T m, T l) noexcept : mu(m), lambda(l) {}

    [[nodiscard]] T pdf(T x) const noexcept { return x <= static_cast<T>(0) ? static_cast<T>(0) : crd::math::exp(logpdf(x)); }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        const T d = x - mu;
        return static_cast<T>(0.5) * (crd::math::log(lambda) - detail::kLn2Pi<T> - static_cast<T>(3) * crd::math::log(x)) -
               lambda * d * d / (static_cast<T>(2) * mu * mu * x);
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T s = crd::math::sqrt(lambda / x);
        const T u = x / mu;
        return detail::phi(s * (u - static_cast<T>(1))) +
               crd::math::exp(static_cast<T>(2) * lambda / mu) * detail::phi(-s * (u + static_cast<T>(1)));
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        return detail::ppf_bisect<T>([&](T x) { return cdf(x); }, p, static_cast<T>(1e-12),
                                     mu + static_cast<T>(60) * crd::math::sqrt(mu * mu * mu / lambda));
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const T nrm = static_cast<T>(standard_normal(g));
        const T y = nrm * nrm;
        const T x = mu + mu * mu * y / (static_cast<T>(2) * lambda) -
                    mu / (static_cast<T>(2) * lambda) *
                        crd::math::sqrt(static_cast<T>(4) * mu * lambda * y + mu * mu * y * y);
        const T u = static_cast<T>(next_double(g));
        return u <= mu / (mu + x) ? x : mu * mu / x;
    }
    [[nodiscard]] T mean() const noexcept { return mu; }
    [[nodiscard]] T var() const noexcept { return mu * mu * mu / lambda; }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(3) * crd::math::sqrt(mu / lambda); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(15) * mu / lambda; }
    // Wald differential entropy has no simple closed form (an E₁/Bessel-K expectation); NOT gated. Leading-order only.
    [[nodiscard]] T entropy() const noexcept
    {
        return static_cast<T>(0.5) * (static_cast<T>(1) + crd::math::log(detail::kTwoPi<T> * mu * mu * mu / lambda));
    }
};

// ───────────────────────────── VonMises(μ, κ) on the circle ─────────────────────────────
template <Real T>
struct VonMises : ContinuousBase<VonMises<T>, T>
{
    using value_type = T;
    T mu = static_cast<T>(0);
    T kappa = static_cast<T>(1);

    VonMises() noexcept = default;
    VonMises(T m, T k) noexcept : mu(m), kappa(k) {}

    [[nodiscard]] T pdf(T x) const noexcept { return crd::math::exp(logpdf(x)); }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        return kappa * crd::math::cos(x - mu) - detail::kLn2Pi<T> - crd::math::log(special::cyl_bessel_i(static_cast<T>(0), kappa));
    }
    // cdf on (μ−π, μ+π], via the Fourier series F(x) = (z + 2 Σ_k [I_k(κ)/I_0(κ)] sin(kz)/k)/(2π) + ½, z = x−μ.
    [[nodiscard]] T cdf(T x) const noexcept
    {
        T z = x - mu;
        const T i0 = special::cyl_bessel_i(static_cast<T>(0), kappa);
        T s = z;
        for (int k = 1; k <= 60; ++k)
        {
            const T ik = special::cyl_bessel_i(static_cast<T>(k), kappa) / i0;
            s += static_cast<T>(2) * ik * crd::math::sin(static_cast<T>(k) * z) / static_cast<T>(k);
            if (ik < static_cast<T>(1e-17))
            {
                break;
            }
        }
        return static_cast<T>(0.5) + s / detail::kTwoPi<T>;
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        return mu + detail::ppf_bisect<T>([&](T x) { return cdf(x); }, p, mu - detail::kPi<T>, mu + detail::kPi<T>) - mu;
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        // Best & Fisher (1979) wrapped-Cauchy acceptance.
        const T tau = static_cast<T>(1) + crd::math::sqrt(static_cast<T>(1) + static_cast<T>(4) * kappa * kappa);
        const T rho = (tau - crd::math::sqrt(static_cast<T>(2) * tau)) / (static_cast<T>(2) * kappa);
        const T rr = (static_cast<T>(1) + rho * rho) / (static_cast<T>(2) * rho);
        for (;;)
        {
            const T u1 = static_cast<T>(next_double(g));
            const T z = crd::math::cos(detail::kPi<T> * u1);
            const T f = (static_cast<T>(1) + rr * z) / (rr + z);
            const T c = kappa * (rr - f);
            const T u2 = static_cast<T>(next_double(g));
            if (c * (static_cast<T>(2) - c) - u2 > static_cast<T>(0) || crd::math::log(c / u2) + static_cast<T>(1) - c >= static_cast<T>(0))
            {
                const T u3 = static_cast<T>(next_double(g));
                const T sign = u3 - static_cast<T>(0.5) < static_cast<T>(0) ? static_cast<T>(-1) : static_cast<T>(1);
                return mu + sign * crd::math::acos(f);
            }
        }
    }
    [[nodiscard]] T mean() const noexcept { return mu; }
    [[nodiscard]] T var() const noexcept
    {
        // circular variance 1 − I_1(κ)/I_0(κ).
        return static_cast<T>(1) - special::cyl_bessel_i(static_cast<T>(1), kappa) /
                                       special::cyl_bessel_i(static_cast<T>(0), kappa);
    }
    [[nodiscard]] T skewness() const noexcept { return static_cast<T>(0); }
    [[nodiscard]] T kurtosis() const noexcept { return static_cast<T>(0); }
    [[nodiscard]] T entropy() const noexcept
    {
        const T i0 = special::cyl_bessel_i(static_cast<T>(0), kappa);
        return -kappa * special::cyl_bessel_i(static_cast<T>(1), kappa) / i0 + crd::math::log(detail::kTwoPi<T> * i0);
    }
};

// ───────────────────────────── Rice(ν, σ) ─────────────────────────────
template <Real T>
struct Rice : ContinuousBase<Rice<T>, T>
{
    using value_type = T;
    T nu = static_cast<T>(0);
    T sigma = static_cast<T>(1);

    Rice() noexcept = default;
    Rice(T n, T s) noexcept : nu(n), sigma(s) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        if (x < static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T s2 = sigma * sigma;
        return (x / s2) * crd::math::exp(-(x * x + nu * nu) / (static_cast<T>(2) * s2)) *
               special::cyl_bessel_i(static_cast<T>(0), x * nu / s2);
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x < static_cast<T>(0) ? static_cast<T>(0)
                                     : static_cast<T>(1) - special::marcum_q(static_cast<T>(1), nu / sigma, x / sigma);
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        const T hi = nu + static_cast<T>(40) * sigma;
        return detail::ppf_bisect<T>([&](T x) { return cdf(x); }, p, static_cast<T>(0), hi);
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const T x1 = sigma * static_cast<T>(standard_normal(g)) + nu;
        const T x2 = sigma * static_cast<T>(standard_normal(g));
        return crd::math::sqrt(x1 * x1 + x2 * x2);
    }
    [[nodiscard]] T mean() const noexcept
    {
        // σ √(π/2) L_{1/2}(−ν²/2σ²); gated numerically.
        const T t = -nu * nu / (static_cast<T>(2) * sigma * sigma);
        const T l = crd::math::exp(static_cast<T>(0.5) * t) *
                    ((static_cast<T>(1) - t) * special::cyl_bessel_i(static_cast<T>(0), static_cast<T>(-0.5) * t) -
                     t * special::cyl_bessel_i(static_cast<T>(1), static_cast<T>(-0.5) * t));
        return sigma * crd::math::sqrt(detail::kPi<T> / static_cast<T>(2)) * l;
    }
    [[nodiscard]] T var() const noexcept
    {
        const T m = mean();
        return static_cast<T>(2) * sigma * sigma + nu * nu - m * m;
    }
    [[nodiscard]] T skewness() const noexcept { return detail::nan<T>(); } // no simple closed form (gated where defined)
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T entropy() const noexcept { return detail::nan<T>(); }
};

} // namespace crd::hesap::stats
