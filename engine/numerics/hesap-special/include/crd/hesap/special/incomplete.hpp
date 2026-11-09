#pragma once

// crd-hesap-special v12-a — regularized incomplete gamma / beta + their inverses (the cdf/ppf engine).
//
//   gammainc_p(a,x)=P(a,x) · gammainc_q(a,x)=Q(a,x) · gammainc_p_inv(a,p) · gammainc_q_inv(a,q)
//   betainc(a,b,x)=I_x(a,b)                          · betainc_inv(a,b,p)
//
// P/Q: series for x < a+1 (the classical power series, DLMF 8.7.1; gammap_series_sum keeps its Cephes/NR origin),
// continued fraction for x ≥ a+1 (DLMF 8.9.2, evaluated by the modified Lentz method, Lentz 1976 / Thompson & Barnett
// 1986). I_x: the DLMF 8.17.22 continued fraction with the x↔1−x symmetry switch (DLMF 8.17.4). The inverses:
// closed-form initial guess (A&S 26.2.22 normal deviate, then Wilson-Hilferty for gamma, A&S 26.5.22 for beta; small-a
// gamma guess cf. DiDonato & Morris 1986) + Halley refinement with a damped, interval-safe step, organised the way
// Numerical Recipes organises `invgammp`/`invbetai`. The code text of the fractions and inverses is Cerid's own,
// written from those sources. All data-dependent ⇒ scalar; these are where Cerid MATCHES scipy/MATLAB
// (cf-iteration-bound), the distribution layer adds the SIMD crush.
// Large shapes: the prefactors x^a e^{−x}/Γ(a) and x^a(1−x)^b/B(a,b) switch to Stirling forms (DLMF 5.11.3; DiDonato &
// Morris 1986, 1992) so the cancelling log-gamma terms never form. From a ≥ 1e3 (kGammaUniformShape; for the beta from
// a+b ≥ kBetaUniformSum with both shapes ≥ kBetaUniformMin) the series and fractions give way, near the mode, to the
// uniform expansion in the error-function variable
// (Temme 1979, DLMF 8.12; Temme 1982, DLMF 8.18(ii)), written from Laplace's method and valid to any shape. Tails: the
// inverses iterate on the tail whose probability is exact (1−p, q), on ln Q in the far upper tail and on ln P in the
// far lower tail of the gamma and on ln I in the far tails of a beta with both shapes above 1 (Newton on a concave
// function, so it cannot diverge), and keep a bracket of the root for the beta. A single-precision inverse of a shape
// below 1/4 is evaluated in double.
// Gate ≤ 1e-12 vs scipy.special.{gammainc,gammaincinv,betainc,betaincinv} + Boost (f64).

#include <crd/core/platform.hpp>
#include <crd/hesap/special/gamma.hpp>
#include <crd/math/cmath.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
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

// The gamma shape from which the Stirling-form prefactor (Stirling's formula, DLMF 5.11.3, whose correction series is
// detail::stirling_correction of gamma.hpp, exact from kStirlingMin on) replaces the plain exp(-x + a·ln x − ln Γ(a)):
// there the three terms of that exponent are large and cancel, and their rounding (a few ulps of a·ln a) exceeds the
// accuracy gate.
template <Real T>
inline constexpr T kGammaShapeBig = static_cast<T>(sizeof(T) > sizeof(float) ? 400 : 100);
// Bounds on the terms of a series or fraction. Near x = a the gamma expansions need about 8.5·sqrt(a) terms to reach
// machine precision (and the beta fraction a similar O(sqrt(max(a,b)))), so kMaxTerms sets the largest shape that is
// summed to full accuracy. Below the shapes where the Stirling forms begin, kBaseTerms is ample. A NaN never converges
// and fails every test of the long runs, which therefore end at their first step: the work on a NaN argument stays at
// kBaseTerms.
inline constexpr int kBaseTerms = 2000;
inline constexpr int kMaxTerms = 100000;

// The shape from which the beta fraction is allowed its long run (the slow convergence is genuine only there).
template <Real T>
inline constexpr T kBetaShapeBig = static_cast<T>(sizeof(T) > sizeof(float) ? 128 : 64);
// The plain prefactor's own rounding, a few ulps of its exponent's largest term (of size s), stays inside the gate
// while ln B is accurate (lbeta of gamma.hpp: R = 0.5 at s = 2000 against exact sums; the raw sum of three lgamma
// terms, of size s·ln s, reaches R = 4 from max(a,b) = 600 on). R grows in proportion to s, so the Stirling form
// replaces it outright from here.
template <Real T>
inline constexpr T kBetaStirlingOutright = static_cast<T>(sizeof(T) > sizeof(float) ? 1024 : 64);

// Upper-tail probability below which the gamma inverse iterates on ln Q instead of Q.
template <Real T>
inline constexpr T kLogTailProbability = static_cast<T>(sizeof(T) > sizeof(float) ? 1e-15 : 1e-6);
template <Real T>
inline constexpr T kInvSqrt2Pi = static_cast<T>(0.398942280401432677939946059934381868);

// Lower-tail probability below which the gamma inverse checks, after its first step, whether the Wilson-Hilferty start
// is within reach of the Halley iteration; a start many e-folds away is replaced by Newton's method on ln P.
template <Real T>
inline constexpr T kLowerTailProbability = static_cast<T>(sizeof(T) > sizeof(float) ? 1e-8 : 1e-4);

// Single-precision inverses of a shape below this are evaluated in double and rounded once. The density of such a
// shape, x^{a−1}e^{−x}/Γ(a) with Γ(a) ≈ 1/a, is a·x^{a−1}-small, so the single-precision rounding of P(a,x) or I_x(a,b)
// (and of ln B, whose error divides by the shape in the root) is divided by it: R = 3 at a = 0.001 and p = 0.999. For
// the gamma inverse, which a double evaluation costs 40 % more than single precision, only the upper tail (p above 1/2,
// where 1 − P cancels) of a·q below kSingleDoubleTail takes it, and then only when its single-precision root is
// representable (q below kSingleDoubleReach·a): the lower tail has no such cancellation (R ≤ 0.1 measured) and the
// other probabilities are inside the limit (R ≤ 0.7 at a·q ≥ 1e-4); the beta inverse takes it for every probability (it
// is faster in double, whose iteration needs fewer steps to settle).
template <Real T>
inline constexpr T kSingleDoubleShape = static_cast<T>(0.25);
template <Real T>
inline constexpr T kSingleDoubleTail = static_cast<T>(1e-4);
// The root of a single-precision gamma inverse in the upper tail is about exp(−q/a) (ln p ≈ −q, x^a/Γ(1+a) = p): at
// q = 50·a it is 2e-22, and below it (at a = 0.001, p = 0.9 it is 1e-46) the root has all but underflowed, where single
// precision and double both give 0; only a tail with q below kSingleDoubleReach·a is redone in double.
template <Real T>
inline constexpr T kSingleDoubleReach = static_cast<T>(50);
// From this shape a single-precision gamma inverse is evaluated in double: the logarithm of the density at the mode,
// (a−1)·(ln(a−1) − 1) − ln Γ(a), is the difference of two terms of size a·ln a, and float's rounding of them (a few
// ulps of 2e7 at a = 1e6) leaves the density wrong by a factor e^{±1} and more; the Halley step then wanders or divides
// inf by inf (NaN at 3e6, 1e8, 1e9). Against the double inverse at the float inputs (4000 points per half decade):
// R at most 0.004 below 3.2e5, and from there on 75 to 1584 points over 0.5 and NaN.
template <Real T>
inline constexpr T kSingleGammaInverseShape = static_cast<T>(1e5);
// A single-precision betainc value above this is subtracted from 1 in double: the complement I of such a value is below
// 1.5e-2, and the rounding of the value, a few 1e-7 (the prefactor's exponent, ln B = 7 at a shape of 0.001, rounds to
// 5e-7), is scaled by 70 or more: R = 1.2 at (5, 0.001, 0.87), 1.5 at (0.003, 2e-5, 0.999).
template <Real T>
inline constexpr T kSingleComplement = static_cast<T>(0.985);
// Below this shape the root of a single-precision inverse is a power of the probability with an exponent of 1e6 or
// more, which only the edge cases (a probability within 1e-6 of the end of its range, a shape of 1e30 against 1e-30)
// can still represent, and those keep the single-precision path.
template <Real T>
inline constexpr T kSingleDoubleFloor = static_cast<T>(1e-6);
// Below the floor, a larger shape from which the root of an inverse is again beyond what either precision represents
// (the edge row (1e30, 1e-30, 1e-30)).
template <Real T>
inline constexpr T kSingleDoubleLarge = static_cast<T>(1e10);

// Tail probability below which the beta inverse checks, after its first step, whether the A&S 26.5.22 start is within
// reach of the Halley iteration; a start many e-folds away (the far tail of a shape ≫ 1 with the other above 1) is
// replaced by Newton's method on ln I.
template <Real T>
inline constexpr T kBetaLogTailProbability = static_cast<T>(sizeof(T) > sizeof(float) ? 1e-8 : 1e-4);

// Shapes from which the expansions of this header replace the series and the fractions. Near x = a the series and the
// fraction need about 8.5·sqrt(a) terms, and the rounding of their products and of the prefactor's exponent grows with
// them: R = 0.27 of the 1e-12 gate at a = 1e6, 2.6 at 1e8 and wrong from 1e9 on. The uniform expansion (below) costs
// about as much as 300 of those terms, has no limit in the shape and an error of 1e-16 from a = 1e3 on (R = 0.002
// against 0.01 to 0.05 for the series between 1e3 and 3e4), so it takes over from a = 1e3.
template <Real T>
inline constexpr T kGammaUniformShape = static_cast<T>(1e3);
// Beta: the uniform expansion when both shapes are large. Its fallback, the fraction with the Stirling-form prefactor
// (beta_stable_side), needs O(sqrt(a+b)) terms near the mode and the rounding of their product grows with them. The
// worst R of the fallback near the mode, against exact sums over 5e4 to 3e5 points per band: 0.68 of the gate at a + b
// = 2e4..3e4, 0.99 at 5e4..6e4, 1.25 at 6e4..8e4, 6 at 3e5..5e5, 22 at 1e6..2e6, 280 at 1e7..3e7, 2700 at 1e8..3e8;
// so the expansion serves the mode's neighbourhood from a + b = kBetaUniformSum. Its error falls as the smaller shape
// grows (R = 0.85 of the gate at a smaller shape of 100, 0.5 from 200, 0.1 from 1e3, against scipy) and its
// coefficients grow with the ratio of the shapes, which the incomplete-gamma expansion below takes over first. It
// serves only the part of the band where the fallback is over the gate (beta_uniform_width, measured per sum on the two
// sides of the mode): farther out the fraction converges in a few hundred terms, is inside the limit and costs a
// quarter of the expansion's 1.6 us. From kBetaUniformFullBand on the expansion takes the whole band: the fallback's
// own error needs ever wider margins there and it returns garbage from a + b = 1e16 on. In single precision neither the
// expansion nor the fallback of the uniform expansion's shapes is usable from 1e10 on (a float x no longer resolves the
// mode's width against the rounding of the shapes' ratio), so those sums are evaluated in double and narrowed.
template <Real T>
inline constexpr T kBetaUniformMin = static_cast<T>(sizeof(T) > sizeof(float) ? 2e2 : 1e2);
template <Real T>
inline constexpr T kBetaUniformSum = static_cast<T>(sizeof(T) > sizeof(float) ? 6e4 : 5e4);
template <Real T>
inline constexpr T kBetaUniformFullBand = static_cast<T>(sizeof(T) > sizeof(float) ? 1e14 : 1e10);
template <Real T>
inline constexpr T kBetaUniformSkew = static_cast<T>(sizeof(T) > sizeof(float) ? 1e15 : 1e4);
// Beta with one small shape: when the other is at least this, the fraction needs thousands of terms and its prefactor
// is the Stirling form with one small shape; the expansion in incomplete gamma functions (BGRAT below) takes over. Its
// terms fall like (a²/2b)^k/k!: sixteen of them reach 5e-14 where the small shape squared equals twice the large one.
template <Real T>
inline constexpr T kBetaSmallShapeBig = static_cast<T>(sizeof(T) > sizeof(float) ? 1e4 : 3e3);
// Below this small shape the incomplete-gamma expansion serves everything its convergence allows (the large shape at
// least half the small one squared); from it on the uniform expansion serves the shapes below that limit and the
// incomplete-gamma expansion those beyond a skew of kBetaUniformSkew.
template <Real T>
inline constexpr T kBetaSmallShapeCap = static_cast<T>(sizeof(T) > sizeof(float) ? 1e3 : 1e2);
// The uniform expansion is used for |relative offset from the mode| below this; beyond it the far tail is far below the
// working range for these shapes, or the fraction converges in a few hundred terms.
template <Real T>
inline constexpr T kUniformBand = static_cast<T>(0.15);
// The band of the beta expansion from a smaller shape of kBetaSmallShapeCap on: at huge sums the gate's width reaches
// past 0.15 of that shape (6 standard deviations are 0.19 of a shape of 1e3), where the fallback is still over the
// limit and the expansion is not (R at most 0.007 against exact sums, offset up to 0.5 of the smaller shape and every
// sum up to 1e15).
template <Real T>
inline constexpr T kBetaUniformBandWide = static_cast<T>(sizeof(T) > sizeof(float) ? 0.5 : 0.15);
// Below this smaller shape the incomplete-gamma expansion of a single-precision call runs in double: at a shape of 1e-3
// or less its float rounding is over the probe limit near one (the value is 1 minus a tail of 1e-5 at a shape of 1e-40,
// R up to 7.5) and at 1e-40 and below the sum overflows. From kGammaShapeBig on it also runs in double, its float terms
// overflow there (a shape of 1e4 reaches 1e39 at the fifth term, and the power of 1/b underflows to 0).
template <Real T>
inline constexpr T kBetaFloatSmallShapeMin = static_cast<T>(1e-3);
// The smallest smaller shape of the incomplete-gamma expansion: in double a subnormal shape overflows its prefactor
// (x^a/(a·Γ(a)) is formed through 1/a) and the fallback serves it as base did; a single-precision call of a subnormal
// shape is evaluated in double (kBetaFloatSmallShapeMin), where the shape is normal.
template <Real T>
inline constexpr T kBetaSmallShapeFloor =
    static_cast<T>(sizeof(T) > sizeof(float) ? std::numeric_limits<double>::min() : 0.0);
// Terms of the expansion in the offset from the mode.
inline constexpr int kUniformTerms = 12;
// Terms of the expansion in incomplete gamma functions.
inline constexpr int kSmallShapeTerms = 16;

// ln(1+t) − t, the part of a Stirling-form exponent that stays small near t = 0 (it is about −t²/2).
template <Real T>
[[nodiscard]] T log1p_deficit(T t) noexcept
{
    return crd::math::log1p(t) - t;
}

// The same, with 1+t also known to full relative precision: where it is below one half, ln(1+t) is taken from it
// directly, because forming it from t = (1+t) − 1 near −1 would round away its digits.
template <Real T>
[[nodiscard]] T log1p_deficit(T t, T one_plus_t) noexcept
{
    return (one_plus_t < static_cast<T>(0.5)) ? crd::math::log(one_plus_t) - t : log1p_deficit(t);
}

// ln(1+t) − t for small |t| without the cancellation of log1p(t) − t (which is about −t²/2 yet loses the rounding of t
// itself, a relative 2ε/|t|). With w = t/(2+t): ln(1+t) = 2·atanh(w) and t = 2w/(1−w), so
//   ln(1+t) − t = −2w²·[1/(1−w) − w·(1/3 + w²/5 + w⁴/7 + …)],
// whose terms do not cancel. Nine terms of the bracket reach double precision for |t| < 0.2. The beta expansion's wide
// band takes t down to −0.5, where the truncation is 5.25e-11 of the bracket; its effect on I_x is the exponent
// (0.19·a at least, a ≥ 1e3) times that truncation, against a value of e^{−0.19·a} or less, so no result moves.
template <Real T>
[[nodiscard]] T log1p_deficit_small(T t) noexcept
{
    const T w = t / (static_cast<T>(2) + t);
    const T w2 = w * w;
    T series = static_cast<T>(1.0 / 19.0);
    for (int k = 8; k >= 1; --k)
    {
        series = static_cast<T>(1) / static_cast<T>(2 * k + 1) + w2 * series;
    }
    return static_cast<T>(-2) * w2 * (static_cast<T>(1) / (static_cast<T>(1) - w) - w * series);
}

// x^a e^{−x} / Γ(a), the factor in front of both gamma expansions. Below kGammaShapeBig it is the plain exponential of
// −x + a·ln x − ln Γ(a). From there on, Stirling's formula for Γ(a) (DLMF 5.11.3) gives the cancellation-free form
//   x^a e^{−x} / Γ(a) = sqrt(a/2π) · exp(a·(ln(1+d) − d) − S(a)),   d = (x−a)/a,
// with S the Stirling correction (the ln Γ(a) argument `gln` is then not needed; cf. DiDonato & Morris 1986, 1992).
template <Real T>
CRD_NOINLINE T gamma_prefactor_large(T a, T x, T gln) noexcept
{
    if (std::isinf(a))
    {
        return crd::math::exp(-x + a * crd::math::log(x) - gln);
    }
    const T rel = (x - a) / a;
    return crd::math::sqrt(a) * kInvSqrt2Pi<T> *
           crd::math::exp(a * detail::log1p_deficit(rel) - detail::stirling_correction(a));
}

template <bool Large, Real T>
[[nodiscard]] T gamma_prefactor(T a, T x, T gln) noexcept
{
    if constexpr (Large)
    {
        return gamma_prefactor_large(a, x, gln); // out of line: the small-shape path stays as compact as it was
    }
    else
    {
        return crd::math::exp(-x + a * crd::math::log(x) - gln);
    }
}

// ln of the same factor, for the far upper tail where the factor itself underflows.
template <Real T>
[[nodiscard]] T gamma_log_prefactor(T a, T x, T gln) noexcept
{
    if (a < kGammaShapeBig<T> || std::isinf(a))
    {
        return -x + a * crd::math::log(x) - gln;
    }
    const T rel = (x - a) / a;
    return crd::math::log(crd::math::sqrt(a) * kInvSqrt2Pi<T>) + a * detail::log1p_deficit(rel) -
           detail::stirling_correction(a);
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

// The Σ-series of P(a,x), valid x < a+1, without the prefactor: P(a,x) = prefactor · sum, and the sum alone is what the
// far lower tail takes the logarithm of.
template <bool Large, Real T>
[[nodiscard]] T gammap_series_sum(T a, T x) noexcept
{
    T ap = a;
    T del = static_cast<T>(1) / a;
    T sum = del;
    int n = 0;
    for (; n < kBaseTerms; ++n)
    {
        ap += static_cast<T>(1);
        del *= x / ap;
        sum += del;
        if (std::abs(del) < std::abs(sum) * kCfEps<T>)
        {
            return sum;
        }
    }
    if constexpr (Large) // slow convergence is genuine only for large shapes
    {
        for (; n < kMaxTerms; ++n)
        {
            ap += static_cast<T>(1);
            del *= x / ap;
            sum += del;
            if (!(std::abs(del) >= std::abs(sum) * kCfEps<T>)) // a NaN fails the comparison: it ends the run at once
            {
                break;
            }
        }
    }
    return sum;
}

// Σ-series for P(a,x), valid x < a+1. `gln` = precomputed lgamma(a) (amortised by fixed-shape distributions).
template <bool Large, Real T>
[[nodiscard]] T gammap_series(T a, T x, T gln) noexcept
{
    return detail::gammap_series_sum<Large>(a, x) * detail::gamma_prefactor<Large>(a, x, gln);
}

// Continued fraction for Q(a,x), valid x ≥ a+1 (DLMF 8.9.2):
//   Γ(a,x) = e^{-x} x^a / (x+1−a − 1·(1−a)/(x+3−a − 2·(2−a)/(x+5−a − …))),
// i.e. partial numerators j·(a−j) and partial denominators x+1−a+2j, summed by the modified Lentz method.
// `gammaq_cf_value` is the fraction alone; `gammaq_cf` multiplies in the prefactor, `gln` = precomputed lgamma(a).
template <bool Large, Real T>
[[nodiscard]] T gammaq_cf_value(T a, T x) noexcept
{
    const T tiny = fpmin<T>();
    const T first_den = x + static_cast<T>(1) - a;
    T big = static_cast<T>(1) / tiny;      // Lentz's C (ratio of successive numerators)
    T inv = static_cast<T>(1) / first_den; // Lentz's D, kept inverted
    T value = inv;
    // One pass of the loop for index j: the modified-Lentz update; returns the factor applied to `value`.
    const auto sweep = [&](int j) noexcept
    {
        const T jt = static_cast<T>(j);
        const T num = jt * (a - jt);
        const T den = first_den + static_cast<T>(2 * j);
        inv = detail::lentz_guard(den + num * inv, tiny);
        big = detail::lentz_guard(den + num / big, tiny);
        inv = static_cast<T>(1) / inv;
        const T ratio = inv * big;
        value *= ratio;
        return ratio;
    };
    int j = 1;
    for (; j < kBaseTerms; ++j)
    {
        if (std::abs(sweep(j) - static_cast<T>(1)) < kCfEps<T>)
        {
            return value;
        }
    }
    if constexpr (Large) // slow convergence is genuine only for large shapes
    {
        for (; j < kMaxTerms; ++j)
        {
            if (!(std::abs(sweep(j) - static_cast<T>(1)) >= kCfEps<T>)) // a NaN fails the comparison: it ends the run
            {
                break;
            }
        }
    }
    return value;
}

template <bool Large, Real T>
[[nodiscard]] T gammaq_cf(T a, T x, T gln) noexcept
{
    const T value = detail::gammaq_cf_value<Large>(a, x); // the fraction first, as the prefactor needs nothing from it
    return detail::gamma_prefactor<Large>(a, x, gln) * value;
}

// Uniform expansion for large shapes (Temme 1979, DLMF 8.12, for the gamma ratios; Temme 1982, DLMF 8.18(ii), for the
// beta; derived here by Laplace's method). Both integrals have the form
//   F(ξ) = e^{∓S}·sqrt(s/2π) ∫ e^{−s·η²/2} H(η) dη,
// over the part of the real line on one side of ξ, where s is the large parameter (a, or a+b), η is the signed square
// root of twice the exponent's drop from its maximum (η² = −2Ψ, Ψ = ln λ − λ + 1 for the gamma ratios, with λ = x/a;
// Ψ = ρ·ln(x/ρ) + σ·ln((1−x)/σ), ρ = a/s, σ = b/s, for the beta), H is the remaining smooth density factor and S the
// Stirling corrections of the normalisation. Writing H = Σ h_k η^k, the integral is a sum of Gaussian partial moments,
//   κ_0 = ½·erfcx(sqrt X),  κ_1 = 1/sqrt(2πs),  κ_k = u^{k−1}/sqrt(2πs) + (k−1)/s·κ_{k−2},
// X = s·η²/2 = −s·Ψ and u = |η|, each carrying the one factor e^{−X}; the far tail (the side away from the mode) is
//   e^{−S}·e^{−X}·Σ_k h_k·(±1)^k·κ_k,
// with no cancellation, and the near tail is its complement. The coefficients h_k are those of Temme's c_k, obtained
// without his recurrence: with v the offset from the mode in the scale of its width, η = v·E(v), E² = 1 + Σ_{k≥1}
// bend_k v^k, and H dη = m(v) dv, Lagrange's inversion gives h_k = [v^k] m(v)·E(v)^{−(k+1)}.
template <Real T>
struct UniformSeries
{
    T h[kUniformTerms + 1];
};

template <Real T>
[[nodiscard]] constexpr UniformSeries<T> uniform_series(const T (&bend)[kUniformTerms + 1],
                                                        const T (&amp)[kUniformTerms + 1]) noexcept
{
    constexpr int n = kUniformTerms;
    T inv_root[n + 1] = {}; // E(v)^{−1} = (1 + Y)^{−1/2}, Y = Σ bend_k v^k
    inv_root[0] = static_cast<T>(1);
    for (int k = 1; k <= n; ++k)
    {
        T acc = static_cast<T>(0);
        for (int i = 1; i <= k; ++i)
        {
            acc += (static_cast<T>(-0.5) * static_cast<T>(i) - static_cast<T>(k - i)) * bend[i] * inv_root[k - i];
        }
        inv_root[k] = acc / static_cast<T>(k);
    }
    UniformSeries<T> out{};
    T power[n + 1] = {}; // E(v)^{−(k+1)}, advanced by one factor per coefficient
    for (int i = 0; i <= n; ++i)
    {
        power[i] = inv_root[i];
    }
    for (int k = 0; k <= n; ++k)
    {
        T coefficient = static_cast<T>(0);
        for (int i = 0; i <= k; ++i)
        {
            coefficient += amp[i] * power[k - i];
        }
        out.h[k] = coefficient;
        T next[n + 1] = {};
        for (int i = 0; i <= n; ++i)
        {
            for (int j = 0; i + j <= n; ++j)
            {
                next[i + j] += power[i] * inv_root[j];
            }
        }
        for (int i = 0; i <= n; ++i)
        {
            power[i] = next[i];
        }
    }
    return out;
}

// Gamma ratios: ln(1+v) = Σ (−1)^{j+1} v^j/j gives bend_k = 2·(−1)^k/(k+2) and the density factor 1/(1+v).
template <Real T>
[[nodiscard]] constexpr UniformSeries<T> gamma_uniform_series() noexcept
{
    T bend[kUniformTerms + 1] = {};
    T amp[kUniformTerms + 1] = {};
    T sign = static_cast<T>(1);
    for (int k = 0; k <= kUniformTerms; ++k)
    {
        amp[k] = sign;
        if (k > 0)
        {
            bend[k] = static_cast<T>(2) * sign / static_cast<T>(k + 2);
        }
        sign = -sign;
    }
    return uniform_series<T>(bend, amp);
}

template <Real T>
inline constexpr UniformSeries<T> kGammaUniformSeries = gamma_uniform_series<T>();

// Beta: with α = sqrt(b/a), the offset v of x from the mode ρ in the scale sqrt(ρσ) gives bend_k = 2·((−1)^k·σ·α^k +
// ρ·α^{−k})/(k+2) and the density factor 1/((1+αv)(1−v/α)) = (α²/(1+α²))/(1+αv) + (1/(1+α²))/(1−v/α), whose
// coefficients are (α²·(−α)^k + α^{−k})/(1+α²).
template <Real T>
[[nodiscard]] UniformSeries<T> beta_uniform_series(T a, T b) noexcept
{
    const T s = a + b;
    const T rho = a / s;
    const T sigma = b / s;
    const T alpha = crd::math::sqrt(b / a);
    const T alpha_sq = alpha * alpha;
    T bend[kUniformTerms + 1] = {};
    T amp[kUniformTerms + 1] = {};
    T up = static_cast<T>(1);   // α^k
    T down = static_cast<T>(1); // α^{−k}
    T sign = static_cast<T>(1);
    for (int k = 0; k <= kUniformTerms; ++k)
    {
        amp[k] = (alpha_sq * sign * up + down) / (static_cast<T>(1) + alpha_sq);
        if (k > 0)
        {
            bend[k] = static_cast<T>(2) * (sign * sigma * up + rho * down) / static_cast<T>(k + 2);
        }
        up *= alpha;
        down /= alpha;
        sign = -sign;
    }
    return uniform_series<T>(bend, amp);
}

// erfcx(w) = e^{w²}·erfc(w) for w ≥ 0, from the gamma ratio erfc(w) = Q(½, w²): the fraction's value carries the whole
// factor from w² = 3/2 on (the prefactor of Q(½, x) is e^{−x}·sqrt(x/π)); below it, 1 − P(½, w²) is not small.
template <Real T>
[[nodiscard]] T gauss_scaled_tail(T w) noexcept
{
    const T x = w * w;
    if (x >= static_cast<T>(1.5))
    {
        const T inv_sqrt_pi = static_cast<T>(0.564189583547756286948079451560772586);
        return w * inv_sqrt_pi * gammaq_cf_value<false>(static_cast<T>(0.5), x);
    }
    if (x == static_cast<T>(0))
    {
        return static_cast<T>(1);
    }
    const T ln_gamma_half = static_cast<T>(0.572364942924700087071713675676529356); // ln sqrt(π)
    return crd::math::exp(x) * (static_cast<T>(1) - gammap_series<false>(static_cast<T>(0.5), x, ln_gamma_half));
}

// Σ_k h_k·(±1)^k·κ_k without the factor e^{−X}: the far tail is e^{−X} times this. `lower` is the sign of η (the far
// tail lies below the mode), which flips the odd moments.
template <Real T>
[[nodiscard]] T uniform_scaled_tail(const UniformSeries<T>& series, T s, T exponent, bool lower) noexcept
{
    const T root = kInvSqrt2Pi<T> / crd::math::sqrt(s);
    const T u = crd::math::sqrt(static_cast<T>(2) * exponent / s);
    T even = static_cast<T>(0.5) * gauss_scaled_tail(crd::math::sqrt(exponent)); // κ_0, then κ_2, κ_4, …
    T odd = root;                                                                  // κ_1, then κ_3, …
    T total = series.h[0] * even + (lower ? -series.h[1] : series.h[1]) * odd;
    T power = u; // u^{k−1}
    for (int k = 2; k <= kUniformTerms; ++k)
    {
        const bool even_order = (k % 2) == 0;
        const T moment = power * root + static_cast<T>(k - 1) / s * (even_order ? even : odd);
        (even_order ? even : odd) = moment;
        total += series.h[k] * ((lower && !even_order) ? -moment : moment);
        power *= u;
    }
    return total;
}

// The far tail of one expansion: e^{−exponent}·scaled, on the lower side of the mode when `lower`.
template <Real T>
struct UniformTail
{
    T exponent;
    T scaled;
    bool lower;
};

// The tail probability on the requested side, P (lower) or Q: the far tail itself, or its complement.
template <Real T>
[[nodiscard]] T uniform_side(const UniformTail<T>& tail, bool want_lower) noexcept
{
    const T far = crd::math::exp(-tail.exponent) * tail.scaled;
    return (tail.lower == want_lower) ? far : static_cast<T>(1) - far;
}

// Its logarithm, which does not underflow with the far tail.
template <Real T>
[[nodiscard]] T uniform_log_side(const UniformTail<T>& tail, bool want_lower) noexcept
{
    if (tail.lower == want_lower)
    {
        return crd::math::log(tail.scaled) - tail.exponent;
    }
    return crd::math::log1p(-crd::math::exp(-tail.exponent) * tail.scaled);
}

template <Real T>
[[nodiscard]] bool gamma_uniform_applies(T a, T x) noexcept
{
    return a >= kGammaUniformShape<T> && std::abs((x - a) / a) < kUniformBand<T>; // false for a NaN or an infinite a
}

template <Real T>
[[nodiscard]] UniformTail<T> gamma_uniform_tail(T a, T x) noexcept
{
    const T offset = (x - a) / a; // exact numerator: x and a are within a factor of two
    const T exponent = -a * log1p_deficit_small(offset);
    const bool lower = offset < static_cast<T>(0);
    const T scaled =
        crd::math::exp(-stirling_correction(a)) * uniform_scaled_tail(kGammaUniformSeries<T>, a, exponent, lower);
    return {exponent, scaled, lower};
}

// P (lower) or Q at a point where gamma_uniform_applies.
template <Real T>
[[nodiscard]] T gamma_uniform_value(T a, T x, bool want_lower) noexcept
{
    return uniform_side(gamma_uniform_tail(a, x), want_lower);
}

// P and Q from the one expansion that suits x: the series gives P below a+1, the fraction gives Q above it.
template <bool Large, Real T>
[[nodiscard]] T gamma_expand_p(T a, T x, T gln) noexcept
{
    if constexpr (Large)
    {
        if (gamma_uniform_applies(a, x))
        {
            return gamma_uniform_value(a, x, true);
        }
    }
    return (x < a + static_cast<T>(1)) ? gammap_series<Large>(a, x, gln)
                                       : static_cast<T>(1) - gammaq_cf<Large>(a, x, gln);
}

template <bool Large, Real T>
[[nodiscard]] T gamma_expand_q(T a, T x, T gln) noexcept
{
    if constexpr (Large)
    {
        if (gamma_uniform_applies(a, x))
        {
            return gamma_uniform_value(a, x, false);
        }
    }
    return (x < a + static_cast<T>(1)) ? static_cast<T>(1) - gammap_series<Large>(a, x, gln)
                                       : gammaq_cf<Large>(a, x, gln);
}

// The expansion the inverses differentiate: P when `lower_side` (x below a+1), else Q.
template <bool Large, Real T>
[[nodiscard]] T gamma_expansion(T a, T x, T gln, bool lower_side) noexcept
{
    if constexpr (Large)
    {
        if (gamma_uniform_applies(a, x))
        {
            return gamma_uniform_value(a, x, lower_side);
        }
    }
    return lower_side ? gammap_series<Large>(a, x, gln) : gammaq_cf<Large>(a, x, gln);
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
    if (a >= detail::kGammaShapeBig<T>)
    {
        return detail::gamma_expand_p<true>(a, x, gln);
    }
    return detail::gamma_expand_p<false>(a, x, gln);
}

// Regularized upper incomplete gamma Q(a,x) with PRECOMPUTED gln=lgamma(a).
template <Real T>
[[nodiscard]] T gammainc_q(T a, T x, T gln) noexcept
{
    if (x <= static_cast<T>(0))
    {
        return static_cast<T>(1);
    }
    if (a >= detail::kGammaShapeBig<T>)
    {
        return detail::gamma_expand_q<true>(a, x, gln);
    }
    return detail::gamma_expand_q<false>(a, x, gln);
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

namespace detail
{
// Far upper tail of the gamma inverse: Q and its density sink toward underflow, so Newton's method is run on
// ln Q(a,x) − ln q, whose slope is minus the hazard rate P'(a,x)/Q(a,x); both parts are formed as logarithms and never
// underflow. `x` is the starting point.
template <Real T>
CRD_NOINLINE T gammainc_inv_log_tail(T a, T q, T gln, T x) noexcept
{
    const T tol = static_cast<T>(8) * std::numeric_limits<T>::epsilon();
    const T am1 = a - static_cast<T>(1);
    const T ln_q = crd::math::log(q);
    for (int j = 0; j < 60; ++j)
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        T ln_tail = static_cast<T>(0);
        if (gamma_uniform_applies(a, x))
        {
            ln_tail = uniform_log_side(gamma_uniform_tail(a, x), false);
        }
        else
        {
            ln_tail = (x < a + static_cast<T>(1))
                          ? crd::math::log(gammainc_q(a, x, gln))
                          : gamma_log_prefactor(a, x, gln) + crd::math::log(gammaq_cf_value<true>(a, x));
        }
        const T ln_density = am1 * crd::math::log(x) - x - gln;
        const T step = (ln_tail - ln_q) / crd::math::exp(ln_density - ln_tail);
        const T previous = x;
        x += step;
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

// ln P(a,x) for the lower tail, the logarithm of the series' sum plus the logarithm of the prefactor (so that neither
// underflows); past a+1 the lower tail is the complement of the fraction's value.
template <bool Large, Real T>
[[nodiscard]] T gamma_log_lower(T a, T x, T gln) noexcept
{
    if (gamma_uniform_applies(a, x))
    {
        return uniform_log_side(gamma_uniform_tail(a, x), true);
    }
    if (x < a + static_cast<T>(1))
    {
        return gamma_log_prefactor(a, x, gln) + crd::math::log(gammap_series_sum<Large>(a, x));
    }
    return crd::math::log1p(-gammaq_cf<Large>(a, x, gln));
}

// Far lower tail of the gamma inverse for a > 1: P and its density sink toward underflow as p does, and the Halley
// iteration on P itself then needs one step per e-fold of distance from its start. Newton's method is run on
// ln P(a,x) − ln p instead, which is increasing and concave (P(a,·) is log-concave for a ≥ 1: its density is, and
// integration preserves it, Prékopa 1973), so an iterate below the root stays below it and converges monotonically,
// quadratically at the end. The start is the power law of the leading term, P(a,x) < x^a/Γ(a+1) ⇒ x = (p·Γ(a+1))^{1/a}
// lies below the root; the caller's near-root estimate replaces it when that estimate is also below the root and
// larger.
template <bool Large, Real T>
CRD_NOINLINE T gammainc_inv_log_lower(T a, T p, T gln, T estimate, bool estimate_below) noexcept
{
    const T tol = static_cast<T>(8) * std::numeric_limits<T>::epsilon();
    const T am1 = a - static_cast<T>(1);
    const T ln_p = crd::math::log(p);
    const T power_law = crd::math::exp((ln_p + gln + crd::math::log(a)) / a);
    T x = (estimate_below && estimate > power_law) ? estimate : power_law;
    for (int j = 0; j < 60; ++j)
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        const T ln_tail = gamma_log_lower<Large>(a, x, gln);
        const T ln_density = am1 * crd::math::log(x) - x - gln;
        const T step = (ln_tail - ln_p) / crd::math::exp(ln_density - ln_tail);
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

// Halley refinement shared by both gamma inverses: solves P(a,x) = p = 1 − q. The caller passes the probability that is
// known exactly on each side (p or q), and `upper` says to iterate on the upper tail, Q(a,x) = q, which is the
// accurate side once p > ½: there 1 − p is exact, whereas P − p would cancel down to the rounding of P. The two
// residuals, P − p and q − Q, are equal; only their rounding differs.
//
// Starting point: Wilson-Hilferty cube for a > 1 (fed by the A&S 26.2.22 deviate), a two-piece estimate of P(a,·) for
// a ≤ 1 (small-x power law, then the exponential tail; cf. DiDonato & Morris 1986; the quadratic fit of P(a,1), 0.253
// and 0.12, keeps its Numerical Recipes origin). Refinement: Halley steps with the density
// P'(a,x) = x^{a−1} e^{−x}/Γ(a) and the curvature ratio P''/P' = (a−1)/x − 1; the correction is damped when it would
// overshoot.
template <bool Large, Real T>
[[nodiscard]] T gammainc_inv_refine(T a, T p, T q, bool upper) noexcept
{
    const T tol = static_cast<T>(8) * std::numeric_limits<T>::epsilon();
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
        const T y = upper ? -detail::normal_deviate_guess(q) : detail::normal_deviate_guess(p);
        const T cube_root = static_cast<T>(1) - static_cast<T>(1) / (static_cast<T>(9) * a) -
                            y / (static_cast<T>(3) * crd::math::sqrt(a));
        x = std::max(static_cast<T>(1e-3), a * crd::math::pow(cube_root, static_cast<T>(3)));
    }
    else
    {
        const T beyond = a * (static_cast<T>(0.253) + a * static_cast<T>(0.12)); // ≈ Q(a,1) = 1 − P(a,1)
        if (q > beyond)
        {
            x = crd::math::pow(p / (static_cast<T>(1) - beyond), static_cast<T>(1) / a);
        }
        else
        {
            x = static_cast<T>(1) - crd::math::log(q / beyond);
        }
    }
    if (upper && q < kLogTailProbability<T>)
    {
        return gammainc_inv_log_tail(a, q, gln, x);
    }
    const T peak = crd::math::exp(ln_scale); // density at the reference point x_ref
    for (int j = 0; j < 12; ++j)
    {
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        // One expansion serves both tails: the series gives P (x < a+1), the fraction gives Q.
        const bool lower_side = x < a + static_cast<T>(1);
        const T expansion = detail::gamma_expansion<Large>(a, x, gln, lower_side);
        if (j == 0 && !upper && a > static_cast<T>(1) && p < kLowerTailProbability<T>)
        {
            // A start more than a few e-folds from the root in P is out of the Halley iteration's reach.
            const T lower_tail = lower_side ? expansion : static_cast<T>(1) - expansion;
            if (!(lower_tail < static_cast<T>(64) * p && lower_tail * static_cast<T>(64) > p))
            {
                return gammainc_inv_log_lower<Large>(a, p, gln, x, lower_tail < p);
            }
        }
        const T residual = lower_side ? (upper ? q - (static_cast<T>(1) - expansion) : expansion - p)
                                      : (upper ? q - expansion : (static_cast<T>(1) - expansion) - p);
        const T density = peak * crd::math::exp(am1 * (crd::math::log(x) - ln_ref) - (x - x_ref));
        const T newton = residual / density;
        const T curvature = am1 / x - static_cast<T>(1);
        const T step =
            newton / (static_cast<T>(1) - static_cast<T>(0.5) * std::min(static_cast<T>(1), newton * curvature));
        if (!(std::abs(step) < std::numeric_limits<T>::infinity())) // a density or residual out of range: keep x
        {
            break;
        }
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
} // namespace detail

// Inverse of P(a,·): returns x with P(a,x) = p.
template <Real T>
[[nodiscard]] T gammainc_p_inv(T a, T p) noexcept
{
    if constexpr (sizeof(T) < sizeof(double))
    {
        // The upper tail of a small shape: its single-precision root is the rounding of 1 − P divided by a·q (R = 3 at
        // a = 0.001, q = 0.001).
        if (p > static_cast<T>(0.5) && a < detail::kSingleDoubleShape<T> && a >= detail::kSingleDoubleFloor<T> &&
            a * (static_cast<T>(1) - p) < detail::kSingleDoubleTail<T> &&
            static_cast<T>(1) - p < detail::kSingleDoubleReach<T> * a)
        {
            return static_cast<T>(gammainc_p_inv<double>(static_cast<double>(a), static_cast<double>(p)));
        }
        if (a >= detail::kSingleGammaInverseShape<T>) // a shape whose float density cannot be formed (see there)
        {
            return static_cast<T>(gammainc_p_inv<double>(static_cast<double>(a), static_cast<double>(p)));
        }
    }
    if (!(a > static_cast<T>(0))) // a non-positive or NaN shape
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
    const T q = static_cast<T>(1) - p;
    if (a >= detail::kGammaShapeBig<T>)
    {
        return detail::gammainc_inv_refine<true>(a, p, q, p > static_cast<T>(0.5));
    }
    return detail::gammainc_inv_refine<false>(a, p, q, p > static_cast<T>(0.5));
}

// Inverse of Q(a,·): returns x with Q(a,x) = q. It iterates on whichever tail is the smaller, so a small q keeps its
// full relative accuracy instead of passing through 1 − q.
template <Real T>
[[nodiscard]] T gammainc_q_inv(T a, T q) noexcept
{
    if constexpr (sizeof(T) < sizeof(double))
    {
        if (q < static_cast<T>(0.5) && a < detail::kSingleDoubleShape<T> && a >= detail::kSingleDoubleFloor<T> &&
            a * q < detail::kSingleDoubleTail<T> && q < detail::kSingleDoubleReach<T> * a)
        {
            return static_cast<T>(gammainc_q_inv<double>(static_cast<double>(a), static_cast<double>(q)));
        }
        if (a >= detail::kSingleGammaInverseShape<T>)
        {
            return static_cast<T>(gammainc_q_inv<double>(static_cast<double>(a), static_cast<double>(q)));
        }
    }
    if (!(a > static_cast<T>(0)))
    {
        return std::numeric_limits<T>::quiet_NaN();
    }
    if (q >= static_cast<T>(1))
    {
        return static_cast<T>(0);
    }
    if (q <= static_cast<T>(0))
    {
        return std::numeric_limits<T>::infinity();
    }
    if (a >= detail::kGammaShapeBig<T>)
    {
        return detail::gammainc_inv_refine<true>(a, static_cast<T>(1) - q, q, q < static_cast<T>(0.5));
    }
    return detail::gammainc_inv_refine<false>(a, static_cast<T>(1) - q, q, q < static_cast<T>(0.5));
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
[[nodiscard]] T betacf(T a, T b, T x, T tol = kCfEps<T>) noexcept
{
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
    // One pass of the loop: folds in the even and the odd coefficient of index m; returns the odd factor.
    const auto sweep = [&](int m) noexcept
    {
        const T mt = static_cast<T>(m);
        const T two_m = static_cast<T>(2 * m);
        fold(mt * (b - mt) * x / ((a_prev + two_m) * (a + two_m)));
        return fold(-(a + mt) * (apb + mt) * x / ((a + two_m) * (a_next + two_m)));
    };
    int m = 1;
    for (; m <= kBaseTerms; ++m)
    {
        if (std::abs(sweep(m) - static_cast<T>(1)) < tol)
        {
            return value;
        }
    }
    if (apb >= static_cast<T>(2) * kBetaShapeBig<T>) // slow convergence is genuine only for large shapes
    {
        for (; m <= kMaxTerms; ++m)
        {
            if (!(std::abs(sweep(m) - static_cast<T>(1)) >= tol)) // a NaN fails the comparison: it ends the run
            {
                break;
            }
        }
    }
    return value;
}

// x^a (1−x)^b / B(a,b), the factor in front of the beta fraction, for 0 < x < 1: the plain exponential of
// −ln B + a·ln x + b·ln(1−x), with lbeta_ab = ln B(a,b) precomputed by the caller.
template <Real T>
[[nodiscard]] T beta_prefactor_plain(T a, T b, T x, T lbeta_ab) noexcept
{
    return crd::math::exp(-lbeta_ab + a * crd::math::log(x) + b * crd::math::log1p(-x));
}

// The same factor without the cancellation of the plain form, for max(a,b) ≥ kStirlingMin: Stirling's formula (DLMF
// 5.11.3) is applied to every gamma function that is large and the cancelling parts are combined (cf. DiDonato &
// Morris 1992). With s = a+b and both parameters large,
//   x^a y^b / B(a,b) = sqrt(a·b/(2π s)) · exp(a·φ(u) + b·φ(v) + S(s) − S(a) − S(b)),
//   φ(t) = ln(1+t) − t,   u = (x·b − y·a)/a,   v = −(x·b − y·a)/b,   y = 1−x,
// where a·u + b·v = 0 makes the exponent small near the mode x = a/s. With one parameter m small and the other, L,
// large, ln Γ(m) is kept and only the ratio Γ(m+L)/Γ(L) is expanded:
//   x_m^m x_L^L / B = exp(−ln Γ(m) + m·ln(x_m·L) + L·ln x_L + (m+L−½)·ln(1+m/L) − m + S(m+L) − S(L)),
// where x_m, x_L are the powers' bases (x or 1−x, whichever carries m and L).
template <Real T>
CRD_NOINLINE T beta_prefactor_stable(T a, T b, T x) noexcept
{
    const T y = static_cast<T>(1) - x;
    if (std::min(a, b) >= kStirlingMin<T>)
    {
        const T s = a + b;
        const T gap = x * b - y * a;
        // 1 + gap/a = x·s/a and 1 − gap/b = y·s/b: far from the mode one of them is small, and is taken from these
        // products rather than from the offset, which would round it away.
        const T exponent = a * log1p_deficit(gap / a, x * s / a) + b * log1p_deficit(-gap / b, y * s / b) +
                           stirling_correction(s) - stirling_correction(a) - stirling_correction(b);
        return crd::math::sqrt(a / s * b) * kInvSqrt2Pi<T> * crd::math::exp(exponent);
    }
    const bool a_small = a < b;
    const T m = a_small ? a : b;
    const T big = a_small ? b : a;
    const T base_m = a_small ? x : y;
    const T ln_base_big = a_small ? crd::math::log1p(-x) : crd::math::log(x);
    const T exponent = -lgamma(m) + m * crd::math::log(base_m * big) + big * ln_base_big +
                       (m + big - static_cast<T>(0.5)) * crd::math::log1p(m / big) - m + stirling_correction(m + big) -
                       stirling_correction(big);
    return crd::math::exp(exponent);
}

// x·(a+b) − a, the offset of x from the mode a/(a+b) in the scale of the shapes, exactly rounded once: the offset is
// the difference of two numbers that agree to many digits, and the rounding of x·(a+b) alone would leave a relative
// error of ε·a/|offset|, as large as 1e-10 at a = 1e12. The product and the sum are formed with their error terms
// (Dekker 1971 / Knuth), the difference of the leading parts is exact within the band (the parts agree within a
// factor of two).
template <Real T>
[[nodiscard]] T beta_mode_offset(T a, T b, T x) noexcept
{
    const T sum = a + b;
    const T virtual_b = sum - a;
    const T sum_error = (a - (sum - virtual_b)) + (b - virtual_b);
    const T product = x * sum;
    const T product_error = crd::math::fma(x, sum, -product);
    return (product - a) + (product_error + x * sum_error);
}

// One row of a distance gate: from `below` on the next row applies; `lower` is the gate where I_x is the small side of
// the mode (x below it), `upper` where I_x is near one (x above it), which differ because the limit there is relative
// to one instead of to the tail.
struct BetaGateRow
{
    double below;
    double lower;
    double upper;
};

// The gate of the first row whose bound lies above `key`; `beyond` past the last row.
template <int N>
[[nodiscard]] constexpr double beta_gate(const BetaGateRow (&rows)[N], double key, bool lower, double beyond) noexcept
{
    for (const BetaGateRow& row : rows)
    {
        if (key < row.below)
        {
            return lower ? row.lower : row.upper;
        }
    }
    return beyond;
}

// Standard deviations of x(a+b) from the mode beyond which the fallback (the fraction with the Stirling-form prefactor)
// is inside the limit, for a sum s below kBetaUniformFullBand. Measured from the fallback itself against exact sums
// (5e4 to 3e5 points per band, x out to 8.5 standard deviations, every shape of at least 200 and any skew the uniform
// expansion serves; 3000 points per band and side above 1e10, 15000 at the widest skews): each width ends where the
// worst R beyond it is at most 0.5, and R > 1 occurs nowhere beyond any width. Below it the expansion serves, because
// the fallback is over the limit on neighbouring points (the worst R inside the width: 1.6 in the first row, 5e6 in the
// last).
template <Real T>
[[nodiscard]] T beta_uniform_width(T s, bool lower) noexcept
{
    constexpr BetaGateRow rows[] = {
        {1e5, 2.25, 0.75}, {2e5, 2.5, 1.0},   {3e5, 2.75, 1.0},  {5e5, 3.0, 1.25},  {1e6, 3.25, 1.5},
        {2e6, 3.5, 1.75},  {5e6, 3.75, 2.25}, {1e7, 3.75, 2.5},  {3e7, 4.0, 2.75},  {1e8, 4.25, 3.25},
        {3e8, 4.5, 3.5},   {1e9, 4.75, 3.75}, {1e10, 5.25, 4.5}, {1e11, 5.5, 4.75}, {1e12, 5.75, 5.0},
        {1e13, 6.25, 5.5}, {1e14, 6.5, 5.75},
    };
    // Single precision, measured the same way from betainc_large<float> against exact sums (R of its probe limit,
    // rtol 1e-4 + atol 1e-6; 20000 points per band and side), up to kBetaUniformFullBand.
    constexpr BetaGateRow rows32[] = {
        {1e5, 3.25, 1.5}, {3e5, 3.5, 1.75}, {1e6, 3.75, 2.25}, {3e6, 4.0, 2.5},  {1e7, 4.0, 2.5},
        {3e7, 4.0, 3.0},  {1e8, 4.25, 3.0}, {3e8, 4.25, 3.25}, {1e9, 4.25, 3.5}, {1e10, 4.75, 3.75},
    };
    return static_cast<T>((sizeof(T) > sizeof(float)) ? beta_gate(rows, static_cast<double>(s), lower, 0.0)
                                                      : beta_gate(rows32, static_cast<double>(s), lower, 0.0));
}

// Whether the uniform expansion serves I_x(a,b): both shapes large, a + b past the point where the fraction's rounding
// outgrows the gate, the skew of the shapes bounded (the coefficients grow with sqrt(b/a)), and x within the band of
// the mode a/(a+b), measured against each shape (the band is wider from a smaller shape of kBetaSmallShapeCap on; and,
// below kBetaUniformFullBand, within beta_uniform_width standard deviations of the mode).
template <Real T>
[[nodiscard]] bool beta_uniform_applies(T a, T b, T x) noexcept
{
    const T small = std::min(a, b);
    if (!(small >= kBetaUniformMin<T> && a + b >= kBetaUniformSum<T> && std::max(a, b) <= kBetaUniformSkew<T> * small))
    {
        return false;
    }
    // The distance from the mode is only compared here: the rounded offset x(a+b) − a serves (the exact one is formed
    // when the expansion is evaluated).
    const T s = a + b;
    const T gap = x * s - a;
    const T band = (small >= kBetaSmallShapeCap<T>) ? kBetaUniformBandWide<T> : kUniformBand<T>;
    if (!(std::abs(gap / a) < band && std::abs(gap / b) < band))
    {
        return false;
    }
    if (s >= kBetaUniformFullBand<T>)
    {
        return true;
    }
    // The standard deviation of x(a+b) is sqrt(a·b/(a+b)).
    const T width = beta_uniform_width(s, gap < static_cast<T>(0));
    return gap * gap * s < width * width * a * b;
}

// x far enough below the mode on the small shape's side, x(a+b) < a/2 for a ≤ b (or (1−x)(a+b) < b/2 for b < a): there
// the fraction converges in a few dozen terms and, with the Stirling-form prefactor, is as exact as the expansion below
// and several times cheaper (BGRAT takes over closer to the mode, where the fraction needs thousands of terms).
template <Real T>
[[nodiscard]] bool beta_in_small_shape_tail(T a, T b, T x) noexcept
{
    const T s = a + b;
    return (a <= b) ? (x * s < static_cast<T>(0.5) * a) : ((static_cast<T>(1) - x) * s < static_cast<T>(0.5) * b);
}

// Whether x lies so far from the mode, for shapes served by the incomplete-gamma expansion below, that the fallback
// (the fraction with the Stirling-form prefactor) is inside the limit; the expansion costs twice the fallback there.
// The distance is the large-deviation rate of the gamma variable y = large·τ (τ of the expansion): y − m − m·ln(y/m), m
// the small shape, near (y − m)²/2m, which measures the tail of any small shape where standard deviations do not. For
// the limit below, measured from the fallback itself against exact sums (6000 to 28000 points per band of the large
// shape up to 1e12, small shapes 1e-3 to 1000, any skew the expansion serves), the fallback is inside R = 0.5 from the
// rate given here on (the lower-side and upper-side rates of a row) and over the limit on neighbouring points below it
// (the worst R there: 1.0 to 2e8 down the table); from a large shape of 1e12 on the expansion serves everything.
template <Real T>
[[nodiscard]] bool beta_small_shape_far(T a, T b, T x) noexcept
{
    const bool a_small = a <= b;
    const T m = a_small ? a : b;
    const T large = a_small ? b : a;
    const T y = large * (a_small ? -crd::math::log1p(-x) : -crd::math::log(x));
    const T rate = y - m - m * crd::math::log(y / m);
    constexpr BetaGateRow rows[] = {
        {3e4, 3.0, 0.5},  {5e4, 3.0, 1.0},    {1e5, 4.0, 1.0},    {3e5, 5.0, 1.5},  {1e6, 6.0, 2.5},
        {3e6, 7.0, 3.0},  {1e7, 8.0, 4.0},    {3e7, 8.5, 4.5},    {1e8, 10.0, 6.0}, {3e8, 10.5, 7.0},
        {1e9, 12.0, 7.5}, {1e10, 13.5, 10.0}, {1e12, 18.0, 14.0},
    };
    // Single precision (R of its probe limit, 20000 points per band and side, small shapes 1e-3 to 100); from a large
    // shape of 1e8 on the fallback's rounding is over its limit at nearly every rate (R 1e6 and more at rates up to
    // 40).
    constexpr BetaGateRow rows32[] = {
        {1e4, 3.0, 0.5}, {3e4, 4.0, 1.5}, {1e5, 5.0, 2.0}, {3e5, 6.0, 2.5},
        {1e6, 7.0, 3.5}, {3e6, 7.5, 4.0}, {1e7, 9.0, 5.0}, {1e8, 11.0, 7.5},
    };
    // y below m is the lower tail of the gamma variable, the side where I_x is small when a is the small shape; the
    // mirrored case exchanges the two.
    const bool lower_side = (y < m) == a_small;
    const double bar =
        (sizeof(T) > sizeof(float))
            ? beta_gate(rows, static_cast<double>(large), lower_side, std::numeric_limits<double>::infinity())
            : beta_gate(rows32, static_cast<double>(large), lower_side, std::numeric_limits<double>::infinity());
    return static_cast<double>(rate) >= bar;
}

// Whether the 4-argument betainc serves I_x(a,b) by its own plain prefactor (the old path) although a shape is at or
// above kBetaStirlingOutright: far from the mode, where the plain exponent's rounding is scaled away by the tail. Both
// tests below are lower bounds of a distance, and their limits were measured for that path with an exact ln B against
// exact sums; the path does not serve beyond them.
//   Smaller shape at least 100, the distance in standard deviations of x(a+b) from the mode (x out to 9, 3000 to 90000
//   points per band and side): R at most 0.5 beyond the worst width of the three sum ranges below.
//   Smaller shape m below 100, larger L: with y0 = L·x when a is the small shape and L·(1−x) when b is, the gamma
//   variable y of the expansion lies in [y0, 1.06·y0] below the mode (x is under 0.1 there), and the large-deviation
//   rate y − m − m·ln(y/m) is at least d²/(2·max(y0, m)) for d = max(y0 − m, m − 1.06·y0): a bound with no logarithm,
//   so that the far inputs see the plain path at the plain path's cost. The limit per binary exponent of L (72000
//   points per band of L, m from 1e-3 to 100, t out to 66 standard deviations): R at most 0.5 from it on.
template <Real T>
[[nodiscard]] bool beta_plain_serves(T a, T b, T x) noexcept
{
    if constexpr (sizeof(T) < sizeof(double))
    {
        return false;
    }
    else
    {
        if (a >= static_cast<T>(100) && b >= static_cast<T>(100))
        {
            const T s = a + b;
            const T gap = x * s - a;
            T width_squared = std::numeric_limits<T>::infinity();
            if (s < static_cast<T>(1e12))
            {
                width_squared = (s < static_cast<T>(1e7))   ? static_cast<T>(22.5625) // 4.75 sd
                                : (s < static_cast<T>(1e9)) ? static_cast<T>(36)      // 6 sd
                                                            : static_cast<T>(49);     // 7 sd
            }
            return gap * gap * s >= width_squared * a * b;
        }
        const bool a_small = a <= b;
        const T m = a_small ? a : b;
        if (!(m > static_cast<T>(0)))
        {
            return false;
        }
        constexpr double limits[] = {0.0, 1.5, 1.5, 2.5, 3.5,  3.5,  4.0,  4.0,  5.0,  6.0,  6.0,  6.5,  6.5,  8.0, 8.0,
                                     8.0, 9.5, 9.5, 9.5, 11.0, 11.0, 11.0, 11.0, 14.0, 14.0, 14.0, 14.0, 14.0, 14.0};
        const T large = a_small ? b : a;
        const int exponent = static_cast<int>(std::bit_cast<std::uint64_t>(large) >> 52) - 1023;
        if (exponent < 10 || exponent > 38)
        {
            return false;
        }
        const T y0 = large * (a_small ? x : static_cast<T>(1) - x);
        const T d = std::max(std::max(y0 - m, m - static_cast<T>(1.06) * y0), static_cast<T>(0));
        return d * d >= static_cast<T>(2) * limits[exponent - 10] * std::max(y0, m);
    }
}

// Whether the expansion in incomplete gamma functions serves I_x(a,b): the larger shape at least kBetaSmallShapeBig and
// either the smaller one below kBetaSmallShapeCap with the larger at least half its square, or the shapes beyond the
// uniform expansion's skew limit with the larger at least the smaller one squared.
template <Real T>
[[nodiscard]] bool beta_small_shape_applies(T a, T b) noexcept
{
    const T small = std::min(a, b);
    const T big = std::max(a, b);
    const bool converges = (small < kBetaSmallShapeCap<T>)
                               ? (big + big >= small * small)
                               : (big >= small * small && big > kBetaUniformSkew<T> * small);
    return small > kBetaSmallShapeFloor<T> && big >= kBetaSmallShapeBig<T> && converges && !std::isinf(big);
}

// Whether a single-precision call of the incomplete-gamma expansion is evaluated in double and narrowed (see
// kBetaFloatSmallShapeMin): its float sum is inside the probe limit only for a smaller shape in [1e-3, kGammaShapeBig).
template <Real T>
[[nodiscard]] bool beta_small_shape_in_double(T a, T b) noexcept
{
    const T small = std::min(a, b);
    return small >= kGammaShapeBig<T> || small < kBetaFloatSmallShapeMin<T>;
}

// Whether one of the two expansions can serve shapes a and b at all (they start no lower).
template <Real T>
[[nodiscard]] bool beta_expansions_possible(T a, T b) noexcept
{
    return a + b >= kBetaUniformSum<T> || std::max(a, b) >= kBetaSmallShapeBig<T>;
}

template <Real T>
[[nodiscard]] UniformTail<T> beta_uniform_tail(T a, T b, T x) noexcept
{
    const T s = a + b;
    const T gap = beta_mode_offset(a, b, x);
    const T exponent = -(a * log1p_deficit_small(gap / a) + b * log1p_deficit_small(-gap / b));
    const T normal = stirling_correction(s) - stirling_correction(a) - stirling_correction(b);
    const bool lower = gap < static_cast<T>(0);
    const T scaled = crd::math::exp(normal) * uniform_scaled_tail(beta_uniform_series(a, b), s, exponent, lower);
    return {exponent, scaled, lower};
}

// I_x(a,b) and its complement, each with its own relative accuracy.
template <Real T>
struct BetaSplit
{
    T lower;
    T upper;
};

template <Real T>
struct SmallShapeSeries
{
    T c[kSmallShapeTerms + 1];
};

// ψ(τ) = ln((1−e^{−τ})/τ) = −τ/2 + Σ_{n≥1} B_{2n}·τ^{2n}/(2n·(2n)!), the Bernoulli-number series of ln(sinh z / z)
// with z = τ/2 plus −τ/2; the expansion below raises e^{ψ} to the power a−1.
template <Real T>
[[nodiscard]] constexpr SmallShapeSeries<T> log_decay_series() noexcept
{
    constexpr double bernoulli[8] = {1.0 / 6.0,  -1.0 / 30.0,    1.0 / 42.0, -1.0 / 30.0,
                                     5.0 / 66.0, -691.0 / 2730.0, 7.0 / 6.0,  -3617.0 / 510.0};
    SmallShapeSeries<T> psi{};
    psi.c[1] = static_cast<T>(-0.5);
    double factorial = 1.0;
    for (int n = 1; n <= 8; ++n)
    {
        factorial *= static_cast<double>(2 * n - 1) * static_cast<double>(2 * n);
        psi.c[2 * n] = static_cast<T>(bernoulli[n - 1] / (static_cast<double>(2 * n) * factorial));
    }
    return psi;
}

template <Real T>
inline constexpr SmallShapeSeries<T> kLogDecaySeries = log_decay_series<T>();

// I_x(a,b) for a ≤ b with b large, from the substitution τ = −ln(1−x) (cf. DiDonato & Morris 1992, BGRAT): with
// x = 1 − e^{−τ},
//   B(a,b)·I_x = ∫_0^τx (1−e^{−τ})^{a−1} e^{−bτ} dτ,   (1−e^{−τ})^{a−1} = τ^{a−1}·exp((a−1)ψ(τ)) = τ^{a−1}·Σ c_k τ^k,
// and each power integrates to an incomplete gamma function of y = b·τx:
//   I_x = Γ(a+b)/(Γ(b)·b^a) · Σ_k w_k·P(a+k, y),   w_k = c_k·(a)_k / b^k,
// and the same with Q for 1 − I_x (the sum of the full integral is one). The terms fall like (a²/2b)^k/k!, so for
// a² ≤ b they reach double precision within sixteen (two or three when b is far above a²). P(a+k, y) comes from one
// direct evaluation and the recurrences P(a+k, y) = P(a+k+1, y) + t_k and Q(a+k+1, y) = Q(a+k, y) + t_k,
// t_k = y^{a+k}e^{−y}/Γ(a+k+1), whose terms all add: downward from P(a+n, y) when y ≤ a (the lower tail is the small
// one), upward from Q(a, y) when y > a.
template <Real T>
[[nodiscard]] BetaSplit<T> beta_small_shape_ordered(T a, T b, T tau) noexcept
{
    constexpr int most = kSmallShapeTerms;
    const T y = b * tau;
    // c_k: Σ c_k τ^k = exp((a−1)·ψ(τ)), from E' = (a−1)·ψ'·E, and the weights w_k = c_k·(a)_k / b^k, cut at the first
    // weight past the peak that is below the working precision.
    T c[most + 1] = {};
    T weight[most + 1] = {};
    c[0] = static_cast<T>(1);
    weight[0] = static_cast<T>(1);
    T rising = a;                    // (a)_k
    T scale = static_cast<T>(1) / b; // b^{−k}
    const T negligible = std::numeric_limits<T>::epsilon() / static_cast<T>(16);
    int n = most;
    for (int k = 1; k <= most; ++k)
    {
        T acc = static_cast<T>(0);
        for (int j = 1; j <= k; ++j)
        {
            acc += static_cast<T>(j) * kLogDecaySeries<T>.c[j] * c[k - j];
        }
        c[k] = (a - static_cast<T>(1)) * acc / static_cast<T>(k);
        weight[k] = c[k] * rising * scale;
        rising *= a + static_cast<T>(k);
        scale /= b;
        if (std::abs(weight[k]) < negligible && std::abs(weight[k]) <= std::abs(weight[k - 1]))
        {
            n = k;
            break;
        }
    }
    const T gln = lgamma(a);
    const T prefactor = (a >= kGammaShapeBig<T>) ? gamma_prefactor<true>(a, y, gln) : gamma_prefactor<false>(a, y, gln);
    T density[most + 1] = {}; // t_k = y^{a+k}·e^{−y}/Γ(a+k+1)
    density[0] = prefactor / a;
    for (int k = 1; k < n; ++k)
    {
        density[k] = density[k - 1] * y / (a + static_cast<T>(k));
    }
    T lower_sum = static_cast<T>(0);
    T upper_sum = static_cast<T>(0);
    if (y <= a)
    {
        T p = gammainc_p(a + static_cast<T>(n), y);
        lower_sum = weight[n] * p;
        upper_sum = weight[n] * (static_cast<T>(1) - p);
        for (int k = n - 1; k >= 0; --k)
        {
            p += density[k];
            lower_sum += weight[k] * p;
            upper_sum += weight[k] * (static_cast<T>(1) - p);
        }
    }
    else
    {
        T q = gammainc_q(a, y, gln);
        for (int k = 0; k <= n; ++k)
        {
            upper_sum += weight[k] * q;
            lower_sum += weight[k] * (static_cast<T>(1) - q);
            if (k < n)
            {
                q += density[k];
            }
        }
    }
    // Γ(a+b)/(Γ(b)·b^a) = exp((a+b−½)·ln(1+a/b) − a + S(a+b) − S(b)) by Stirling's formula (S the correction series).
    const T front = crd::math::exp((a + b - static_cast<T>(0.5)) * crd::math::log1p(a / b) - a +
                                   stirling_correction(a + b) - stirling_correction(b));
    return {front * lower_sum, front * upper_sum};
}

// The same for either shape being the small one: I_x(a,b) = 1 − I_{1−x}(b,a), with τ = −ln x on that side.
template <Real T>
[[nodiscard]] BetaSplit<T> beta_small_shape(T a, T b, T x) noexcept
{
    if (a <= b)
    {
        return beta_small_shape_ordered(a, b, -crd::math::log1p(-x));
    }
    const BetaSplit<T> mirrored = beta_small_shape_ordered(b, a, -crd::math::log(x));
    return {mirrored.upper, mirrored.lower};
}

// The small side of I_x(a,b) with the Stirling-form prefactor: the fraction's own value, which is I_x when x lies below
// the switch point (a+1)/(a+b+2) (`direct`) and 1 − I_x above it.
//
// The plain prefactor carries the rounding of its exponent, a few ulps of a term of size s (with lbeta of gamma.hpp for
// ln B; a raw sum of three lgamma terms adds a few ulps of s·ln s). That is harmless for the relative accuracy of the
// value itself while max(a,b) < kBetaStirlingOutright, so from there on the Stirling form is used outright (and the
// log-beta is not read). Below it, a value that the caller subtracts from 1 has that rounding scaled by
// value/(1−value); only where that scale is extreme is the Stirling form computed as well (beta_cancels).
template <Real T>
CRD_NOINLINE T beta_stable_side(T a, T b, T x, bool direct) noexcept
{
    if constexpr (sizeof(T) < sizeof(double))
    {
        // Within 3 standard deviations of x(a+b) of the mode (5 from a sum of 1e7, where the float rounding of the
        // shapes grows) the float fraction's rounding reaches the probe limit (R 1.4 to 4.8 at a large shape of 3000 to
        // 180000 against a smaller one of a few hundred). The fraction then runs in double to float's tolerance, so
        // that the sweep count is float's; farther out the float fraction stays near the limit (40 of 230000 points
        // over it in a sweep, R at most 5.7, base also over at all of them). The distance is formed in double: x·(a+b)
        // − a at a float's rounding of a is a difference of 1e9-sized terms a few hundred apart.
        const double sum = static_cast<double>(a) + static_cast<double>(b);
        const double gap = static_cast<double>(x) * sum - static_cast<double>(a);
        const double reach = (sum < 1e7) ? 9.0 : 25.0; // the distance in standard deviations, squared
        if (gap * gap * sum < reach * static_cast<double>(a) * static_cast<double>(b))
        {
            const double xd = static_cast<double>(x);
            const double tol = static_cast<double>(kCfEps<T>);
            const double cf = direct ? betacf<double>(static_cast<double>(a), static_cast<double>(b), xd, tol)
                                     : betacf<double>(static_cast<double>(b), static_cast<double>(a), 1.0 - xd, tol);
            return static_cast<T>(static_cast<double>(beta_prefactor_stable(a, b, x)) * cf /
                                  static_cast<double>(direct ? a : b));
        }
    }
    const T cf = direct ? betacf(a, b, x) : betacf(b, a, static_cast<T>(1) - x);
    return beta_prefactor_stable(a, b, x) * cf / (direct ? a : b);
}

// Whether a fraction value that is about to be subtracted from 1 needs the Stirling-form prefactor (the residual of the
// inverse does that subtraction; the 4-argument betainc leaves it to the ln B of its caller). The rounding of the plain
// prefactor's exponent is scaled by value/(1−value) in 1 − value. With the lbeta of gamma.hpp it stays inside the gate
// everywhere measured (R ≤ 0.9 down to 1 − x = 1e-14); a raw sum of three lgamma terms adds a few ulps of s·ln s, which
// that scale carries to R = 6 at value·(a+b)/(1−value) = 3e3 and R = 60 at 1e5. The Stirling form (which costs about as
// much as the rest of the call) is taken from the latter, the extreme: below it the plain prefactor costs nothing.
template <Real T>
inline constexpr T kBetaCancelRatio = static_cast<T>(sizeof(T) > sizeof(float) ? 1e5 : 1e3);
template <Real T>
[[nodiscard]] bool beta_cancels(T a, T b, T value) noexcept
{
    return value * (a + b) > (static_cast<T>(1) - value) * kBetaCancelRatio<T> && std::max(a, b) >= kStirlingMin<T>;
}

// I_x(1,b) = 1 − (1−x)^b and I_x(a,1) = x^a in closed form: at a shape near the largest double the fraction's
// coefficients overflow. Out of line, so that the paths of the other shapes stay as compact as they were.
template <Real T>
CRD_NOINLINE T beta_shape_one(T a, T b, T x) noexcept
{
    return (a == static_cast<T>(1)) ? -crd::math::expm1(b * crd::math::log1p(-x)) : crd::math::pow(x, a);
}

// The root of I_x(a,b) = p for a shape of one (the other positive and finite), in closed form; out of line as above.
template <Real T>
CRD_NOINLINE T beta_inverse_shape_one(T a, T b, T p) noexcept
{
    if (b == static_cast<T>(1))
    {
        return crd::math::pow(p, static_cast<T>(1) / a); // I_x(a,1) = x^a
    }
    const T ln_survival = (p > static_cast<T>(0.5)) ? crd::math::log(static_cast<T>(1) - p) : crd::math::log1p(-p);
    return -crd::math::expm1(ln_survival / b); // I_x(1,b) = 1 − (1−x)^b
}

// I_x(a,b) for max(a,b) ≥ kBetaStirlingOutright: the expansions of huge shapes, else the Stirling-form prefactor
// outright. Out of line, so that the path of the moderate shapes below stays as compact as it was.
template <Real T>
CRD_NOINLINE T betainc_large(T a, T b, T x) noexcept
{
    if (a == static_cast<T>(1) || b == static_cast<T>(1))
    {
        return beta_shape_one(a, b, x);
    }
    if (beta_expansions_possible(a, b))
    {
        if (beta_small_shape_applies(a, b))
        {
            if (!beta_in_small_shape_tail(a, b, x) && !beta_small_shape_far(a, b, x))
            {
                if constexpr (sizeof(T) < sizeof(double))
                {
                    if (beta_small_shape_in_double(a, b))
                    {
                        return static_cast<T>(betainc_large<double>(static_cast<double>(a), static_cast<double>(b),
                                                                    static_cast<double>(x)));
                    }
                }
                return beta_small_shape(a, b, x).lower;
            }
        }
        else
        {
            if constexpr (sizeof(T) < sizeof(double))
            {
                if (a + b >= kBetaUniformFullBand<T>) // neither the expansion nor the fallback is usable in float there
                {
                    return static_cast<T>(
                        betainc_large<double>(static_cast<double>(a), static_cast<double>(b), static_cast<double>(x)));
                }
            }
            if (beta_uniform_applies(a, b, x))
            {
                return uniform_side(beta_uniform_tail(a, b, x), true);
            }
        }
    }
    const bool direct = x < (a + static_cast<T>(1)) / (a + b + static_cast<T>(2));
    const T value = beta_stable_side(a, b, x, direct);
    if constexpr (sizeof(T) < sizeof(double))
    {
        if (!direct && value > kSingleComplement<T>) // the complement of a value above 0.985, in double (as betainc)
        {
            return static_cast<T>(
                betainc_large<double>(static_cast<double>(a), static_cast<double>(b), static_cast<double>(x)));
        }
    }
    return direct ? value : static_cast<T>(1) - value;
}

// I_x(a,b) outside the plain path of the 4-argument betainc: a negative or NaN shape (NaN), or one at or above
// kBetaStirlingOutright.
template <Real T>
CRD_NOINLINE T betainc_beyond(T a, T b, T x) noexcept
{
    if (std::min(a, b) < static_cast<T>(0)) // outside the domain
    {
        return std::numeric_limits<T>::quiet_NaN();
    }
    if (std::max(a, b) >= kBetaStirlingOutright<T>)
    {
        return betainc_large(a, b, x);
    }
    return std::numeric_limits<T>::quiet_NaN(); // a NaN shape
}

// The small side of I_x(a,b) for the shapes that need the Stirling form: the fraction's own value, which is I_x when x
// lies below the switch point (`direct`) and 1 − I_x above it. `want_tail` says that the caller needs 1 − I_x, so it
// will subtract the value from 1 when `direct`; `precise` asks for the cancellation of that subtraction to be avoided
// (beta_cancels).
template <Real T>
CRD_NOINLINE T beta_small_side_shaped(T a, T b, T x, T lbeta_ab, bool want_tail, bool direct, bool precise) noexcept
{
    if (beta_expansions_possible(a, b))
    {
        if (beta_small_shape_applies(a, b))
        {
            if (!beta_in_small_shape_tail(a, b, x) && !beta_small_shape_far(a, b, x))
            {
                if constexpr (sizeof(T) < sizeof(double))
                {
                    if (beta_small_shape_in_double(a, b))
                    {
                        return static_cast<T>(beta_small_side_shaped<double>(
                            static_cast<double>(a), static_cast<double>(b), static_cast<double>(x),
                            static_cast<double>(lbeta_ab), want_tail, direct, precise));
                    }
                }
                const BetaSplit<T> split = beta_small_shape(a, b, x);
                return direct ? split.lower : split.upper;
            }
        }
        else
        {
            if constexpr (sizeof(T) < sizeof(double))
            {
                if (a + b >= kBetaUniformFullBand<T>) // as in betainc_large
                {
                    return static_cast<T>(beta_small_side_shaped<double>(
                        static_cast<double>(a), static_cast<double>(b), static_cast<double>(x),
                        static_cast<double>(lbeta_ab), want_tail, direct, precise));
                }
            }
            if (beta_uniform_applies(a, b, x))
            {
                return uniform_side(beta_uniform_tail(a, b, x), direct); // I_x when direct, else 1 − I_x
            }
        }
    }
    if (std::max(a, b) >= kBetaStirlingOutright<T>)
    {
        return beta_stable_side(a, b, x, direct);
    }
    const T bt = beta_prefactor_plain(a, b, x, lbeta_ab);
    const T cf = direct ? betacf(a, b, x) : betacf(b, a, static_cast<T>(1) - x);
    const T scale = direct ? a : b;
    const T value = bt * cf / scale;
    return (precise && direct == want_tail && beta_cancels(a, b, value)) ? beta_prefactor_stable(a, b, x) * cf / scale
                                                                         : value;
}

// The small side of I_x(a,b), for 0 < x < 1: I_x when `direct` (x below the switch point (a+1)/(a+b+2)), else 1 − I_x.
// The plain prefactor serves every shape below kBetaStirlingOutright unless `precise` asks for the Stirling form's
// accuracy.
template <Real T>
[[nodiscard]] T beta_small_side(T a, T b, T x, T lbeta_ab, bool want_tail, bool direct, bool precise) noexcept
{
    const T big = std::max(a, b);
    if (big >= kBetaStirlingOutright<T> || (precise && big >= kStirlingMin<T>))
    {
        return beta_small_side_shaped(a, b, x, lbeta_ab, want_tail, direct, precise);
    }
    const T bt = beta_prefactor_plain(a, b, x, lbeta_ab);
    return direct ? bt * betacf(a, b, x) / a : bt * betacf(b, a, static_cast<T>(1) - x) / b;
}

// ln I_x(a,b) for the lower tail of the beta inverse: the logarithm of the fraction's own value plus the logarithm of
// the plain prefactor (so that neither underflows), and past the switch point the logarithm of the complement of the
// other fraction. The exponent of the plain prefactor is accurate to a few ulps of its largest term, which moves the
// root of an inverse by that much divided by the shape.
template <Real T>
[[nodiscard]] T beta_log_lower(T a, T b, T x, T lbeta_ab) noexcept
{
    const T ln_prefactor = -lbeta_ab + a * crd::math::log(x) + b * crd::math::log1p(-x);
    if (x < (a + static_cast<T>(1)) / (a + b + static_cast<T>(2)))
    {
        return ln_prefactor + crd::math::log(betacf(a, b, x) / a);
    }
    return crd::math::log1p(-crd::math::exp(ln_prefactor) * betacf(b, a, static_cast<T>(1) - x) / b);
}

// Far lower tail of the beta inverse for a, b ≥ 1: I_x(a,b) and its density sink toward underflow as p does, and the
// Halley step on I itself, clamped to about 2x/(a−1), then needs one step per e-fold of distance from its start.
// Newton's method is run on ln I_x − ln p instead, which is increasing and concave (the density x^{a−1}(1−x)^{b−1}/B is
// log-concave for a, b ≥ 1, and integration preserves that, Prékopa 1973), so an iterate below the root stays below it
// and converges monotonically, quadratically at the end. The start is the power law of the leading term: for b ≥ 1,
// (1−t)^{b−1} ≤ 1 gives I_x ≤ x^a/(a·B), so x = (p·a·B)^{1/a} lies below the root; the caller's estimate replaces it
// when that estimate is also below the root and larger. The iteration stops where I is within 5% of p, from where the
// caller's Halley loop finishes with its own accuracy at every shape.
template <Real T>
CRD_NOINLINE T betainc_inv_log_lower(T a, T b, T p, T lbeta_ab, T estimate, bool estimate_below) noexcept
{
    const T am1 = a - static_cast<T>(1);
    const T bm1 = b - static_cast<T>(1);
    const T ln_p = crd::math::log(p);
    const T power_law = crd::math::exp((ln_p + crd::math::log(a) + lbeta_ab) / a);
    T x = (estimate_below && estimate > power_law) ? estimate : power_law;
    for (int j = 0; j < 60; ++j)
    {
        if (!(x > static_cast<T>(0)))
        {
            return static_cast<T>(0);
        }
        const T ln_tail = beta_log_lower(a, b, x, lbeta_ab);
        const T gap = ln_tail - ln_p;
        if (!(gap < static_cast<T>(-0.05)))
        {
            break;
        }
        const T ln_density = am1 * crd::math::log(x) + bm1 * crd::math::log1p(-x) - lbeta_ab;
        const T previous = x;
        x += -gap / crd::math::exp(ln_density - ln_tail);
        if (!(x < static_cast<T>(1)))
        {
            x = static_cast<T>(0.5) * (previous + static_cast<T>(1));
        }
    }
    return x;
}
// Whether a single-precision beta inverse is evaluated in double and rounded once (kSingleDoubleShape): a smaller shape
// in [kSingleDoubleFloor, 1/4), and below the floor the tail whose root is representable, the probability near one when
// a is the small shape (near zero when b is), with the larger shape below kSingleDoubleLarge: there the root is a
// moderate x, and float's rounding of I divided by the density is R 31 to 860 (base: R below 1e-5).
template <Real T>
[[nodiscard]] bool beta_inverse_in_double(T a, T b, T p) noexcept
{
    const T small = std::min(a, b);
    if (!(small < kSingleDoubleShape<T>))
    {
        return false;
    }
    if (small >= kSingleDoubleFloor<T>)
    {
        return true;
    }
    const bool near_end = (a <= b) ? (p > static_cast<T>(0.5)) : (p < static_cast<T>(0.5));
    return small > static_cast<T>(0) && near_end && std::max(a, b) < kSingleDoubleLarge<T>;
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
    // A negative or NaN shape is outside the domain; the large shapes take the Stirling forms and the expansions, unless
    // far enough from the mode that this path's plain prefactor is exact (detail::beta_plain_serves).
    if (!(a >= static_cast<T>(0) && b >= static_cast<T>(0) && a < detail::kBetaStirlingOutright<T> &&
          b < detail::kBetaStirlingOutright<T>) &&
        !detail::beta_plain_serves(a, b, x))
    {
        return detail::betainc_beyond(a, b, x);
    }
    const T bt = detail::beta_prefactor_plain(a, b, x, lbeta_ab);
    if (x < (a + static_cast<T>(1)) / (a + b + static_cast<T>(2)))
    {
        return bt * detail::betacf(a, b, x) / a;
    }
    const T cf = detail::betacf(b, a, static_cast<T>(1) - x);
    const T value = bt * cf / b;
    if constexpr (sizeof(T) < sizeof(double))
    {
        // 1 − value of a value above 0.985 has the single-precision rounding of value (a few 1e-7) scaled by 500 or
        // more.
        if (value > detail::kSingleComplement<T>)
        {
            const double da = static_cast<double>(a);
            const double db = static_cast<double>(b);
            return static_cast<T>(betainc<double>(da, db, static_cast<double>(x), lbeta(da, db)));
        }
    }
    return static_cast<T>(1) - value;
}

// Regularized incomplete beta I_x(a,b).
template <Real T>
[[nodiscard]] T betainc(T a, T b, T x) noexcept
{
    if (std::isnan(a) || std::isnan(b) || std::isnan(x))
    {
        return std::numeric_limits<T>::quiet_NaN();
    }
    if (std::max(a, b) >= detail::kBetaStirlingOutright<T>)
    {
        // From this shape on the Stirling-form path serves, whose prefactor needs no ln B at all.
        if (x <= static_cast<T>(0))
        {
            return static_cast<T>(0);
        }
        if (x >= static_cast<T>(1))
        {
            return static_cast<T>(1);
        }
        return detail::betainc_beyond(a, b, x);
    }
    return betainc(a, b, x, lbeta(a, b));
}

// Inverse of I_·(a,b): returns x with I_x(a,b) = p, with PRECOMPUTED lbeta (amortises the inner Newton betainc
// calls). Starting point: the exact root when a = 1 or b = 1 (I_x(a,1) = x^a, I_x(1,b) = 1 − (1−x)^b), else A&S 26.5.22
// when a,b ≥ 1, otherwise a two-piece estimate from the end-point power laws of I_x. Refinement: Halley steps on
// I_x − p with density x^{a−1}(1−x)^{b−1}/B(a,b) and curvature ratio (a−1)/x − (b−1)/(1−x), damped when they would
// overshoot. For p > ½ the steps are taken on the upper tail, (1−p) − (1−I_x), whose rounding is relative to 1−p
// instead of to 1. A bracket [lo, hi] of the root is kept from the sign of every residual (I_x is increasing); a step
// that leaves it is replaced by a bisection, so the iteration cannot stall far from the root the way an unguarded
// Newton step does on a steep tail. The loop converges to ~1e-13 in ≤ ~5 iterations from the starting point in the
// bulk — well inside the 1e-7 ppf gate — and the bracket only takes over in the extreme tails.
template <Real T>
[[nodiscard]] T betainc_inv(T a, T b, T p) noexcept;

template <Real T>
[[nodiscard]] T betainc_inv(T a, T b, T p, T lbeta_ab) noexcept
{
    if constexpr (sizeof(T) < sizeof(double))
    {
        if (detail::beta_inverse_in_double(a, b, p))
        {
            // The double inverse forms its own ln B: a single-precision one is the error being avoided.
            return static_cast<T>(
                betainc_inv<double>(static_cast<double>(a), static_cast<double>(b), static_cast<double>(p)));
        }
    }
    // gate is 1e-7 on ppf; stop once relative step is below this (fewer iters), but never below the working precision
    const T tol = std::max(static_cast<T>(1e-12), static_cast<T>(8) * std::numeric_limits<T>::epsilon());
    if (a < static_cast<T>(0) || b < static_cast<T>(0))
    {
        return std::numeric_limits<T>::quiet_NaN();
    }
    if (p <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    if (p >= static_cast<T>(1))
    {
        return static_cast<T>(1);
    }
    const T q = static_cast<T>(1) - p;
    const bool upper = p > static_cast<T>(0.5);
    // The complement's cancellation matters only once a tail is small; the bulk iterates on the plain prefactor.
    const bool precise = std::min(p, q) < static_cast<T>(1e-4);
    const T am1 = a - static_cast<T>(1);
    const T bm1 = b - static_cast<T>(1);
    T x = static_cast<T>(0);
    bool analytic_start = false; // the A&S start of shapes at least 1: its far tails can lie many e-folds from the root
    if ((b == static_cast<T>(1) && a > static_cast<T>(0) && !std::isinf(a)) ||
        (a == static_cast<T>(1) && b > static_cast<T>(0) && !std::isinf(b)))
    {
        return detail::beta_inverse_shape_one(a, b, p); // the root in closed form
    }
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
        analytic_start = std::isfinite(a + b);
    }
    else
    {
        const T ab = a + b;
        const T low_mass = crd::math::exp(a * crd::math::log(a / ab)) / a;
        const T high_mass = crd::math::exp(b * crd::math::log(b / ab)) / b;
        const T total = low_mass + high_mass;
        x = (p < low_mass / total) ? crd::math::pow(a * total * p, static_cast<T>(1) / a)
                                   : static_cast<T>(1) - crd::math::pow(b * total * q, static_cast<T>(1) / b);
    }
    T lo = static_cast<T>(0);
    T hi = static_cast<T>(1);
    for (int j = 0; j < 100; ++j)
    {
        if (x <= static_cast<T>(0) || x >= static_cast<T>(1))
        {
            return x;
        }
        const T one_minus_x = static_cast<T>(1) - x;
        // One expansion serves both tails: the small side is I_x below the switch point and 1 − I_x above it.
        const bool direct = x < (a + static_cast<T>(1)) / (a + b + static_cast<T>(2));
        const T small_side = detail::beta_small_side(a, b, x, lbeta_ab, upper, direct, precise);
        const T residual = upper ? q - (direct ? static_cast<T>(1) - small_side : small_side)
                                 : (direct ? small_side : static_cast<T>(1) - small_side) - p;
        if (residual > static_cast<T>(0))
        {
            hi = x; // I_x is already above p: the root lies below x
        }
        else if (residual < static_cast<T>(0))
        {
            lo = x;
        }
        else
        {
            // An exact hit, or a residual that is not a number: a NaN argument passes its NaN on; on finite arguments
            // the iterate is the best answer there is (the forward function's own range ends there).
            return (residual == static_cast<T>(0) || !std::isnan(a + b + p)) ? x : residual;
        }
        if (j == 0 && analytic_start && std::min(p, q) < detail::kBetaLogTailProbability<T>)
        {
            // The tail that is being solved: a start more than 64 times from its target is out of the Halley
            // iteration's reach, and Newton's method on ln I (below the root, from the tail's own side) brings it back.
            const T target = upper ? q : p;
            const T tail = target + (upper ? -residual : residual);
            if (!(tail < static_cast<T>(64) * target && tail * static_cast<T>(64) > target))
            {
                x = upper ? static_cast<T>(1) -
                                detail::betainc_inv_log_lower(b, a, q, lbeta_ab, static_cast<T>(1) - x, tail < q)
                          : detail::betainc_inv_log_lower(a, b, p, lbeta_ab, x, tail < p);
                continue;
            }
        }
        const T density = crd::math::exp(am1 * crd::math::log(x) + bm1 * crd::math::log(one_minus_x) - lbeta_ab);
        const T newton = residual / density;
        const T curvature = am1 / x - bm1 / one_minus_x;
        const T step =
            newton / (static_cast<T>(1) - static_cast<T>(0.5) * std::min(static_cast<T>(1), newton * curvature));
        const T next = x - step;
        if (std::abs(step) < tol * next)
        {
            if (j > 0)
            {
                return next;
            }
            x = next; // an exact start still takes one confirming pass
            continue;
        }
        // Keep the iterate inside the bracket; when the Halley step leaves it, bisect (geometrically once the lower
        // end is known, so a root many orders of magnitude below the upper end is still reached in few steps).
        if (next > lo && next < hi)
        {
            x = next;
        }
        else
        {
            x = (lo > static_cast<T>(0)) ? crd::math::sqrt(lo) * crd::math::sqrt(hi) : static_cast<T>(0.5) * hi;
        }
    }
    return x;
}

// Inverse of I_·(a,b): returns x with I_x(a,b) = p.
template <Real T>
[[nodiscard]] T betainc_inv(T a, T b, T p) noexcept
{
    if constexpr (sizeof(T) < sizeof(double))
    {
        if (detail::beta_inverse_in_double(a, b, p))
        {
            return static_cast<T>(
                betainc_inv<double>(static_cast<double>(a), static_cast<double>(b), static_cast<double>(p)));
        }
    }
    return betainc_inv(a, b, p, lbeta(a, b));
}

} // namespace crd::hesap::special
