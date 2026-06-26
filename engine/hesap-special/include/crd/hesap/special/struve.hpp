#pragma once

// crd-hesap-special v12-d — Struve H_ν(x) and modified Struve L_ν(x), x ≥ 0.
//   H_ν(x) = Σ_{m≥0} (−1)^m (x/2)^{2m+ν+1} / (Γ(m+3/2)Γ(m+ν+3/2))   (alternating ⇒ cancels for large x)
//   L_ν(x) = Σ_{m≥0}        (x/2)^{2m+ν+1} / (Γ(m+3/2)Γ(m+ν+3/2))   (all-positive ⇒ accurate everywhere)
// H_ν: ascending series for x < 16; for larger x the asymptotic H_ν = Y_ν + (1/π)Σ Γ(m+½)/Γ(ν−m+½)(x/2)^{ν−2m−1}
// (DLMF 11.6.1, optimal truncation), reusing the bessjy Y_ν already in this module. Gated vs scipy.special.struve /
// modstruve to <1e-12 (H crossover x≈12–16 ~1e-10, documented). Internals f64.

#include <crd/hesap/special/bessel.hpp> // detail::bessjy (Y_ν for the H asymptotic)
#include <crd/hesap/special/gamma.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::special
{
namespace detail
{
[[nodiscard]] inline double struve_series(double nu, double x, double sign) noexcept // sign −1 → H, +1 → L
{
    const double half = 0.5 * x;
    double t = crd::math::pow(half, nu + 1.0) / (gamma(1.5) * gamma(nu + 1.5));
    double sum = t;
    const double z2 = sign * half * half;
    for (int m = 1; m <= 500; ++m)
    {
        t *= z2 / ((m + 0.5) * (m + nu + 0.5));
        sum += t;
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum))
        {
            break;
        }
    }
    return sum;
}

[[nodiscard]] inline double struve_h_impl(double nu, double x) noexcept
{
    if (x < 16.0)
    {
        return struve_series(nu, x, -1.0);
    }
    double rj = 0.0;
    double ry = 0.0;
    double rjp = 0.0;
    double ryp = 0.0;
    bessjy(x, nu, rj, ry, rjp, ryp); // Y_ν
    const double half = 0.5 * x;
    double asum = 0.0;
    double prev = 1e300;
    for (int m = 0; m <= 60; ++m)
    {
        const double g = gamma(m + 0.5) / gamma(nu - m + 0.5); // Γ(ν−m+½) pole ⇒ g=0 (term vanishes)
        const double term = g * crd::math::pow(half, nu - 2.0 * m - 1.0);
        if (crd::math::fabs(term) > prev)
        {
            break; // optimal truncation
        }
        asum += term;
        prev = crd::math::fabs(term);
    }
    return ry + asum / 3.14159265358979323846264338327950288;
}
} // namespace detail

template <Real T>
[[nodiscard]] T struve_h(T nu, T x) noexcept
{
    return static_cast<T>(detail::struve_h_impl(static_cast<double>(nu), static_cast<double>(x)));
}
template <Real T>
[[nodiscard]] T struve_l(T nu, T x) noexcept
{
    return static_cast<T>(detail::struve_series(static_cast<double>(nu), static_cast<double>(x), 1.0));
}

} // namespace crd::hesap::special
