#pragma once

// crd-hesap-stats v12-j — heavy-tail / extreme-value / noncentral distributions on the Distribution<T> framework.
// GEV / GPD / Lévy have closed-form cdf/ppf; BetaPrime rides betainc; NoncentralChiSquared rides the shipped Marcum-Q
// (cdf) + cyl_bessel_i (pdf). Gated vs scipy.stats (genextreme/genpareto/levy/betaprime/ncx2). f32/f64.
// (α-stable pdf/cdf, skew-normal Owen's-T cdf, and noncentral t/F series are the v12-j tail — separate sub-slice.)

#include <crd/hesap/stats/distribution.hpp>
#include <crd/hesap/stats/samplers.hpp>

#include <crd/hesap/special/bessel.hpp>     // cyl_bessel_i (ncx2 pdf)
#include <crd/hesap/special/erf.hpp>        // erfc / erfcinv / ndtri (Lévy)
#include <crd/hesap/special/gamma.hpp>      // gamma / lgamma (GEV/GPD moments)
#include <crd/hesap/special/incomplete.hpp> // betainc (+inv) (BetaPrime)
#include <crd/hesap/special/marcum.hpp>     // marcum_q (ncx2 cdf)

#include <crd/math/cmath.hpp>

namespace crd::hesap::stats
{
namespace detail
{
// Standard-normal cdf/pdf (shared by SkewNormal).
template <Real T>
[[nodiscard]] T std_phi(T z) noexcept
{
    return static_cast<T>(0.5) * special::erfc(-z / kSqrt2<T>);
}
template <Real T>
[[nodiscard]] T std_npdf(T z) noexcept
{
    return crd::math::exp(static_cast<T>(-0.5) * z * z) / kSqrt2Pi<T>;
}

// 16-point Gauss-Legendre rule on [−1,1] (positive half; the rule is symmetric ±x with the same w). Standard
// published nodes/weights — verified through the Owen's-T → skew-normal cdf gate (a wrong node breaks it at 1e-9).
template <Real T>
inline constexpr T kGl16x[8] = {static_cast<T>(0.0950125098376374), static_cast<T>(0.2816035507792589),
                                static_cast<T>(0.4580167776572274), static_cast<T>(0.6178762444026438),
                                static_cast<T>(0.7554044083550030), static_cast<T>(0.8656312023878318),
                                static_cast<T>(0.9445750230732326), static_cast<T>(0.9894009349916499)};
template <Real T>
inline constexpr T kGl16w[8] = {static_cast<T>(0.1894506104550685), static_cast<T>(0.1826034150449236),
                                static_cast<T>(0.1691565193950025), static_cast<T>(0.1495959888165767),
                                static_cast<T>(0.1246289712555339), static_cast<T>(0.0951585116824928),
                                static_cast<T>(0.0622535239386479), static_cast<T>(0.0271524594117541)};

// Owen's T(h,a) = (1/2π) ∫₀ᵃ exp(−½h²(1+x²))/(1+x²) dx. Substitute x=tanθ ⇒ (1/2π) ∫₀^{atan a} exp(−½h²sec²θ) dθ,
// a smooth bounded integrand → fixed 16-point Gauss-Legendre (skew-normal cdf = Φ(z) − 2·T(z,α)).
template <Real T>
[[nodiscard]] T owens_t(T h, T a) noexcept
{
    if (a == static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    const T sgn = a < static_cast<T>(0) ? static_cast<T>(-1) : static_cast<T>(1);
    const T b = crd::math::atan(std::abs(a));
    const T hh = static_cast<T>(0.5) * h * h;
    // Composite over 4 panels (≈64-pt effective) — the skew-normal cdf is Φ(z)−2T(z,α), a near-cancellation in the
    // tail, so plain 16-pt (~1e-11 abs) loses too many relative digits; 4 panels push the abs error to ~1e-15.
    constexpr int kPanels = 4;
    const T pw = b / static_cast<T>(kPanels);
    const T half = static_cast<T>(0.5) * pw;
    T sum = static_cast<T>(0);
    for (int m = 0; m < kPanels; ++m)
    {
        const T c0 = static_cast<T>(m) * pw + half; // panel center
        for (int i = 0; i < 8; ++i)
        {
            const T c1 = crd::math::cos(c0 + half * kGl16x<T>[i]);
            const T c2 = crd::math::cos(c0 - half * kGl16x<T>[i]);
            sum += kGl16w<T>[i] * (crd::math::exp(-hh / (c1 * c1)) + crd::math::exp(-hh / (c2 * c2)));
        }
    }
    return sgn * half * sum / kTwoPi<T>;
}
} // namespace detail

// ───────────────────────────── GEV(ξ, μ, σ) — Generalized Extreme Value (Jenkinson-von-Mises ξ; scipy genextreme c=−ξ) ─────────────────────────────
template <Real T>
struct GEV : ContinuousBase<GEV<T>, T>
{
    using value_type = T;
    T xi = static_cast<T>(0.1); // shape (ξ>0 Fréchet, ξ<0 Weibull, ξ→0 Gumbel)
    T mu = static_cast<T>(0);
    T sigma = static_cast<T>(1);

    GEV() noexcept = default;
    GEV(T shape, T loc, T scale) noexcept : xi(shape), mu(loc), sigma(scale) {}

    [[nodiscard]] T tval(T x) const noexcept
    {
        const T z = (x - mu) / sigma;
        if (xi == static_cast<T>(0))
        {
            return crd::math::exp(-z);
        }
        return crd::math::pow(static_cast<T>(1) + xi * z, -static_cast<T>(1) / xi);
    }
    [[nodiscard]] T pdf(T x) const noexcept
    {
        const T zz = (x - mu) / sigma;
        if (xi != static_cast<T>(0) && static_cast<T>(1) + xi * zz <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T t = tval(x);
        return crd::math::pow(t, xi + static_cast<T>(1)) * crd::math::exp(-t) / sigma;
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        const T zz = (x - mu) / sigma;
        if (xi > static_cast<T>(0) && static_cast<T>(1) + xi * zz <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        if (xi < static_cast<T>(0) && static_cast<T>(1) + xi * zz <= static_cast<T>(0))
        {
            return static_cast<T>(1);
        }
        return crd::math::exp(-tval(x));
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        if (xi == static_cast<T>(0))
        {
            return mu - sigma * crd::math::log(-crd::math::log(p));
        }
        return mu + sigma * (crd::math::pow(-crd::math::log(p), -xi) - static_cast<T>(1)) / xi;
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return ppf(static_cast<T>(next_double(g)));
    }
    [[nodiscard]] T mean() const noexcept
    {
        if (xi == static_cast<T>(0))
        {
            return mu + sigma * detail::kEuler<T>;
        }
        if (xi >= static_cast<T>(1))
        {
            return std::numeric_limits<T>::infinity();
        }
        return mu + sigma * (special::gamma(static_cast<T>(1) - xi) - static_cast<T>(1)) / xi;
    }
    [[nodiscard]] T var() const noexcept
    {
        if (xi == static_cast<T>(0))
        {
            return sigma * sigma * detail::kPi<T> * detail::kPi<T> / static_cast<T>(6);
        }
        if (xi >= static_cast<T>(0.5))
        {
            return std::numeric_limits<T>::infinity();
        }
        const T g1 = special::gamma(static_cast<T>(1) - xi);
        const T g2 = special::gamma(static_cast<T>(1) - static_cast<T>(2) * xi);
        return sigma * sigma * (g2 - g1 * g1) / (xi * xi);
    }
    [[nodiscard]] T skewness() const noexcept
    {
        if (xi >= static_cast<T>(1) / static_cast<T>(3))
        {
            return detail::nan<T>();
        }
        const T g1 = special::gamma(static_cast<T>(1) - xi);
        const T g2 = special::gamma(static_cast<T>(1) - static_cast<T>(2) * xi);
        const T g3 = special::gamma(static_cast<T>(1) - static_cast<T>(3) * xi);
        return (g3 - static_cast<T>(3) * g1 * g2 + static_cast<T>(2) * g1 * g1 * g1) /
               crd::math::pow(g2 - g1 * g1, static_cast<T>(1.5));
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        if (xi >= static_cast<T>(0.25))
        {
            return detail::nan<T>();
        }
        const T g1 = special::gamma(static_cast<T>(1) - xi);
        const T g2 = special::gamma(static_cast<T>(1) - static_cast<T>(2) * xi);
        const T g3 = special::gamma(static_cast<T>(1) - static_cast<T>(3) * xi);
        const T g4 = special::gamma(static_cast<T>(1) - static_cast<T>(4) * xi);
        const T num = g4 - static_cast<T>(4) * g1 * g3 + static_cast<T>(6) * g1 * g1 * g2 -
                      static_cast<T>(3) * g1 * g1 * g1 * g1;
        const T den = g2 - g1 * g1;
        return num / (den * den) - static_cast<T>(3);
    }
    [[nodiscard]] T entropy() const noexcept
    {
        return crd::math::log(sigma) + detail::kEuler<T> * (xi + static_cast<T>(1)) + static_cast<T>(1);
    }
};

// ───────────────────────────── GPD(ξ, μ, σ) — Generalized Pareto (scipy genpareto c=ξ) ─────────────────────────────
template <Real T>
struct GPD : ContinuousBase<GPD<T>, T>
{
    using value_type = T;
    T xi = static_cast<T>(0.1);
    T mu = static_cast<T>(0);
    T sigma = static_cast<T>(1);

    GPD() noexcept = default;
    GPD(T shape, T loc, T scale) noexcept : xi(shape), mu(loc), sigma(scale) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        const T z = (x - mu) / sigma;
        if (z < static_cast<T>(0) || (xi < static_cast<T>(0) && z > -static_cast<T>(1) / xi))
        {
            return static_cast<T>(0);
        }
        if (xi == static_cast<T>(0))
        {
            return crd::math::exp(-z) / sigma;
        }
        return crd::math::pow(static_cast<T>(1) + xi * z, -static_cast<T>(1) / xi - static_cast<T>(1)) / sigma;
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        const T z = (x - mu) / sigma;
        if (z <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        if (xi == static_cast<T>(0))
        {
            return -crd::math::expm1(-z);
        }
        if (xi < static_cast<T>(0) && z >= -static_cast<T>(1) / xi)
        {
            return static_cast<T>(1);
        }
        return static_cast<T>(1) - crd::math::pow(static_cast<T>(1) + xi * z, -static_cast<T>(1) / xi);
    }
    [[nodiscard]] T sf(T x) const noexcept
    {
        const T z = (x - mu) / sigma;
        if (z <= static_cast<T>(0))
        {
            return static_cast<T>(1);
        }
        if (xi == static_cast<T>(0))
        {
            return crd::math::exp(-z);
        }
        return crd::math::pow(static_cast<T>(1) + xi * z, -static_cast<T>(1) / xi);
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        if (xi == static_cast<T>(0))
        {
            return mu - sigma * crd::math::log1p(-p);
        }
        return mu + sigma * (crd::math::pow(static_cast<T>(1) - p, -xi) - static_cast<T>(1)) / xi;
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        return ppf(static_cast<T>(next_double(g)));
    }
    [[nodiscard]] T mean() const noexcept
    {
        return xi < static_cast<T>(1) ? mu + sigma / (static_cast<T>(1) - xi) : std::numeric_limits<T>::infinity();
    }
    [[nodiscard]] T var() const noexcept
    {
        if (xi >= static_cast<T>(0.5))
        {
            return std::numeric_limits<T>::infinity();
        }
        const T o = static_cast<T>(1) - xi;
        return sigma * sigma / (o * o * (static_cast<T>(1) - static_cast<T>(2) * xi));
    }
    [[nodiscard]] T skewness() const noexcept
    {
        if (xi >= static_cast<T>(1) / static_cast<T>(3))
        {
            return detail::nan<T>();
        }
        return static_cast<T>(2) * (static_cast<T>(1) + xi) * crd::math::sqrt(static_cast<T>(1) - static_cast<T>(2) * xi) /
               (static_cast<T>(1) - static_cast<T>(3) * xi);
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        if (xi >= static_cast<T>(0.25))
        {
            return detail::nan<T>();
        }
        const T num = static_cast<T>(3) * (static_cast<T>(1) - static_cast<T>(2) * xi) *
                      (static_cast<T>(2) * xi * xi + xi + static_cast<T>(3));
        const T den = (static_cast<T>(1) - static_cast<T>(3) * xi) * (static_cast<T>(1) - static_cast<T>(4) * xi);
        return num / den - static_cast<T>(3);
    }
    [[nodiscard]] T entropy() const noexcept { return crd::math::log(sigma) + xi + static_cast<T>(1); }
};

// ───────────────────────────── Lévy(μ, σ) — stable α=½ (scipy levy) ─────────────────────────────
template <Real T>
struct Levy : ContinuousBase<Levy<T>, T>
{
    using value_type = T;
    T mu = static_cast<T>(0);
    T sigma = static_cast<T>(1);

    Levy() noexcept = default;
    Levy(T loc, T scale) noexcept : mu(loc), sigma(scale) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        const T y = x - mu;
        if (y <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        return crd::math::sqrt(sigma / detail::kTwoPi<T>) * crd::math::exp(-sigma / (static_cast<T>(2) * y)) /
               crd::math::pow(y, static_cast<T>(1.5));
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        const T y = x - mu;
        return y <= static_cast<T>(0) ? static_cast<T>(0)
                                      : special::erfc(crd::math::sqrt(sigma / (static_cast<T>(2) * y)));
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        const T u = special::erfcinv(p);
        return mu + sigma / (static_cast<T>(2) * u * u);
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const T z = static_cast<T>(standard_normal(g));
        return mu + sigma / (z * z);
    }
    [[nodiscard]] T mean() const noexcept { return std::numeric_limits<T>::infinity(); }
    [[nodiscard]] T var() const noexcept { return std::numeric_limits<T>::infinity(); }
    [[nodiscard]] T skewness() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T entropy() const noexcept
    {
        return (static_cast<T>(1) + static_cast<T>(3) * detail::kEuler<T> + crd::math::log(static_cast<T>(16) * detail::kPi<T> * sigma * sigma)) /
               static_cast<T>(2);
    }
};

// ───────────────────────────── BetaPrime(a, b) — beta of the second kind (scipy betaprime) ─────────────────────────────
template <Real T>
struct BetaPrime : ContinuousBase<BetaPrime<T>, T>
{
    using value_type = T;
    T a = static_cast<T>(2);
    T b = static_cast<T>(3);

    BetaPrime() noexcept : m_lbeta(special::lbeta(a, b)) {}
    BetaPrime(T aa, T bb) noexcept : a(aa), b(bb), m_lbeta(special::lbeta(aa, bb)) {}

    [[nodiscard]] T pdf(T x) const noexcept { return x <= static_cast<T>(0) ? static_cast<T>(0) : crd::math::exp(logpdf(x)); }
    [[nodiscard]] T logpdf(T x) const noexcept
    {
        return (a - static_cast<T>(1)) * crd::math::log(x) - (a + b) * crd::math::log1p(x) - m_lbeta;
    }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        return x <= static_cast<T>(0) ? static_cast<T>(0) : special::betainc(a, b, x / (static_cast<T>(1) + x), m_lbeta);
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        const T z = special::betainc_inv(a, b, p, m_lbeta);
        return z / (static_cast<T>(1) - z);
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const double bb = beta_dist(g, static_cast<double>(a), static_cast<double>(b));
        return static_cast<T>(bb / (1.0 - bb));
    }
    [[nodiscard]] T mean() const noexcept
    {
        return b > static_cast<T>(1) ? a / (b - static_cast<T>(1)) : std::numeric_limits<T>::infinity();
    }
    [[nodiscard]] T var() const noexcept
    {
        if (b <= static_cast<T>(2))
        {
            return std::numeric_limits<T>::infinity();
        }
        return a * (a + b - static_cast<T>(1)) / ((b - static_cast<T>(2)) * (b - static_cast<T>(1)) * (b - static_cast<T>(1)));
    }
    [[nodiscard]] T skewness() const noexcept
    {
        if (b <= static_cast<T>(3))
        {
            return detail::nan<T>();
        }
        return static_cast<T>(2) * (static_cast<T>(2) * a + b - static_cast<T>(1)) / (b - static_cast<T>(3)) *
               crd::math::sqrt((b - static_cast<T>(2)) / (a * (a + b - static_cast<T>(1))));
    }
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); } // finite only for b>4; gated via ref
    // BetaPrime differential entropy: lbeta(a,b) − (a−1)ψ(a) − (b+1)ψ(b)... wait — use the standard closed form
    // H = lbeta(a,b) + (a+b)ψ(a+b) − (a−1)ψ(a) − (b+1)ψ(b). Gated vs scipy.
    [[nodiscard]] T entropy() const noexcept
    {
        return m_lbeta + (a + b) * special::digamma(a + b) - (a - static_cast<T>(1)) * special::digamma(a) -
               (b + static_cast<T>(1)) * special::digamma(b);
    }

private:
    T m_lbeta = static_cast<T>(0);
};

// ───────────────────────────── NoncentralChiSquared(k, λ) — scipy ncx2(df=k, nc=λ) ─────────────────────────────
template <Real T>
struct NoncentralChiSquared : ContinuousBase<NoncentralChiSquared<T>, T>
{
    using value_type = T;
    T k = static_cast<T>(1);
    T lambda = static_cast<T>(1);

    NoncentralChiSquared() noexcept = default;
    NoncentralChiSquared(T df, T nc) noexcept : k(df), lambda(nc) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T h = static_cast<T>(0.5) * k - static_cast<T>(1);
        return static_cast<T>(0.5) * crd::math::exp(-static_cast<T>(0.5) * (x + lambda)) *
               crd::math::pow(x / lambda, static_cast<T>(0.5) * h) *
               special::cyl_bessel_i(h, crd::math::sqrt(lambda * x));
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        // P(X ≤ x) = 1 − Q_{k/2}(√λ, √x), Marcum's Q.
        return x <= static_cast<T>(0)
                   ? static_cast<T>(0)
                   : static_cast<T>(1) - special::marcum_q(static_cast<T>(0.5) * k, crd::math::sqrt(lambda), crd::math::sqrt(x));
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        const T hi = mean() + static_cast<T>(40) * crd::math::sqrt(var());
        return detail::ppf_bisect<T>([&](T x) { return cdf(x); }, p, static_cast<T>(0), hi);
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        // Poisson mixture: J ~ Poisson(λ/2), X ~ χ²(k + 2J).
        const crd::i64 j = poisson(g, static_cast<double>(static_cast<T>(0.5) * lambda));
        return static_cast<T>(chi_squared(g, static_cast<double>(k) + 2.0 * static_cast<double>(j)));
    }
    [[nodiscard]] T mean() const noexcept { return k + lambda; }
    [[nodiscard]] T var() const noexcept { return static_cast<T>(2) * (k + static_cast<T>(2) * lambda); }
    [[nodiscard]] T skewness() const noexcept
    {
        return crd::math::pow(static_cast<T>(2), static_cast<T>(1.5)) * (k + static_cast<T>(3) * lambda) /
               crd::math::pow(k + static_cast<T>(2) * lambda, static_cast<T>(1.5));
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        return static_cast<T>(12) * (k + static_cast<T>(4) * lambda) /
               ((k + static_cast<T>(2) * lambda) * (k + static_cast<T>(2) * lambda));
    }
    [[nodiscard]] T entropy() const noexcept { return detail::nan<T>(); } // no simple closed form
};

// ───────────────────────────── SkewNormal(α, ξ, ω) — scipy skewnorm(a, loc, scale) ─────────────────────────────
template <Real T>
struct SkewNormal : ContinuousBase<SkewNormal<T>, T>
{
    using value_type = T;
    T alpha = static_cast<T>(0); // skew (α>0 right, α<0 left, α=0 normal)
    T xi = static_cast<T>(0);    // location
    T omega = static_cast<T>(1); // scale

    SkewNormal() noexcept = default;
    SkewNormal(T a, T loc, T scale) noexcept : alpha(a), xi(loc), omega(scale) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        const T z = (x - xi) / omega;
        return static_cast<T>(2) / omega * detail::std_npdf(z) * detail::std_phi(alpha * z);
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        const T z = (x - xi) / omega;
        return detail::std_phi(z) - static_cast<T>(2) * detail::owens_t(z, alpha);
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        return detail::ppf_bisect<T>([&](T x) { return cdf(x); }, p, xi - static_cast<T>(40) * omega,
                                     xi + static_cast<T>(40) * omega);
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        // X = δ|Z0| + √(1−δ²)Z1, δ = α/√(1+α²) (Azzalini) ⇒ SkewNormal(α).
        const T d = alpha / crd::math::sqrt(static_cast<T>(1) + alpha * alpha);
        const T z0 = static_cast<T>(standard_normal(g));
        const T z1 = static_cast<T>(standard_normal(g));
        return xi + omega * (d * crd::math::fabs(z0) + crd::math::sqrt(static_cast<T>(1) - d * d) * z1);
    }
    [[nodiscard]] T delta() const noexcept { return alpha / crd::math::sqrt(static_cast<T>(1) + alpha * alpha); }
    [[nodiscard]] T mean() const noexcept
    {
        return xi + omega * delta() * crd::math::sqrt(static_cast<T>(2) / detail::kPi<T>);
    }
    [[nodiscard]] T var() const noexcept
    {
        const T d = delta();
        return omega * omega * (static_cast<T>(1) - static_cast<T>(2) * d * d / detail::kPi<T>);
    }
    [[nodiscard]] T skewness() const noexcept
    {
        const T d = delta();
        const T m = d * crd::math::sqrt(static_cast<T>(2) / detail::kPi<T>);
        const T base = static_cast<T>(1) - m * m;
        return (static_cast<T>(4) - detail::kPi<T>) / static_cast<T>(2) * m * m * m /
               crd::math::pow(base, static_cast<T>(1.5));
    }
    [[nodiscard]] T kurtosis() const noexcept
    {
        const T d = delta();
        const T m = d * crd::math::sqrt(static_cast<T>(2) / detail::kPi<T>);
        const T base = static_cast<T>(1) - m * m;
        return static_cast<T>(2) * (detail::kPi<T> - static_cast<T>(3)) * (m * m * m * m) / (base * base);
    }
    [[nodiscard]] T entropy() const noexcept { return detail::nan<T>(); } // scipy computes it numerically; not gated
};

// ───────────────────────────── NoncentralT(ν, λ) — scipy nct(df, nc) ─────────────────────────────
template <Real T>
struct NoncentralT : ContinuousBase<NoncentralT<T>, T>
{
    using value_type = T;
    T nu = static_cast<T>(1);
    T lambda = static_cast<T>(0);

    NoncentralT() noexcept = default;
    NoncentralT(T df, T nc) noexcept : nu(df), lambda(nc) {}

    [[nodiscard]] T pdf(T x) const noexcept
    {
        // f(t) = ν^{ν/2} e^{−λ²/2} / [√π Γ(ν/2)(ν+t²)^{(ν+1)/2}] · Σ_k Γ((ν+k+1)/2)/k! · (λt√2/√(ν+t²))^k.
        const T c = nu + x * x;
        const T base = lambda * x * detail::kSqrt2<T> / crd::math::sqrt(c);
        const T lpre = static_cast<T>(0.5) * nu * crd::math::log(nu) - static_cast<T>(0.5) * lambda * lambda -
                       static_cast<T>(0.5) * crd::math::log(detail::kPi<T>) - special::lgamma(static_cast<T>(0.5) * nu) -
                       static_cast<T>(0.5) * (nu + static_cast<T>(1)) * crd::math::log(c);
        const T lab = crd::math::log(std::abs(base) + std::numeric_limits<T>::min());
        T sum = static_cast<T>(0);
        T sgn = static_cast<T>(1);
        for (int k = 0; k < 200; ++k)
        {
            const T lt = special::lgamma(static_cast<T>(0.5) * (nu + static_cast<T>(k) + static_cast<T>(1))) -
                         special::lgamma(static_cast<T>(k + 1)) + static_cast<T>(k) * lab;
            const T term = sgn * crd::math::exp(lt + lpre);
            sum += term;
            if (k > static_cast<int>(std::abs(base)) + 5 && std::abs(term) < static_cast<T>(1e-18) * (std::abs(sum) + static_cast<T>(1e-300)))
            {
                break;
            }
            sgn = base < static_cast<T>(0) ? -sgn : sgn;
        }
        return sum < static_cast<T>(0) ? static_cast<T>(0) : sum;
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T cdf(T x) const noexcept
    {
        if (x < static_cast<T>(0))
        {
            return static_cast<T>(1) - cdf_pos(-x, -lambda);
        }
        return cdf_pos(x, lambda);
    }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        const T s = crd::math::sqrt(var() > static_cast<T>(0) && std::isfinite(var()) ? var() : nu);
        const T m = std::isfinite(mean()) ? mean() : lambda;
        return detail::ppf_bisect<T>([&](T t) { return cdf(t); }, p, m - static_cast<T>(60) * s - static_cast<T>(40),
                                     m + static_cast<T>(60) * s + static_cast<T>(40));
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const double z = standard_normal(g) + static_cast<double>(lambda);
        const double c = chi_squared(g, static_cast<double>(nu));
        return static_cast<T>(z / crd::math::sqrt(c / static_cast<double>(nu)));
    }
    [[nodiscard]] T mean() const noexcept
    {
        if (nu <= static_cast<T>(1))
        {
            return detail::nan<T>();
        }
        return lambda * crd::math::sqrt(static_cast<T>(0.5) * nu) *
               crd::math::exp(special::lgamma(static_cast<T>(0.5) * (nu - static_cast<T>(1))) -
                        special::lgamma(static_cast<T>(0.5) * nu));
    }
    [[nodiscard]] T var() const noexcept
    {
        if (nu <= static_cast<T>(2))
        {
            return detail::nan<T>();
        }
        const T m = mean();
        return nu * (static_cast<T>(1) + lambda * lambda) / (nu - static_cast<T>(2)) - m * m;
    }
    [[nodiscard]] T skewness() const noexcept { return detail::nan<T>(); } // gated where finite via ref
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T entropy() const noexcept { return detail::nan<T>(); }

private:
    // Lenth (AS 243): F(t) = Φ(−δ) + ½ Σ_j [p_j I_x(j+½, ν/2) + q_j I_x(j+1, ν/2)], x = t²/(t²+ν), for t ≥ 0.
    [[nodiscard]] T cdf_pos(T t, T del) const noexcept
    {
        if (t <= static_cast<T>(0))
        {
            return detail::std_phi(-del);
        }
        const T x = t * t / (t * t + nu);
        const T d2 = static_cast<T>(0.5) * del * del;
        const T edm = crd::math::exp(-d2);
        T p = edm;                                                       // p_0
        T q = del * edm * crd::math::sqrt(static_cast<T>(2) / detail::kPi<T>); // q_0
        const T hn = static_cast<T>(0.5) * nu;
        T sum = static_cast<T>(0);
        for (int j = 0; j < 1000; ++j)
        {
            const T ip = special::betainc(static_cast<T>(j) + static_cast<T>(0.5), hn, x);
            const T iq = special::betainc(static_cast<T>(j + 1), hn, x);
            sum += p * ip + q * iq;
            if (j > static_cast<int>(d2) + 5 && (p + q) < static_cast<T>(1e-17))
            {
                break;
            }
            p *= d2 / static_cast<T>(j + 1);
            q *= d2 / (static_cast<T>(j) + static_cast<T>(1.5));
        }
        return detail::std_phi(-del) + static_cast<T>(0.5) * sum;
    }
};

// ───────────────────────────── NoncentralF(d1, d2, λ) — scipy ncf(dfn, dfd, nc) ─────────────────────────────
template <Real T>
struct NoncentralF : ContinuousBase<NoncentralF<T>, T>
{
    using value_type = T;
    T d1 = static_cast<T>(1);
    T d2 = static_cast<T>(1);
    T lambda = static_cast<T>(0);

    NoncentralF() noexcept = default;
    NoncentralF(T n1, T n2, T nc) noexcept : d1(n1), d2(n2), lambda(nc) {}

    // cdf via the Poisson mixture of central betas: F(x) = Σ_j e^{−λ/2}(λ/2)^j/j! · I_y(d1/2+j, d2/2), y = d1 x/(d1 x+d2).
    [[nodiscard]] T cdf(T x) const noexcept
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T y = d1 * x / (d1 * x + d2);
        const T l2 = static_cast<T>(0.5) * lambda;
        T w = crd::math::exp(-l2);
        T sum = static_cast<T>(0);
        for (int j = 0; j < 1000; ++j)
        {
            sum += w * special::betainc(static_cast<T>(0.5) * d1 + static_cast<T>(j), static_cast<T>(0.5) * d2, y);
            if (j > static_cast<int>(l2) + 5 && w < static_cast<T>(1e-17))
            {
                break;
            }
            w *= l2 / static_cast<T>(j + 1);
        }
        return sum;
    }
    [[nodiscard]] T pdf(T x) const noexcept
    {
        // Σ_j e^{−λ/2}(λ/2)^j/j! · [central F(d1+2j, d2) density of (d1 x)/(d1+2j)] · d1/(d1+2j).
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T l2 = static_cast<T>(0.5) * lambda;
        T w = crd::math::exp(-l2);
        T sum = static_cast<T>(0);
        for (int j = 0; j < 1000; ++j)
        {
            const T a = static_cast<T>(0.5) * d1 + static_cast<T>(j);
            const T b = static_cast<T>(0.5) * d2;
            const T y = d1 * x / (d1 * x + d2);
            const T betadens =
                crd::math::exp((a - static_cast<T>(1)) * crd::math::log(y) + (b - static_cast<T>(1)) * crd::math::log1p(-y) -
                         special::lbeta(a, b));
            const T dydx = d1 * d2 / ((d1 * x + d2) * (d1 * x + d2));
            sum += w * betadens * dydx;
            if (j > static_cast<int>(l2) + 5 && w < static_cast<T>(1e-17))
            {
                break;
            }
            w *= l2 / static_cast<T>(j + 1);
        }
        return sum;
    }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(pdf(x)); }
    [[nodiscard]] T ppf(T p) const noexcept
    {
        const T hi = mean() + static_cast<T>(60) * crd::math::sqrt(std::isfinite(var()) ? var() : (mean() + static_cast<T>(1)));
        return detail::ppf_bisect<T>([&](T x) { return cdf(x); }, p, static_cast<T>(0), hi + static_cast<T>(50));
    }
    template <BitGenerator G>
    [[nodiscard]] T rvs(G& g) const noexcept
    {
        const double num = static_cast<double>(NoncentralChiSquared<T>(d1, lambda).rvs(g)) / static_cast<double>(d1);
        const double den = chi_squared(g, static_cast<double>(d2)) / static_cast<double>(d2);
        return static_cast<T>(num / den);
    }
    [[nodiscard]] T mean() const noexcept
    {
        return d2 > static_cast<T>(2) ? d2 * (d1 + lambda) / (d1 * (d2 - static_cast<T>(2)))
                                      : std::numeric_limits<T>::infinity();
    }
    [[nodiscard]] T var() const noexcept
    {
        if (d2 <= static_cast<T>(4))
        {
            return std::numeric_limits<T>::infinity();
        }
        const T num = (d1 + lambda) * (d1 + lambda) + (d1 + static_cast<T>(2) * lambda) * (d2 - static_cast<T>(2));
        const T den = (d2 - static_cast<T>(2)) * (d2 - static_cast<T>(2)) * (d2 - static_cast<T>(4));
        return static_cast<T>(2) * (d2 / d1) * (d2 / d1) * num / den;
    }
    [[nodiscard]] T skewness() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T kurtosis() const noexcept { return detail::nan<T>(); }
    [[nodiscard]] T entropy() const noexcept { return detail::nan<T>(); }
};

// ───────────────────────────── α-stable sampler — Chambers-Mallows-Stuck (Nolan S1 parameterization) ─────────────────────────────
// The stable family has no closed-form pdf/cdf for general α (scipy computes them by numerical integration), so this
// is a SAMPLER, not a Distribution<T>: S(α, β; loc, scale). Special cases: α=2 → N(loc, √2·scale); α=1,β=0 → Cauchy;
// α=½,β=1 → Lévy. Pure (seed)-deterministic ⇒ the {1,4,16} moat. Gated by those special cases + determinism.
template <Real T>
class StableSampler
{
public:
    StableSampler(T alpha, T beta, T loc = static_cast<T>(0), T scale = static_cast<T>(1)) noexcept
        : m_alpha(alpha), m_beta(beta), m_loc(loc), m_scale(scale)
    {
    }

    template <BitGenerator G>
    [[nodiscard]] T sample(G& g) const noexcept
    {
        // U ~ Uniform(−π/2, π/2), W ~ Exp(1).
        const T u = detail::kPi<T> * (static_cast<T>(next_double(g)) - static_cast<T>(0.5));
        const T w = static_cast<T>(standard_exponential(g));
        T x;
        if (std::abs(m_alpha - static_cast<T>(1)) < static_cast<T>(1e-8))
        {
            const T hp = static_cast<T>(0.5) * detail::kPi<T>;
            x = (static_cast<T>(2) / detail::kPi<T>) *
                ((hp + m_beta * u) * crd::math::tan(u) -
                 m_beta * crd::math::log((hp * w * crd::math::cos(u)) / (hp + m_beta * u)));
            return m_loc + m_scale * x +
                   m_beta * (static_cast<T>(2) / detail::kPi<T>) * m_scale * crd::math::log(m_scale);
        }
        const T zeta = -m_beta * crd::math::tan(static_cast<T>(0.5) * detail::kPi<T> * m_alpha);
        const T xi = crd::math::atan(-zeta) / m_alpha;
        const T c = crd::math::pow(static_cast<T>(1) + zeta * zeta, static_cast<T>(0.5) / m_alpha);
        x = c * crd::math::sin(m_alpha * (u + xi)) / crd::math::pow(crd::math::cos(u), static_cast<T>(1) / m_alpha) *
            crd::math::pow(crd::math::cos(u - m_alpha * (u + xi)) / w, (static_cast<T>(1) - m_alpha) / m_alpha);
        return m_loc + m_scale * x;
    }

    [[nodiscard]] T alpha() const noexcept { return m_alpha; }
    [[nodiscard]] T beta() const noexcept { return m_beta; }

private:
    T m_alpha;
    T m_beta;
    T m_loc;
    T m_scale;
};

} // namespace crd::hesap::stats
