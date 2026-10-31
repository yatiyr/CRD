#pragma once

// crd-hesap-special — the recurrence-free Lanczos lgamma (g = 7, n = 9) on crd::math::log. Unlike the Stirling
// form with its product recurrence (a per-lane shift count that defeats SIMD), the Lanczos sum is a fixed 8-term
// rational, branch-free, so the batch kernels vectorize it (SIMD-DISPATCH, kernels/special_kernels.cpp).
//
//   lgamma(x), x >= 0.5:  z = x - 1;  a = c0 + sum_{k=1..8} c_k / (z + k);  t = z + (g + 0.5);
//                         lgamma = ln(2 pi)/2 + (z + 0.5) ln(t) - t + ln(a)
//   x < 0.5:  reflection  lgamma(x) = ln(pi) - ln|sin(pi x)| - lgamma(1 - x), with 1 - x >= 0.5
//
// The half-plane core lives in the unit's level namespace and calls only the level's log (ADR-0134 §5; general code
// reaches it through the scalar table), so the kernel units include it. The full functions (reflection, poles, NaN,
// infinities) are out of line in general code; the kernels call them for the lanes the core does not cover. Every level
// keeps the core's operation order, its single-rounded FMAs and -ffp-contract=off, so a batch gives lanczos_lgamma's
// bits on every level and partition (the {1,4,16}-thread determinism moat). The coefficients are the canonical g = 7
// set (about 1e-15 in the right half-plane); gated against scipy at 1e-13.

#include <crd/math/transcendental.hpp>

namespace crd::hesap::special::detail
{

// ln|Gamma(x)|: +inf at the poles (0, -1, -2, ...) and at both infinities, NaN for NaN (src/lanczos.cpp).
[[nodiscard]] double lanczos_lgamma(double x) noexcept;
// Gamma(x) = exp(lgamma(x)) for x > 0, by reflection for x < 0: +-inf at +-0, NaN at the negative integers and at
// -inf, +inf at +inf, NaN for NaN (src/lanczos.cpp).
[[nodiscard]] double lanczos_tgamma(double x) noexcept;

inline namespace CRD_MATH_LEVEL_NS
{

inline constexpr double kLanczosGph = 7.5; // g + 0.5 as one constant: one add gives t on every level
inline constexpr double kHalfLn2Pi = 0.918938533204672741780329736405617639; // ln(2 pi) / 2
inline constexpr double kLanczosC[9] = {0.99999999999980993,  676.5203681218851,     -1259.1392167224028,
                                        771.32342877765313,   -176.61502916214059,   12.507343278686905,
                                        -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7};

// lgamma for finite x >= 0.5 (no reflection).
[[nodiscard]] inline double lanczos_lgamma_half(double x) noexcept
{
    const double z = x - 1.0;
    double a = kLanczosC[0];
    a += kLanczosC[1] / (z + 1.0);
    a += kLanczosC[2] / (z + 2.0);
    a += kLanczosC[3] / (z + 3.0);
    a += kLanczosC[4] / (z + 4.0);
    a += kLanczosC[5] / (z + 5.0);
    a += kLanczosC[6] / (z + 6.0);
    a += kLanczosC[7] / (z + 7.0);
    a += kLanczosC[8] / (z + 8.0);
    const double t = z + kLanczosGph;
    return kHalfLn2Pi + (z + 0.5) * crd::math::log(t) - t + crd::math::log(a);
}

// The finite half-plane the core covers (x >= 0.5, not +inf; NaN fails both).
[[nodiscard]] constexpr bool lanczos_in_half(double x) noexcept
{
    return x >= 0.5 && x <= 1.7976931348623157e308;
}

} // namespace CRD_MATH_LEVEL_NS
} // namespace crd::hesap::special::detail
