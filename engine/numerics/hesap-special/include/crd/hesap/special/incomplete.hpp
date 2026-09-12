#pragma once

// crd-hesap-special v12-a — regularized incomplete gamma / beta + their inverses (the cdf/ppf engine).
//
//   gammainc_p(a,x)=P(a,x) · gammainc_q(a,x)=Q(a,x) · gammainc_p_inv(a,p) · gammainc_q_inv(a,q)
//   betainc(a,b,x)=I_x(a,b)                          · betainc_inv(a,b,p)
//
// P/Q: series for x < a+1, modified-Lentz continued fraction for x ≥ a+1 (Cephes/NR). I_x: the NR `betacf`
// continued fraction with the x↔1−x symmetry switch. The inverses: closed-form initial guess (Cornish-Fisher /
// Acton) + Halley refinement with a bisection-safe step (NR `invgammp`/`invbetai`). All data-dependent ⇒ scalar;
// these are where Cerid MATCHES scipy/MATLAB (cf-iteration-bound), the distribution layer adds the SIMD crush.
// Gate ≤ 1e-12 vs scipy.special.{gammainc,gammaincinv,betainc,betaincinv} + Boost.

#include <crd/hesap/special/gamma.hpp>

#include <algorithm>
#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::special
{
namespace detail
{
template <Real T>
inline T fpmin() noexcept
{
    return std::numeric_limits<T>::min() / std::numeric_limits<T>::epsilon();
}

// Convergence tolerance for the series/CFs. The accuracy gate is 1e-12; running to full machine-ε (2.2e-16) buys
// ~3 extra iterations for ulps the gate doesn't need. 8·ε ≈ 1.8e-15 keeps ≥13 honest digits and trims the tail.
template <Real T>
inline constexpr T kCfEps = static_cast<T>(8) * std::numeric_limits<T>::epsilon();

// Σ-series for P(a,x), valid x < a+1. `gln` = precomputed lgamma(a) (amortised by fixed-shape distributions).
template <Real T>
[[nodiscard]] T gammap_series(T a, T x, T gln) noexcept
{
    T ap = a;
    T del = static_cast<T>(1) / a;
    T sum = del;
    for (int n = 0; n < 2000; ++n)
    {
        ap += static_cast<T>(1);
        del *= x / ap;
        sum += del;
        if (std::abs(del) < std::abs(sum) * kCfEps<T>)
        {
            break;
        }
    }
    return sum * crd::math::exp(-x + a * crd::math::log(x) - gln);
}

// Modified-Lentz continued fraction for Q(a,x), valid x ≥ a+1. `gln` = precomputed lgamma(a).
template <Real T>
[[nodiscard]] T gammaq_cf(T a, T x, T gln) noexcept
{
    const T tiny = fpmin<T>();
    T b = x + static_cast<T>(1) - a;
    T c = static_cast<T>(1) / tiny;
    T d = static_cast<T>(1) / b;
    T h = d;
    for (int i = 1; i < 2000; ++i)
    {
        const T an = -static_cast<T>(i) * (static_cast<T>(i) - a);
        b += static_cast<T>(2);
        d = an * d + b;
        if (std::abs(d) < tiny)
        {
            d = tiny;
        }
        c = b + an / c;
        if (std::abs(c) < tiny)
        {
            c = tiny;
        }
        d = static_cast<T>(1) / d;
        const T del = d * c;
        h *= del;
        if (std::abs(del - static_cast<T>(1)) < kCfEps<T>)
        {
            break;
        }
    }
    return crd::math::exp(-x + a * crd::math::log(x) - gln) * h;
}
} // namespace detail

// Regularized lower incomplete gamma P(a,x) with PRECOMPUTED gln=lgamma(a) (fixed-shape distributions amortise it).
template <Real T>
[[nodiscard]] T gammainc_p(T a, T x, T gln) noexcept
{
    if (x <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    return (x < a + static_cast<T>(1)) ? detail::gammap_series(a, x, gln) : static_cast<T>(1) - detail::gammaq_cf(a, x, gln);
}

// Regularized upper incomplete gamma Q(a,x) with PRECOMPUTED gln=lgamma(a).
template <Real T>
[[nodiscard]] T gammainc_q(T a, T x, T gln) noexcept
{
    if (x <= static_cast<T>(0))
    {
        return static_cast<T>(1);
    }
    return (x < a + static_cast<T>(1)) ? static_cast<T>(1) - detail::gammap_series(a, x, gln) : detail::gammaq_cf(a, x, gln);
}

// Regularized lower incomplete gamma P(a,x) = γ(a,x)/Γ(a).
template <Real T>
[[nodiscard]] T gammainc_p(T a, T x) noexcept
{
    if (std::isnan(a) || std::isnan(x) || x < static_cast<T>(0) || a <= static_cast<T>(0))
    {
        return std::numeric_limits<T>::quiet_NaN();
    }
    if (x == static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    return gammainc_p(a, x, lgamma(a));
}

// Regularized upper incomplete gamma Q(a,x) = Γ(a,x)/Γ(a).
template <Real T>
[[nodiscard]] T gammainc_q(T a, T x) noexcept
{
    if (std::isnan(a) || std::isnan(x) || x < static_cast<T>(0) || a <= static_cast<T>(0))
    {
        return std::numeric_limits<T>::quiet_NaN();
    }
    if (x == static_cast<T>(0))
    {
        return static_cast<T>(1);
    }
    return gammainc_q(a, x, lgamma(a));
}

// Inverse of P(a,·): returns x with P(a,x) = p.  (NR `invgammp`.)
template <Real T>
[[nodiscard]] T gammainc_p_inv(T a, T p) noexcept
{
    const T eps = static_cast<T>(8) * std::numeric_limits<T>::epsilon();
    if (a <= static_cast<T>(0))
    {
        return std::numeric_limits<T>::quiet_NaN();
    }
    if (p <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    if (p >= static_cast<T>(1))
    {
        return std::numeric_limits<T>::infinity();
    }
    const T gln = lgamma(a);
    const T a1 = a - static_cast<T>(1);
    T x = static_cast<T>(0);
    T lna1 = static_cast<T>(0);
    T afac = static_cast<T>(0);
    if (a > static_cast<T>(1))
    {
        lna1 = crd::math::log(a1);
        afac = crd::math::exp(a1 * (lna1 - static_cast<T>(1)) - gln);
        const T pp = (p < static_cast<T>(0.5)) ? p : static_cast<T>(1) - p;
        const T t = crd::math::sqrt(static_cast<T>(-2) * crd::math::log(pp));
        T xx = (static_cast<T>(2.30753) + t * static_cast<T>(0.27061)) /
                   (static_cast<T>(1) + t * (static_cast<T>(0.99229) + t * static_cast<T>(0.04481))) -
               t;
        if (p < static_cast<T>(0.5))
        {
            xx = -xx;
        }
        x = std::max(static_cast<T>(1e-3),
                     a * crd::math::pow(static_cast<T>(1) - static_cast<T>(1) / (static_cast<T>(9) * a) -
                                      xx / (static_cast<T>(3) * crd::math::sqrt(a)),
                                  static_cast<T>(3)));
    }
    else
    {
        const T t = static_cast<T>(1) - a * (static_cast<T>(0.253) + a * static_cast<T>(0.12));
        if (p < t)
        {
            x = crd::math::pow(p / t, static_cast<T>(1) / a);
        }
        else
        {
            x = static_cast<T>(1) - crd::math::log(static_cast<T>(1) - (p - t) / (static_cast<T>(1) - t));
        }
    }
    for (int j = 0; j < 12; ++j)
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T err = gammainc_p(a, x, gln) - p;
        T t = (a > static_cast<T>(1))
                  ? afac * crd::math::exp(-(x - a1) + a1 * (crd::math::log(x) - lna1))
                  : crd::math::exp(-x + a1 * crd::math::log(x) - gln);
        const T u = err / t;
        t = u / (static_cast<T>(1) -
                 static_cast<T>(0.5) * std::min(static_cast<T>(1), u * (a1 / x - static_cast<T>(1))));
        x -= t;
        if (x <= static_cast<T>(0))
        {
            x = static_cast<T>(0.5) * (x + t);
        }
        if (std::abs(t) < eps * x)
        {
            break;
        }
    }
    return x;
}

// Inverse of Q(a,·): returns x with Q(a,x) = q.
template <Real T>
[[nodiscard]] T gammainc_q_inv(T a, T q) noexcept
{
    return gammainc_p_inv(a, static_cast<T>(1) - q);
}

namespace detail
{
// NR `betacf` — continued fraction for I_x(a,b).
template <Real T>
[[nodiscard]] T betacf(T a, T b, T x) noexcept
{
    const T eps = kCfEps<T>;
    const T tiny = fpmin<T>();
    const T qab = a + b;
    const T qap = a + static_cast<T>(1);
    const T qam = a - static_cast<T>(1);
    T c = static_cast<T>(1);
    T d = static_cast<T>(1) - qab * x / qap;
    if (std::abs(d) < tiny)
    {
        d = tiny;
    }
    d = static_cast<T>(1) / d;
    T h = d;
    for (int m = 1; m <= 2000; ++m)
    {
        const T m2 = static_cast<T>(2 * m);
        T aa = static_cast<T>(m) * (b - static_cast<T>(m)) * x / ((qam + m2) * (a + m2));
        d = static_cast<T>(1) + aa * d;
        if (std::abs(d) < tiny)
        {
            d = tiny;
        }
        c = static_cast<T>(1) + aa / c;
        if (std::abs(c) < tiny)
        {
            c = tiny;
        }
        d = static_cast<T>(1) / d;
        h *= d * c;
        aa = -(a + static_cast<T>(m)) * (qab + static_cast<T>(m)) * x / ((a + m2) * (qap + m2));
        d = static_cast<T>(1) + aa * d;
        if (std::abs(d) < tiny)
        {
            d = tiny;
        }
        c = static_cast<T>(1) + aa / c;
        if (std::abs(c) < tiny)
        {
            c = tiny;
        }
        d = static_cast<T>(1) / d;
        const T del = d * c;
        h *= del;
        if (std::abs(del - static_cast<T>(1)) < eps)
        {
            break;
        }
    }
    return h;
}
} // namespace detail

// Regularized incomplete beta I_x(a,b) with PRECOMPUTED lbeta = lgamma(a)+lgamma(b)−lgamma(a+b). Fixed-(a,b)
// distributions amortise the 3 lgamma calls (~20 ns) across an evaluated array via this overload.
template <Real T>
[[nodiscard]] T betainc(T a, T b, T x, T lbeta_ab) noexcept
{
    if (x <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    if (x >= static_cast<T>(1))
    {
        return static_cast<T>(1);
    }
    const T bt = crd::math::exp(-lbeta_ab + a * crd::math::log(x) + b * crd::math::log1p(-x));
    if (x < (a + static_cast<T>(1)) / (a + b + static_cast<T>(2)))
    {
        return bt * detail::betacf(a, b, x) / a;
    }
    return static_cast<T>(1) - bt * detail::betacf(b, a, static_cast<T>(1) - x) / b;
}

// Regularized incomplete beta I_x(a,b).
template <Real T>
[[nodiscard]] T betainc(T a, T b, T x) noexcept
{
    if (std::isnan(a) || std::isnan(b) || std::isnan(x))
    {
        return std::numeric_limits<T>::quiet_NaN();
    }
    return betainc(a, b, x, lgamma(a) + lgamma(b) - lgamma(a + b));
}

// Inverse of I_·(a,b): returns x with I_x(a,b) = p, with PRECOMPUTED lbeta (amortises the inner Newton betainc
// calls). The Halley loop converges to ~1e-13 in ≤ ~5 iterations — well inside the 1e-7 ppf gate.
template <Real T>
[[nodiscard]] T betainc_inv(T a, T b, T p, T lbeta_ab) noexcept
{
    const T eps = static_cast<T>(1e-12); // gate is 1e-7 on ppf; stop once relative step is below this (fewer iters)
    if (p <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    if (p >= static_cast<T>(1))
    {
        return static_cast<T>(1);
    }
    const T a1 = a - static_cast<T>(1);
    const T b1 = b - static_cast<T>(1);
    T x = static_cast<T>(0);
    if (a >= static_cast<T>(1) && b >= static_cast<T>(1))
    {
        const T pp = (p < static_cast<T>(0.5)) ? p : static_cast<T>(1) - p;
        const T t = crd::math::sqrt(static_cast<T>(-2) * crd::math::log(pp));
        T xx = (static_cast<T>(2.30753) + t * static_cast<T>(0.27061)) /
                   (static_cast<T>(1) + t * (static_cast<T>(0.99229) + t * static_cast<T>(0.04481))) -
               t;
        if (p < static_cast<T>(0.5))
        {
            xx = -xx;
        }
        const T al = (xx * xx - static_cast<T>(3)) / static_cast<T>(6);
        const T h = static_cast<T>(2) / (static_cast<T>(1) / (static_cast<T>(2) * a - static_cast<T>(1)) +
                                         static_cast<T>(1) / (static_cast<T>(2) * b - static_cast<T>(1)));
        const T w = (xx * crd::math::sqrt(al + h) / h) -
                    (static_cast<T>(1) / (static_cast<T>(2) * b - static_cast<T>(1)) -
                     static_cast<T>(1) / (static_cast<T>(2) * a - static_cast<T>(1))) *
                        (al + static_cast<T>(5) / static_cast<T>(6) - static_cast<T>(2) / (static_cast<T>(3) * h));
        x = a / (a + b * crd::math::exp(static_cast<T>(2) * w));
    }
    else
    {
        const T lna = crd::math::log(a / (a + b));
        const T lnb = crd::math::log(b / (a + b));
        const T t = crd::math::exp(a * lna) / a;
        const T u = crd::math::exp(b * lnb) / b;
        const T w = t + u;
        x = (p < t / w) ? crd::math::pow(a * w * p, static_cast<T>(1) / a)
                        : static_cast<T>(1) - crd::math::pow(b * w * (static_cast<T>(1) - p), static_cast<T>(1) / b);
    }
    const T afac = -lbeta_ab;
    for (int j = 0; j < 12; ++j)
    {
        if (x <= static_cast<T>(0) || x >= static_cast<T>(1))
        {
            return x;
        }
        const T err = betainc(a, b, x, lbeta_ab) - p;
        T t = crd::math::exp(a1 * crd::math::log(x) + b1 * crd::math::log(static_cast<T>(1) - x) + afac);
        const T u = err / t;
        t = u / (static_cast<T>(1) -
                 static_cast<T>(0.5) *
                     std::min(static_cast<T>(1), u * (a1 / x - b1 / (static_cast<T>(1) - x))));
        x -= t;
        if (x <= static_cast<T>(0))
        {
            x = static_cast<T>(0.5) * (x + t);
        }
        if (x >= static_cast<T>(1))
        {
            x = static_cast<T>(0.5) * (x + t + static_cast<T>(1));
        }
        if (std::abs(t) < eps * x && j > 0)
        {
            break;
        }
    }
    return x;
}

// Inverse of I_·(a,b): returns x with I_x(a,b) = p.  (NR `invbetai`.)
template <Real T>
[[nodiscard]] T betainc_inv(T a, T b, T p) noexcept
{
    return betainc_inv(a, b, p, lgamma(a) + lgamma(b) - lgamma(a + b));
}

} // namespace crd::hesap::special
