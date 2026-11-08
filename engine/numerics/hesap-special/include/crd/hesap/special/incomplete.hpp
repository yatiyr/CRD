#pragma once

// crd-hesap-special v12-a — regularized incomplete gamma / beta + their inverses (the cdf/ppf engine).
//
//   gammainc_p(a,x)=P(a,x) · gammainc_q(a,x)=Q(a,x) · gammainc_p_inv(a,p) · gammainc_q_inv(a,q)
//   betainc(a,b,x)=I_x(a,b)                          · betainc_inv(a,b,p)
//
// P/Q: series for x < a+1 (the classical power series, DLMF 8.7.1; gammap_series keeps its Cephes/NR origin),
// continued fraction for x ≥ a+1 (DLMF 8.9.2, evaluated by the modified Lentz method, Lentz 1976 / Thompson & Barnett
// 1986). I_x: the DLMF 8.17.22 continued fraction with the x↔1−x symmetry switch (DLMF 8.17.4). The inverses:
// closed-form initial guess (A&S 26.2.22 normal deviate, then Wilson-Hilferty for gamma, A&S 26.5.22 for beta; small-a
// gamma guess cf. DiDonato & Morris 1986) + Halley refinement with a damped, interval-safe step, organised the way
// Numerical Recipes organises `invgammp`/`invbetai`. The code text of the fractions and inverses is Cerid's own,
// written from those sources. All data-dependent ⇒ scalar; these are where Cerid MATCHES scipy/MATLAB
// (cf-iteration-bound), the distribution layer adds the SIMD crush.
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

// Modified-Lentz safeguard (Lentz 1976, Thompson & Barnett 1986): a partial numerator or denominator that vanishes is
// replaced by a tiny value, so the next division stays finite.
template <Real T>
[[nodiscard]] inline T lentz_guard(T v, T tiny) noexcept
{
    return (std::abs(v) < tiny) ? tiny : v;
}

// Starting normal deviate y for upper-tail probability p, i.e. Q(y) = p (A&S 26.2.22, |error| < 3e-3). It only seeds
// the inverse iterations, so that accuracy is plenty.
template <Real T>
[[nodiscard]] T normal_deviate_guess(T p) noexcept
{
    const T tail = (p < static_cast<T>(0.5)) ? p : static_cast<T>(1) - p;
    const T t = crd::math::sqrt(static_cast<T>(-2) * crd::math::log(tail));
    const T deviate = t - (static_cast<T>(2.30753) + t * static_cast<T>(0.27061)) /
                              (static_cast<T>(1) + t * (static_cast<T>(0.99229) + t * static_cast<T>(0.04481)));
    return (p < static_cast<T>(0.5)) ? deviate : -deviate;
}

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

// Continued fraction for Q(a,x), valid x ≥ a+1 (DLMF 8.9.2):
//   Γ(a,x) = e^{-x} x^a / (x+1−a − 1·(1−a)/(x+3−a − 2·(2−a)/(x+5−a − …))),
// i.e. partial numerators j·(a−j) and partial denominators x+1−a+2j, summed by the modified Lentz method.
// `gln` = precomputed lgamma(a).
template <Real T>
[[nodiscard]] T gammaq_cf(T a, T x, T gln) noexcept
{
    const T tiny = fpmin<T>();
    const T first_den = x + static_cast<T>(1) - a;
    T big = static_cast<T>(1) / tiny;      // Lentz's C (ratio of successive numerators)
    T inv = static_cast<T>(1) / first_den; // Lentz's D, kept inverted
    T value = inv;
    for (int j = 1; j < 2000; ++j)
    {
        const T jt = static_cast<T>(j);
        const T num = jt * (a - jt);
        const T den = first_den + static_cast<T>(2 * j);
        inv = detail::lentz_guard(den + num * inv, tiny);
        big = detail::lentz_guard(den + num / big, tiny);
        inv = static_cast<T>(1) / inv;
        const T ratio = inv * big;
        value *= ratio;
        if (std::abs(ratio - static_cast<T>(1)) < kCfEps<T>)
        {
            break;
        }
    }
    return crd::math::exp(-x + a * crd::math::log(x) - gln) * value;
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

// Inverse of P(a,·): returns x with P(a,x) = p. Starting point: Wilson-Hilferty cube for a > 1 (fed by the A&S 26.2.22
// deviate), a two-piece estimate of P(a,·) for a ≤ 1 (small-x power law, then the exponential tail; cf. DiDonato &
// Morris 1986; the quadratic fit of P(a,1), 0.253 and 0.12, keeps its Numerical Recipes origin). Refinement: Halley
// steps on P(a,x) − p, with the density P'(a,x) = x^{a−1} e^{−x}/Γ(a) and the curvature ratio P''/P' = (a−1)/x − 1;
// the correction is damped when it would overshoot.
template <Real T>
[[nodiscard]] T gammainc_p_inv(T a, T p) noexcept
{
    const T tol = static_cast<T>(8) * std::numeric_limits<T>::epsilon();
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
    const T am1 = a - static_cast<T>(1);
    // ln P'(a,x) = ln_scale + am1·(ln x − ln_ref) − (x − x_ref). For a > 1 the terms are centred on the mode x = a−1,
    // which keeps the exponent small; for a ≤ 1 there is no centring (ln_ref = x_ref = 0).
    T ln_ref = static_cast<T>(0);
    T x_ref = static_cast<T>(0);
    T ln_scale = -gln;
    T x = static_cast<T>(0);
    if (a > static_cast<T>(1))
    {
        ln_ref = crd::math::log(am1);
        x_ref = am1;
        ln_scale = am1 * (ln_ref - static_cast<T>(1)) - gln;
        const T y = detail::normal_deviate_guess(p);
        const T cube_root = static_cast<T>(1) - static_cast<T>(1) / (static_cast<T>(9) * a) -
                            y / (static_cast<T>(3) * crd::math::sqrt(a));
        x = std::max(static_cast<T>(1e-3), a * crd::math::pow(cube_root, static_cast<T>(3)));
    }
    else
    {
        const T knee = static_cast<T>(1) - a * (static_cast<T>(0.253) + a * static_cast<T>(0.12)); // ≈ P(a,1)
        if (p < knee)
        {
            x = crd::math::pow(p / knee, static_cast<T>(1) / a);
        }
        else
        {
            x = static_cast<T>(1) - crd::math::log(static_cast<T>(1) - (p - knee) / (static_cast<T>(1) - knee));
        }
    }
    const T peak = crd::math::exp(ln_scale); // density at the reference point x_ref
    for (int j = 0; j < 12; ++j)
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T residual = gammainc_p(a, x, gln) - p;
        const T density = peak * crd::math::exp(am1 * (crd::math::log(x) - ln_ref) - (x - x_ref));
        const T newton = residual / density;
        const T curvature = am1 / x - static_cast<T>(1);
        const T step =
            newton / (static_cast<T>(1) - static_cast<T>(0.5) * std::min(static_cast<T>(1), newton * curvature));
        const T previous = x;
        x -= step;
        if (x <= static_cast<T>(0))
        {
            x = static_cast<T>(0.5) * previous;
        }
        if (std::abs(step) < tol * x)
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
// Continued fraction for I_x(a,b) (DLMF 8.17.22):
//   I_x(a,b) = x^a (1−x)^b / (a·B(a,b)) · 1/(1 + d_1/(1 + d_2/(1 + …))),
//   d_{2m}   =  m(b−m) x / ((a+2m−1)(a+2m)),
//   d_{2m+1} = −(a+m)(a+b+m) x / ((a+2m)(a+2m+1)),
// summed by the modified Lentz method; each pass of the loop folds in one even and one odd coefficient.
// Returns the fraction 1/(1 + d_1/(1 + …)) without the prefactor.
template <Real T>
[[nodiscard]] T betacf(T a, T b, T x) noexcept
{
    const T tol = kCfEps<T>;
    const T tiny = fpmin<T>();
    const T apb = a + b;
    const T a_next = a + static_cast<T>(1);
    const T a_prev = a - static_cast<T>(1);
    // Seed with the first convergent 1/(1 + d_1).
    T inv = lentz_guard(static_cast<T>(1) - apb * x / a_next, tiny);
    inv = static_cast<T>(1) / inv;
    T big = static_cast<T>(1);
    T value = inv;
    // One Lentz update with partial numerator `coef` over denominator 1; returns the factor applied to `value`.
    const auto fold = [&](T coef) noexcept
    {
        inv = lentz_guard(static_cast<T>(1) + coef * inv, tiny);
        big = lentz_guard(static_cast<T>(1) + coef / big, tiny);
        inv = static_cast<T>(1) / inv;
        const T ratio = inv * big;
        value *= ratio;
        return ratio;
    };
    for (int m = 1; m <= 2000; ++m)
    {
        const T mt = static_cast<T>(m);
        const T two_m = static_cast<T>(2 * m);
        fold(mt * (b - mt) * x / ((a_prev + two_m) * (a + two_m)));
        const T last = fold(-(a + mt) * (apb + mt) * x / ((a + two_m) * (a_next + two_m)));
        if (std::abs(last - static_cast<T>(1)) < tol)
        {
            break;
        }
    }
    return value;
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
// calls). Starting point: A&S 26.5.22 when a,b ≥ 1, otherwise a two-piece estimate from the end-point power laws
// of I_x. Refinement: Halley steps on I_x − p with density x^{a−1}(1−x)^{b−1}/B(a,b) and curvature ratio
// (a−1)/x − (b−1)/(1−x), damped when they would overshoot and pulled back inside (0,1).
// The loop converges to ~1e-13 in ≤ ~5 iterations — well inside the 1e-7 ppf gate.
template <Real T>
[[nodiscard]] T betainc_inv(T a, T b, T p, T lbeta_ab) noexcept
{
    const T tol = static_cast<T>(1e-12); // gate is 1e-7 on ppf; stop once relative step is below this (fewer iters)
    if (p <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    if (p >= static_cast<T>(1))
    {
        return static_cast<T>(1);
    }
    const T am1 = a - static_cast<T>(1);
    const T bm1 = b - static_cast<T>(1);
    T x = static_cast<T>(0);
    if (a >= static_cast<T>(1) && b >= static_cast<T>(1))
    {
        const T y = detail::normal_deviate_guess(p);
        const T ra = static_cast<T>(1) / (static_cast<T>(2) * a - static_cast<T>(1));
        const T rb = static_cast<T>(1) / (static_cast<T>(2) * b - static_cast<T>(1));
        const T lambda = (y * y - static_cast<T>(3)) / static_cast<T>(6);
        const T h = static_cast<T>(2) / (ra + rb);
        const T w =
            (y * crd::math::sqrt(lambda + h) / h) -
            (rb - ra) * (lambda + static_cast<T>(5) / static_cast<T>(6) - static_cast<T>(2) / (static_cast<T>(3) * h));
        x = a / (a + b * crd::math::exp(static_cast<T>(2) * w));
    }
    else
    {
        const T ab = a + b;
        const T low_mass = crd::math::exp(a * crd::math::log(a / ab)) / a;
        const T high_mass = crd::math::exp(b * crd::math::log(b / ab)) / b;
        const T total = low_mass + high_mass;
        x = (p < low_mass / total)
                ? crd::math::pow(a * total * p, static_cast<T>(1) / a)
                : static_cast<T>(1) - crd::math::pow(b * total * (static_cast<T>(1) - p), static_cast<T>(1) / b);
    }
    for (int j = 0; j < 12; ++j)
    {
        if (x <= static_cast<T>(0) || x >= static_cast<T>(1))
        {
            return x;
        }
        const T one_minus_x = static_cast<T>(1) - x;
        const T residual = betainc(a, b, x, lbeta_ab) - p;
        const T density = crd::math::exp(am1 * crd::math::log(x) + bm1 * crd::math::log(one_minus_x) - lbeta_ab);
        const T newton = residual / density;
        const T curvature = am1 / x - bm1 / one_minus_x;
        const T step =
            newton / (static_cast<T>(1) - static_cast<T>(0.5) * std::min(static_cast<T>(1), newton * curvature));
        const T previous = x;
        x -= step;
        if (x <= static_cast<T>(0))
        {
            x = static_cast<T>(0.5) * previous;
        }
        if (x >= static_cast<T>(1))
        {
            x = static_cast<T>(0.5) * (previous + static_cast<T>(1));
        }
        if (std::abs(step) < tol * x && j > 0)
        {
            break;
        }
    }
    return x;
}

// Inverse of I_·(a,b): returns x with I_x(a,b) = p.
template <Real T>
[[nodiscard]] T betainc_inv(T a, T b, T p) noexcept
{
    return betainc_inv(a, b, p, lgamma(a) + lgamma(b) - lgamma(a + b));
}

} // namespace crd::hesap::special
