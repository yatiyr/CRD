#pragma once

// crd-hesap-stats v12-h — the Distribution<T> framework (D(stat)-4). Every distribution is a small value type holding
// its parameters and exposing the SAME surface — pdf/logpdf/cdf/logcdf/sf/logsf/ppf/isf/rvs + mean/var/std/skewness/
// kurtosis/median/entropy (+ mgf/fit where they exist) — through a C++20 concept, the rv_continuous/rv_discrete twin.
// Templated on the scalar (f32/f64). CDFs/PPFs ride the already-shipped hesap-special incomplete-gamma/beta/erf
// inverses (SANITY rule 8 — reuse), rvs rides the v12-f samplers, and log-space methods are first-class (D(stat)-2).
//
// A CRTP base supplies the mechanical fallbacks (std=√var, median=ppf(½), isf=ppf(1−q), logcdf/logsf, sf=1−cdf) so a
// distribution only writes what is distribution-specific; it overrides any fallback where a direct formula is more
// accurate in the tail (e.g. Normal::sf via erfc instead of 1−cdf).

#include <crd/hesap/stats/bitgen.hpp>

#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>
#include <limits>
#include <type_traits>

namespace crd::hesap::stats
{
template <typename T>
concept Real = std::is_floating_point_v<T>;

// The deterministic surface every continuous distribution satisfies (rvs is a generator-template member, checked
// separately at the call site). pdf/cdf/sf/ppf + the four central moments + entropy.
template <typename D>
concept ContinuousDistribution = Real<typename D::value_type> &&
    requires(const D d, typename D::value_type x) {
        typename D::value_type;
        { d.pdf(x) } -> std::same_as<typename D::value_type>;
        { d.logpdf(x) } -> std::same_as<typename D::value_type>;
        { d.cdf(x) } -> std::same_as<typename D::value_type>;
        { d.sf(x) } -> std::same_as<typename D::value_type>;
        { d.ppf(x) } -> std::same_as<typename D::value_type>;
        { d.mean() } -> std::same_as<typename D::value_type>;
        { d.var() } -> std::same_as<typename D::value_type>;
        { d.skewness() } -> std::same_as<typename D::value_type>;
        { d.kurtosis() } -> std::same_as<typename D::value_type>;
        { d.entropy() } -> std::same_as<typename D::value_type>;
    };

// The deterministic surface every discrete distribution satisfies (pmf over integers; ppf returns an integer
// quantile). rvs is checked at the call site.
template <typename D>
concept DiscreteDistribution = Real<typename D::value_type> &&
    requires(const D d, crd::i64 k, typename D::value_type p) {
        typename D::value_type;
        { d.pmf(k) } -> std::same_as<typename D::value_type>;
        { d.logpmf(k) } -> std::same_as<typename D::value_type>;
        { d.cdf(k) } -> std::same_as<typename D::value_type>;
        { d.sf(k) } -> std::same_as<typename D::value_type>;
        { d.ppf(p) } -> std::same_as<crd::i64>;
        { d.mean() } -> std::same_as<typename D::value_type>;
        { d.var() } -> std::same_as<typename D::value_type>;
        { d.entropy() } -> std::same_as<typename D::value_type>;
    };

namespace detail
{
template <Real T>
inline constexpr T kPi = static_cast<T>(3.14159265358979323846264338327950288);
template <Real T>
inline constexpr T kTwoPi = static_cast<T>(6.28318530717958647692528676655900577);
template <Real T>
inline constexpr T kInvPi = static_cast<T>(0.31830988618379067153776752674502872); // 1/π (multiply, not divide)
template <Real T>
inline constexpr T kSqrt2 = static_cast<T>(1.41421356237309504880168872420969808);
template <Real T>
inline constexpr T kSqrt2Pi = static_cast<T>(2.50662827463100050241576528481104525);
template <Real T>
inline constexpr T kLn2Pi = static_cast<T>(1.83787706640934548356065947281123527);
template <Real T>
inline constexpr T kLn2 = static_cast<T>(0.69314718055994530941723212145817657);
template <Real T>
inline constexpr T kEuler = static_cast<T>(0.57721566490153286060651209008240243); // Euler-Mascheroni γ
template <Real T>
inline constexpr T kApery = static_cast<T>(1.20205690315959428539973816151144999); // ζ(3)

template <Real T>
inline constexpr T nan() noexcept
{
    return std::numeric_limits<T>::quiet_NaN();
}

// Generic ppf via bracketed bisection on a monotone CDF — for distributions with no closed-form / special-fn inverse
// (von Mises, Rice). Deterministic (fixed iteration count) ⇒ moat-safe. The bracket must straddle the root.
template <Real T, class Cdf>
[[nodiscard]] T ppf_bisect(Cdf cdf, T p, T lo, T hi) noexcept
{
    for (int i = 0; i < 100; ++i)
    {
        const T mid = static_cast<T>(0.5) * (lo + hi);
        if (cdf(mid) < p)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    return static_cast<T>(0.5) * (lo + hi);
}
} // namespace detail

// CRTP base — mechanical fallbacks shared by every continuous distribution. Override any of these in a derived
// distribution when a direct formula is more accurate (the derived member hides the base one for value-typed calls).
template <class D, Real T>
struct ContinuousBase
{
    using value_type = T;

    [[nodiscard]] T std() const noexcept { return crd::math::sqrt(self().var()); }
    [[nodiscard]] T median() const noexcept { return self().ppf(static_cast<T>(0.5)); }
    [[nodiscard]] T isf(T q) const noexcept { return self().ppf(static_cast<T>(1) - q); }
    [[nodiscard]] T sf(T x) const noexcept { return static_cast<T>(1) - self().cdf(x); }
    [[nodiscard]] T logpdf(T x) const noexcept { return crd::math::log(self().pdf(x)); }
    [[nodiscard]] T logcdf(T x) const noexcept { return crd::math::log(self().cdf(x)); }
    [[nodiscard]] T logsf(T x) const noexcept { return crd::math::log(self().sf(x)); }

protected:
    [[nodiscard]] const D& self() const noexcept { return static_cast<const D&>(*this); }
};

// CRTP base for discrete distributions. Generic fallbacks: sf=1−cdf, logpmf=log(pmf), std=√var, and a generic
// integer ppf = smallest k≥0 with cdf(k) ≥ p (bracket-double then bisect; valid for non-negative support — a
// distribution with offset/two-sided support overrides ppf).
template <class D, Real T>
struct DiscreteBase
{
    using value_type = T;

    [[nodiscard]] T sf(crd::i64 k) const noexcept { return static_cast<T>(1) - self().cdf(k); }
    [[nodiscard]] T logpmf(crd::i64 k) const noexcept { return crd::math::log(self().pmf(k)); }
    [[nodiscard]] T std() const noexcept { return crd::math::sqrt(self().var()); }
    [[nodiscard]] crd::i64 ppf(T p) const noexcept
    {
        if (p <= static_cast<T>(0))
        {
            return 0;
        }
        // Absorb the cdf's ~ulp rounding so a quantile that lands EXACTLY on a cdf jump (p == cdf(k) in reals)
        // resolves to the lower k — the mathematical "smallest k with cdf(k) ≥ p" (else a 0.9-1e-16 < 0.9 flips it).
        const T thr = p - (p * static_cast<T>(1e-12) + static_cast<T>(1e-15));
        crd::i64 lo = 0;
        crd::i64 hi = 1;
        while (self().cdf(hi) < thr)
        {
            lo = hi;
            hi *= 2;
            if (hi > (crd::i64{1} << 42))
            {
                break;
            }
        }
        while (hi - lo > 1)
        {
            const crd::i64 mid = lo + (hi - lo) / 2;
            if (self().cdf(mid) < thr)
            {
                lo = mid;
            }
            else
            {
                hi = mid;
            }
        }
        return self().cdf(lo) >= thr ? lo : hi;
    }

protected:
    [[nodiscard]] const D& self() const noexcept { return static_cast<const D&>(*this); }
};

} // namespace crd::hesap::stats
