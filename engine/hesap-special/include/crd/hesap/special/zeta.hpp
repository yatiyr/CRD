#pragma once

// crd-hesap-special v12-d — Hurwitz zeta ζ(s,a)=Σ_{k≥0}(a+k)^{−s} and Riemann ζ(s)=ζ(s,1), via Euler-Maclaurin
// (9 direct terms + 8 Bernoulli corrections). The Euler-Maclaurin formula IS the analytic continuation: with 8
// Bernoulli terms it is valid for Re(s) > 1−16, i.e. all s ≠ 1 of practical interest (s>1 for Zipf, and s<1 too —
// ζ(0)=−½, ζ(−1)=−1/12, ζ(½)=−1.4603545…). Gated vs scipy.special.zeta (s>1) + closed-form values (s<1) to <1e-12.

#include <crd/hesap/special/gamma.hpp>     // Real concept
#include <crd/hesap/special/poly_eval.hpp> // detail::horner_t

#include "zeta_poly.inc" // GENERATED iteration-free Riemann-zeta minimax rationals (gen_zeta_poly.py; v12-d perf)

#include <crd/math/cmath.hpp>

namespace crd::hesap::special
{
namespace detail
{
// B_{2k}/(2k)! for k = 1..8.
inline constexpr double kZetaB[] = {1.0 / 12.0,
                                    -1.0 / 720.0,
                                    1.0 / 30240.0,
                                    -1.0 / 1209600.0,
                                    1.0 / 47900160.0,
                                    -691.0 / 1307674368000.0,
                                    1.0 / 74724249600.0,
                                    -3617.0 / 10670622842880000.0};

[[nodiscard]] inline double hurwitz_zeta_impl(double s, double a) noexcept
{
    constexpr int kDirect = 9;
    double sum = 0.0;
    for (int k = 0; k < kDirect; ++k)
    {
        sum += crd::math::pow(a + k, -s);
    }
    const double t = a + kDirect;
    const double tpow = crd::math::pow(t, -s);          // t^{−s}
    sum += t * tpow / (s - 1.0) + 0.5 * tpow;     // t^{1−s}/(s−1) + ½ t^{−s}
    double rf = s;                                 // (s)_{2k−1}, starts at (s)_1 = s
    double tp = crd::math::pow(t, -(s + 1.0));           // t^{−(s+1)}
    for (int k = 1; k <= 8; ++k)
    {
        const double term = kZetaB[k - 1] * rf * tp;
        sum += term;
        if (crd::math::fabs(term) < 1e-18 * crd::math::fabs(sum))
        {
            break;
        }
        rf *= (s + 2.0 * k - 1.0) * (s + 2.0 * k); // → (s)_{2k+1}
        tp /= (t * t);                             // → t^{−(s+2k+1)}
    }
    return sum;
}
} // namespace detail

template <Real T>
[[nodiscard]] T hurwitz_zeta(T s, T a) noexcept
{
    return static_cast<T>(detail::hurwitz_zeta_impl(static_cast<double>(s), static_cast<double>(a)));
}
template <Real T>
[[nodiscard]] T riemann_zeta(T s) noexcept
{
    const double sd = static_cast<double>(s);
    // Iteration-free fast paths (v12-d perf): two Horner-rationals replace 8+ crd::math::pow Euler-Maclaurin terms.
    if (sd >= 1.8 && sd <= 8.0)
    {
        const double t = (2.0 * sd - 9.8) / 6.2; // map [1.8,8] → [−1,1]
        return static_cast<T>(detail::horner_t(detail::kZetaR1P, t) / detail::horner_t(detail::kZetaR1Q, t));
    }
    if (sd > 8.0 && sd <= 16.0)
    {
        const double t = (2.0 * sd - 24.0) / 8.0; // map [8,16] → [−1,1]
        return static_cast<T>(detail::horner_t(detail::kZetaR2P, t) / detail::horner_t(detail::kZetaR2Q, t));
    }
    return static_cast<T>(detail::hurwitz_zeta_impl(sd, 1.0)); // s<1.8, s>16, functional-equation region
}

} // namespace crd::hesap::special
