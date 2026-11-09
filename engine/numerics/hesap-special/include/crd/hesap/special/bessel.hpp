#pragma once

// crd-hesap-special v12-b — Bessel & Hankel functions (real order, real argument), gold-standard.
//
// Cylindrical J_ν, Y_ν, I_ν, K_ν (+ derivatives) by the Steed/Temme method (Temme 1975, J. Comp. Phys. 19 for K; Temme
// 1976, J. Comput. Phys. 21 for Y; Barnett, Feng, Steed and Goldfarb 1974; Thompson and Barnett 1986; DLMF chapter 10),
// organised the way Numerical Recipes §6.7 organises `bessjy`/`bessik`. The code text below is Cerid's own, written
// from those papers; the Chebyshev table of bes_temme_gammas and the step-count rule of bes_base_steps/bes_jy_steps
// keep their Numerical Recipes origin. CF1 (Lentz continued fraction) gives the logarithmic derivative, a downward
// recurrence carries it to a base order μ ∈ [−½,½], then Temme's power series (x small) or Steed's CF2 (x large) gives
// the base values, which the Wronskian normalises, and an upward recurrence in order finishes. No fragile
// rational-coefficient tables, only continued fractions and a tiny Chebyshev fit for the Γ-ratios near μ=0. Where that
// machinery fails, other published methods take over: below x = 1e-3 J, J', I and I' are summed from their ascending
// series, and from x = 1e3 J, Y, J' and Y' come from Hankel's expansion (DLMF 10.17) at the base order μ and μ+1
// carried up to ν by the recurrence in order (DLMF 10.6.1), because the continued fraction needs about x steps; above
// order 2e5 (where that recurrence would need ν steps) from Debye's uniform expansion (DLMF 10.19.6) with its phase in
// double-double arithmetic, started a margin below x and carried up to ν (from x = 1e20, where neither it nor Hankel's
// expansion at the order serves, J, Y and their slopes are NaN). A negative order is reflected (DLMF 10.2.3,
// 10.27.2), with sin νπ and cos νπ exact. Accurate to ~1e-14 over the whole range. Spherical j_n/y_n (closed-form
// recurrence), Hankel H^(1,2) = J ± iY. Internals compute in f64; the public templates cast for f32. Gated vs
// scipy.special (jv/yv/iv/kv/spherical_jn/yn) to <1e-13.

#include <crd/core/platform.hpp>       // CRD_NOINLINE
#include <crd/hesap/special/gamma.hpp> // Real concept
#include <crd/math/cmath.hpp>

#include <bit>
#include <complex>
#include <limits>

namespace crd::hesap::special
{
namespace detail
{
inline constexpr double kBesPi = 3.14159265358979323846264338327950288;
inline constexpr double kBesRelTol = 1.0e-16; // convergence test of the series and fractions; small-argument limit
inline constexpr double kBesTiny = 1.0e-300;  // floor that keeps Lentz's tails and the unnormalised seed off zero
inline constexpr double kBesTemmeLimit = 2.0; // below: Temme's series; from here on: Steed's CF2
inline constexpr int kBesIterCap = 10000;
// Acceptance of the Hankel asymptotic for J: the smallest term kept bounds its truncation error, and the largest term
// met bounds the cancellation of the two sums (a term of size g costs about g·ε of relative accuracy).
inline constexpr double kBesAsympFloor = 1.0e-13;
inline constexpr double kBesAsympGrowth = 1.0e3;
// The expansion is cut after at most this many terms, or as soon as a term falls below 2^-56 (an eighth of the
// rounding of 1, the size of the sums P and Q).
inline constexpr int kBesHankelTerms = 60;
inline constexpr double kBesHankelDone = 0x1p-56;
// The recurrence in order that starts from Hankel's expansion at the base order runs at most this many steps.
inline constexpr double kBesForwardSteps = 2.0e5;
// Above that order, J and Y for x ≥ ν come from Debye's uniform expansion (DLMF 10.19.6) of kBesDebyeOrder + 1 terms,
// started kBesDebyeMargin·ν^{1/3} below x (where its terms fall by a factor of about 90 each; closer to the turning
// point it needs Airy functions) and carried up to the order by the recurrence in order. Its phase is formed in
// double-double arithmetic, which holds it to 1e-16 up to x = kBesDebyeLimit; beyond, Hankel's expansion at the order
// itself serves while it is reliable (its hump is e^{ν²/2x}). The phase carries about 1e-32·x absolute (1e-17 at
// x = kBesDebyeLimit); from x = kBesDebyeMax (1e-12, the module's bar) on, with ν above what Hankel's expansion at the
// order carries (ν² > 12x), J, Y, J' and Y' are NaN: the value is not determined by a double-double phase.
inline constexpr int kBesDebyeOrder = 12;
inline constexpr double kBesDebyeMargin = 10.0;
inline constexpr double kBesDebyeLimit = 1.0e15;
inline constexpr double kBesDebyeMax = 1.0e20;
// From this x on J, Y, J' and Y' come from the Hankel expansion when its terms allow it: the CF1 of the Steed method
// needs about x steps and its cap of 10^4 is reached near x = 10^4 (CF1 would also take microseconds where the
// expansion takes a few hundred nanoseconds). From kBesPhaseExactFrom the phase x − (ν/2 + ¼)π is formed without the
// rounding of x.
inline constexpr double kBesHankelFrom = 1000.0;
inline constexpr double kBesPhaseExactFrom = 1.0e4;
// Below this x, J, J', I and I' come from their ascending series: the CF1 recurrence that serves larger x loses digits
// as x falls (I' is 3e5 times over the limit at x = 1e-10), and its 1e-300 seed then underflows (J' = 0 below 1e-23).
inline constexpr double kBesSmallArg = 1.0e-3;
inline constexpr double kBesWholeTurns = 0x1p53; // from here every double is an even integer
// The argument below which 0.5·x may be inexact (it is a subnormal, or zero for the smallest subnormal), and
// 1075·ln 2 as a pair of doubles (high part, then the remainder): ln(x/2) = ln(count) − 1075·ln 2 for x =
// count·2^−1074.
inline constexpr double kBesHalfExact = 0x1p-1021;
inline constexpr double kBesLnHalfCountHi = 745.1332191019412;
inline constexpr double kBesLnHalfCountLo = -1.4483164125345337e-14;
// Complex argument: below this |z| neither asymptotic is tried; kBesRoundoff is the unit of the error estimates.
inline constexpr double kBesComplexAsympFrom = 8.0;
inline constexpr double kBesRoundoff = 1.1e-16;
// Complex argument: a method whose error estimate is below kBesComplexGood is taken as it is; otherwise Hankel's
// expansion at the base order with the recurrence in order is priced as well, from |z| = kBesComplexForwardFrom on (its
// start at order < 2 needs the expansion's smallest term, about e^{−2|z|}, to be down at the rounding). Where base
// sums the series of J (|z| < 2ν), the series is the method up to the module's bar (kBesComplexUsable, below), with the
// estimate (the largest term over the sum) modelling the rounding of the terms by the index of the largest:
// max(1, index/kBesSeriesIndexUnit) times the estimate (the error is up to 2.5 times the estimate at the index 6 and
// grows with the index, about as its square root; with this factor no point of 5800 random ones of the range that the
// series takes is over the bar), and base's own route costs what it costs.
// Everywhere else the limit is kBesComplexGood: an estimate up to the bar was off by up to 11 times the bar for
// Hankel's expansion (J_48.5(84.6 − 66.8i)).
inline constexpr double kBesComplexGood = 1.0e-13;
inline constexpr double kBesComplexForwardFrom = 15.0;
// Where that recurrence is estimated to lose more than kBesComplexStableFrom (its actual error is about 25 times the
// estimate, so this is the bar of the module's tests for J, 1e-10), J and Y come from the downward recurrence of J's
// continued fraction (summed to kBesComplexCf1Tol, at most kBesComplexCf1Cap terms) and K's CF2 at the rotated
// argument, while |Im z| is below kBesComplexStableMax (J is e^{Im z} large, and its factors leave the doubles near
// 709). Below that estimate the recurrence stays, as it costs a third.
inline constexpr double kBesComplexStableFrom = 4.0e-12;
inline constexpr double kBesComplexCf1Tol = 1.0e-15;
inline constexpr int kBesComplexCf1Cap = 1000000;
inline constexpr double kBesComplexStableMax = 690.0;
// Complex J and Y of an order from kBesForwardSteps on have no method of their own (the recurrences stop at that many
// steps): the series or Hankel's expansion serve while their error estimate is within kBesComplexUsable, the module's
// bar for J, and the value is NaN (not determined) where it is not.
inline constexpr double kBesComplexUsable = 1.0e-10;
inline constexpr double kBesSeriesIndexUnit = 6.0; // the index of the largest term, see above
// From this |Im ω| on cos ω and sin ω are one exponential each, e^{|Im ω|} times a unit, and the product of Hankel's
// sums with them is formed on the moderate factors and multiplied by e^{|Im ω|} last (as e^{700}·e^{|Im ω|−700}), so
// that it leaves the doubles only where the result does (J overflows near Im z = 712, its cosine already at 710.5).
inline constexpr double kBesFarAxis = 600.0;
inline constexpr double kBesExp700 = 1.0142320547350045e304;

// Small value types: a pair of neighbouring orders, the four results of the J/Y and I/K cores, a complex number, and
// the Γ-ratio set of Temme's series.
struct BesPair
{
    double base; // order μ (or ν)
    double next; // order μ+1 (or ν+1)
};
struct BesJY
{
    double j;
    double y;
    double jp;
    double yp;
};
struct BesIK
{
    double i;
    double k;
    double ip;
    double kp;
};
struct BesZ
{
    double re;
    double im;
};
struct BesTrig
{
    double sin;
    double cos;
};
struct BesGammas
{
    double g1;              // Γ1(μ) = (1/Γ(1−μ) − 1/Γ(1+μ))/(2μ)
    double g2;              // Γ2(μ) = (1/Γ(1−μ) + 1/Γ(1+μ))/2
    double inv_gamma_plus;  // 1/Γ(1+μ) = Γ2 − μΓ1
    double inv_gamma_minus; // 1/Γ(1−μ) = Γ2 + μΓ1
};

// ln(x/2) for x > 0. Where x/2 is a subnormal it is rounded (to zero for the smallest subnormal, whose logarithm is
// −∞ and turned the series of K, Y, I and I' into NaN; at an odd multiple of it by up to a third). There x < 2^−1021 is
// the integer count = x/2^−1074 (the bit pattern of such an x is that integer, a subnormal's or the first binade's),
// so ln(x/2) = ln(count) − 1075·ln 2 with the count converted exactly: no halving, no scaling by a power of two, and no
// subnormal operand (each of those costs the unit a microcode assist) reach the logarithm.
[[nodiscard]] inline double bes_log_half(double x) noexcept
{
    if (x < kBesHalfExact) [[unlikely]]
    {
        // -0 has the sign bit set and is the count 0, as +0 is; a negative x stays a negative count, whose log is NaN
        const long long count = (x == 0.0) ? 0LL : std::bit_cast<long long>(x);
        return (crd::math::log(static_cast<double>(count)) - kBesLnHalfCountHi) - kBesLnHalfCountLo;
    }
    return crd::math::log(0.5 * x);
}

[[nodiscard]] inline BesZ bes_z_mul(BesZ a, BesZ b) noexcept
{
    return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

[[nodiscard]] inline BesZ bes_z_inv(BesZ a) noexcept
{
    const double norm = a.re * a.re + a.im * a.im;
    return {a.re / norm, -a.im / norm};
}

// r/z for real r: r·conj(z)/|z|².
[[nodiscard]] inline BesZ bes_z_real_over(double r, BesZ z) noexcept
{
    const double ratio = r / (z.re * z.re + z.im * z.im);
    return {z.re * ratio, -z.im * ratio};
}

// The Γ-ratio combinations of Temme (1975; 1976) for |μ| ≤ ½. Γ1 and Γ2 are even in μ, so each is a Chebyshev series in
// t = 8μ² − 1 ∈ [−1, 1]; the fit has no 0/0 cancellation at μ → 0, which integer orders need (their base order is
// exactly 0). The coefficients are the table that Numerical Recipes (2nd ed., §6.7, `beschb`) prints for these two
// fits. Both series are summed together by Clenshaw's recurrence (Clenshaw 1955): ½c_0 + Σ_{k≥1} c_k T_k(t); the Γ1
// table has seven entries, and its eighth, zero, entry lets one loop serve both.
[[nodiscard]] inline BesGammas bes_temme_gammas(double mu) noexcept
{
    static constexpr double kFitGamma1[8] = {
        -1.142022680371168e0, 6.5165112670737e-3, 3.087090173086e-4, -3.4706269649e-6,
        6.9437664e-9,         3.67795e-11,        -1.356e-13,        0.0};
    static constexpr double kFitGamma2[8] = {
        1.843740587300905e0, -7.68528408447867e-2, 1.2719271366546e-3, -4.9717367042e-6,
        -3.31261198e-8,      2.423096e-10,         -1.702e-13,         -1.49e-15};
    const double t = 8.0 * mu * mu - 1.0;
    const double two_t = t + t;
    double a1 = 0.0; // Clenshaw b_{k+1} of the Γ1 series
    double a2 = 0.0; // b_{k+2}
    double c1 = 0.0; // the same pair for the Γ2 series
    double c2 = 0.0;
    for (int k = 7; k >= 1; --k)
    {
        const double a0 = kFitGamma1[k] + two_t * a1 - a2;
        const double c0 = kFitGamma2[k] + two_t * c1 - c2;
        a2 = a1;
        a1 = a0;
        c2 = c1;
        c1 = c0;
    }
    const double g1 = 0.5 * kFitGamma1[0] + t * a1 - a2;
    const double g2 = 0.5 * kFitGamma2[0] + t * c1 - c2;
    return {g1, g2, g2 - mu * g1, g2 + mu * g1};
}

// Downward-recurrence count that puts the base order μ = ν − n in [−½, ½] (the step rule NR's bessjy/bessik use for J,
// Y, I and K). The C cast truncates the half-offset sum toward zero, which is round-half-up for ν ≥ 0. It is kept
// exactly, as an explicit trunc of the same sum, rather than replaced by lround: ν = 0.49999999999999994 gives ν + 0.5
// == 1.0 in binary64 (cast → 1, lround → 0), and either substitution moves μ for such orders and changes the results
// bit for bit, which the hesap bit-exact contract forbids. Every public entry point reflects a negative order first, so
// the cores only ever see ν ≥ 0.
[[nodiscard]] inline int bes_base_steps(double xnu) noexcept
{
    return static_cast<int>(crd::math::trunc(xnu + 0.5));
}

// Step count for J and Y (the rule NR's bessjy uses): below the Temme/CF2 switch it is the base count above; from the
// switch on, the recurrence starts from an order about one below x, so that CF2 converges quickly, and the count is
// floored at zero. The 1.5 offset is a shift, not a rounding, and stays as written.
[[nodiscard]] inline int bes_jy_steps(double x, double xnu) noexcept
{
    if (x < kBesTemmeLimit)
    {
        return bes_base_steps(xnu);
    }
    const int shifted = static_cast<int>(xnu - x + 1.5);
    return (shifted > 0) ? shifted : 0;
}

// Result of the J fraction: the ratio J'_ν/J_ν and the sign of the unnormalised J_ν it was found with.
struct BesCf1
{
    double ratio;
    int sign;
};

// CF1 for the logarithmic derivative (Barnett et al. 1974). The order recurrence w_{ν−1} + s·w_{ν+1} = (2ν/x)w_ν,
// with s = +1 for J and −1 for I, gives
//   J'/J = ν/x − 1/(2(ν+1)/x − 1/(2(ν+2)/x − …)),   I'/I = ν/x + 1/(2(ν+1)/x + 1/(2(ν+2)/x + …)).
// Both are evaluated by Lentz's modified algorithm (Lentz 1976; Thompson & Barnett 1986): the tails are
// C_k = b_k + t/C_{k−1} and 1/D_k = b_k + t·D_{k−1}, with b_k = 2(ν+k)/x and t = −1 for J, +1 for I, and the value is
// multiplied by C_k·D_k until that factor is 1. Each b_k is formed from its index, so no rounding accumulates over the
// thousands of steps a large argument needs.

// The J fraction. A denominator can vanish or turn negative, so both tails are held off zero, and every negative D_k
// flips the sign of the unnormalised J (the parity of the flips is returned as a sign). This fraction and the CF2 of
// Hankel's function below are forced inline, as the core that calls them is: with the core inlined into every public
// function that wants J or Y, each of those sites counted as a call of its own and the compiler left both out of
// line, which cost J in the Steed range (x of 17 .. 40 at an order of 12 or 20) 3 % at a constant order.
[[nodiscard]] CRD_FORCEINLINE BesCf1 bes_cf1_oscillating(double inv_x, double nu) noexcept
{
    const double spacing = 2.0 * inv_x;
    double value = nu * inv_x;
    if (value < kBesTiny)
    {
        value = kBesTiny;
    }
    double num_tail = value;
    double den_tail = 0.0;
    double index = 0.0;
    unsigned flips = 0U;
    for (int step = 1; step <= kBesIterCap; ++step)
    {
        index += 1.0;
        const double b = spacing * (nu + index);
        double den = b - den_tail;
        double num = b - 1.0 / num_tail;
        // Both floors sit behind one rarely taken test on the smaller magnitude, so no select lengthens the
        // dependency chain.
        const double den_mag = crd::math::fabs(den);
        const double num_mag = crd::math::fabs(num);
        const double smaller = (den_mag < num_mag) ? den_mag : num_mag;
        if (smaller < kBesTiny) [[unlikely]]
        {
            if (den_mag < kBesTiny)
            {
                den = kBesTiny;
            }
            if (num_mag < kBesTiny)
            {
                num = kBesTiny;
            }
        }
        den_tail = 1.0 / den;
        num_tail = num;
        if (den_tail < 0.0)
        {
            ++flips;
        }
        const double factor = num_tail * den_tail;
        value *= factor;
        if (crd::math::fabs(factor - 1.0) < kBesRelTol)
        {
            break;
        }
    }
    return {value, ((flips & 1U) != 0U) ? -1 : 1};
}

// The I fraction. Every tail is positive, so neither a floor nor a sign is needed. The D recurrence is advanced before
// the C recurrence.
[[nodiscard]] inline double bes_cf1_growing(double inv_x, double nu) noexcept
{
    const double spacing = 2.0 * inv_x;
    double value = nu * inv_x;
    if (value < kBesTiny)
    {
        if (nu < 0.0) [[unlikely]]
        {
            return std::numeric_limits<double>::quiet_NaN(); // a negative order is reflected before it reaches the core
        }
        value = kBesTiny;
    }
    double num_tail = value;
    double den_tail = 0.0;
    double index = 0.0;
    for (int step = 1; step <= kBesIterCap; ++step)
    {
        index += 1.0;
        const double b = spacing * (nu + index);
        den_tail = 1.0 / (b + den_tail);
        num_tail = b + 1.0 / num_tail;
        const double factor = num_tail * den_tail;
        value *= factor;
        if (crd::math::fabs(factor - 1.0) < kBesRelTol)
        {
            break;
        }
    }
    return value;
}

// Steed's CF2 for p + iq = H^(1)'_μ(x)/H^(1)_μ(x) (x ≥ 2, |μ| ≤ ½; Barnett et al. 1974, Thompson & Barnett 1986):
//   p + iq = −1/(2x) + i + (i/x)·a_1/(b_1 + a_2/(b_2 + …)),  a_k = (k − ½)² − μ², b_k = 2(x + ik),
// summed by the modified Lentz recurrence on complex terms (tails C_k = b_k + a_k/C_{k−1}, 1/D_k = b_k + a_k·D_{k−1},
// running value multiplied by C_k·D_k). The leading term −1/(2x) + i plays the role of b_0, and the first partial
// numerator i·a_1/x is imaginary, so the first factor is formed on its own and the loop covers the real a_k, k ≥ 2.
[[nodiscard]] CRD_FORCEINLINE BesZ bes_cf2_hankel(double x, double mu2) noexcept
{
    const double inv_x = 1.0 / x;
    BesZ value{-0.5 * inv_x, 1.0};
    BesZ b{2.0 * x, 2.0};
    const BesZ lead = bes_z_mul(BesZ{0.0, (0.25 - mu2) * inv_x}, bes_z_inv(value));
    BesZ num_tail{b.re + lead.re, b.im + lead.im};
    BesZ den_tail = bes_z_inv(b);
    BesZ factor = bes_z_mul(num_tail, den_tail);
    value = bes_z_mul(value, factor);
    for (int k = 2; k <= kBesIterCap; ++k)
    {
        const double a = (k - 0.5) * (k - 0.5) - mu2;
        b.im += 2.0;
        BesZ den{b.re + a * den_tail.re, b.im + a * den_tail.im};
        const BesZ back = bes_z_real_over(a, num_tail);
        num_tail = {b.re + back.re, b.im + back.im};
        // Both floors sit behind one rarely taken test on the smaller magnitude, so no select lengthens the
        // dependency chain.
        const double den_mag = crd::math::fabs(den.re) + crd::math::fabs(den.im);
        const double num_mag = crd::math::fabs(num_tail.re) + crd::math::fabs(num_tail.im);
        const double smaller = (den_mag < num_mag) ? den_mag : num_mag;
        if (smaller < kBesTiny) [[unlikely]]
        {
            if (den_mag < kBesTiny)
            {
                den.re = kBesTiny;
            }
            if (num_mag < kBesTiny)
            {
                num_tail.re = kBesTiny;
            }
        }
        den_tail = bes_z_inv(den);
        factor = bes_z_mul(num_tail, den_tail);
        value = bes_z_mul(value, factor);
        if (crd::math::fabs(factor.re - 1.0) + crd::math::fabs(factor.im) < kBesRelTol)
        {
            break;
        }
    }
    return value;
}

// Quantities shared by Temme's series for Y_μ (Temme 1976) and for K_μ (Temme 1975), with σ = μ ln(2/x):
//   f_0 = (πμ/sin πμ)·[Γ1(μ) cosh σ + Γ2(μ)·(sinh σ/σ)·ln(2/x)]   (Y carries an extra factor 2/π),
// together with e^σ, 1/Γ(1±μ) and πμ. The two removable singularities (πμ → 0 and σ → 0) take their limit 1.
struct BesTemmeHead
{
    double f0;
    double e_sigma;
    double inv_gamma_plus;
    double inv_gamma_minus;
    double mu_pi;
};

[[nodiscard]] inline BesTemmeHead bes_temme_head(double x, double mu) noexcept
{
    const double mu_pi = kBesPi * mu;
    const double ln_2_over_x = -bes_log_half(x);
    const double sigma = mu * ln_2_over_x;
    const double x_over_sin = (crd::math::fabs(mu_pi) < kBesRelTol) ? 1.0 : mu_pi / crd::math::sin(mu_pi);
    const double sinhc = (crd::math::fabs(sigma) < kBesRelTol) ? 1.0 : crd::math::sinh(sigma) / sigma;
    const BesGammas g = bes_temme_gammas(mu);
    return {x_over_sin * (g.g1 * crd::math::cosh(sigma) + g.g2 * sinhc * ln_2_over_x), crd::math::exp(sigma),
            g.inv_gamma_plus, g.inv_gamma_minus, mu_pi};
}

// Temme's series (Temme 1976) for Y_μ(x) and Y_{μ+1}(x), x < 2, |μ| ≤ ½ (DLMF 10.8):
//   Y_μ = −Σ c_k g_k,  Y_{μ+1} = −(2/x) Σ c_k h_k,  c_k = (−x²/4)^k/k!,
//   g_k = f_k + (2/μ)sin²(μπ/2)·q_k,  h_k = p_k − k·g_k,
// with the coupling f_k = (k f_{k−1} + p_{k−1} + q_{k−1})/(k² − μ²), p_k = p_{k−1}/(k − μ), q_k = q_{k−1}/(k + μ).
// One reciprocal of k² − μ² serves the f, p and q updates: 1/(k∓μ) = (k±μ)/(k² − μ²).
[[nodiscard]] inline BesPair bes_temme_y(double x, double mu) noexcept
{
    const BesTemmeHead s = bes_temme_head(x, mu);
    const double half_mu_pi = 0.5 * s.mu_pi;
    const double sinc = (crd::math::fabs(half_mu_pi) < kBesRelTol) ? 1.0 : crd::math::sin(half_mu_pi) / half_mu_pi;
    const double skew = 0.5 * kBesPi * s.mu_pi * sinc * sinc; // (2/μ)sin²(μπ/2), finite at μ = 0
    const double weight = -0.25 * x * x;
    double fk = (2.0 / kBesPi) * s.f0;
    double pk = s.e_sigma / (kBesPi * s.inv_gamma_plus);
    double qk = 1.0 / (kBesPi * s.e_sigma * s.inv_gamma_minus);
    double ck = 1.0;
    double sum_g = fk + skew * qk; // Σ c_k g_k
    double sum_h = pk;             // Σ c_k h_k
    for (int k = 1; k <= kBesIterCap; ++k)
    {
        const double kd = k;
        const double inv_kk = 1.0 / ((kd - mu) * (kd + mu));
        fk = (kd * fk + pk + qk) * inv_kk;
        pk *= (kd + mu) * inv_kk;
        qk *= (kd - mu) * inv_kk;
        ck *= weight / kd;
        const double gk = fk + skew * qk;
        sum_g += ck * gk;
        sum_h += ck * (pk - kd * gk);
        if (crd::math::fabs(ck * gk) < (1.0 + crd::math::fabs(sum_g)) * kBesRelTol)
        {
            break;
        }
    }
    return {-sum_g, -2.0 * sum_h / x};
}

// Temme's series for K_μ(x) and K_{μ+1}(x), x < 2, |μ| ≤ ½ (Temme 1975; DLMF 10.31): K_μ = Σ c_k f_k,
// K_{μ+1} = (2/x) Σ c_k h_k, c_k = (x²/4)^k/k!, h_k = p_k − k f_k, with f_k, p_k, q_k as in bes_temme_y.
[[nodiscard]] inline BesPair bes_temme_k(double x, double mu) noexcept
{
    const BesTemmeHead s = bes_temme_head(x, mu);
    const double weight = 0.25 * x * x;
    double fk = s.f0;
    double pk = 0.5 * s.e_sigma / s.inv_gamma_plus;
    double qk = 0.5 / (s.e_sigma * s.inv_gamma_minus);
    double ck = 1.0;
    double sum_f = fk; // Σ c_k f_k
    double sum_h = pk; // Σ c_k h_k
    for (int k = 1; k <= kBesIterCap; ++k)
    {
        const double kd = k;
        const double inv_kk = 1.0 / ((kd - mu) * (kd + mu));
        fk = (kd * fk + pk + qk) * inv_kk;
        pk *= (kd + mu) * inv_kk;
        qk *= (kd - mu) * inv_kk;
        ck *= weight / kd;
        const double term = ck * fk;
        sum_f += term;
        sum_h += ck * (pk - kd * fk);
        if (crd::math::fabs(term) < crd::math::fabs(sum_f) * kBesRelTol)
        {
            break;
        }
    }
    return {sum_f, 2.0 * sum_h / x};
}

// Steed's CF2 for K (x ≥ 2, |μ| ≤ ½; Temme 1975, Thompson & Barnett 1986): K_μ(x) = √(π/2x)·e^{−x}/S. With
// a_k = (k − ½)² − μ² and b_k = 2(x + k), two Steed recurrences run side by side. The fraction
//   h = 1/(b_1 − a_2/(b_2 − a_3/(b_3 − …)))  is accumulated as h = Σ Δh_k, with D_k = 1/(b_k − a_k D_{k−1}) and
//   Δh_k = (b_k D_k − 1)Δh_{k−1};  the weights are c_k = a_k c_{k−1}/k  and  Q_k = (b_{k−1}Q_{k−1} − Q_{k−2})/a_k,
// and S = 1 + Σ_k (Σ_{j≤k} c_j Q_j)·Δh_k. Then K_{μ+1}/K_μ = (μ + x + ½ + (μ² − ¼)h)/x. The sums h and S do not hold
// e^{−x}: bes_cf2_k returns K_μ and K_{μ+1}, and the rescaled I below takes the sums alone.
struct BesCf2Sums
{
    double s;
    double h;
    double a1;
};
[[nodiscard]] CRD_FORCEINLINE BesCf2Sums bes_cf2_k_sums(double x, double mu) noexcept
{
    const double mu2 = mu * mu;
    const double a1 = 0.25 - mu2;
    double b_prev = 2.0 * (x + 1.0); // b_{k−1}
    double d = 1.0 / b_prev;         // D_1
    double dh = d;                   // Δh_1
    double h = dh;
    double q_old = 0.0;         // Q_{k−2}
    double q_cur = 1.0;         // Q_{k−1}
    double c_k = a1;            // c_1
    double cq_sum = a1 * q_cur; // Σ_{j≤k} c_j Q_j
    double s = 1.0 + cq_sum * dh;
    for (int k = 2; k <= kBesIterCap; ++k)
    {
        const double kd = k;
        const double a = (kd - 0.5) * (kd - 0.5) - mu2;
        const double b = 2.0 * (x + kd);
        c_k *= a / kd;
        const double q_new = (b_prev * q_cur - q_old) / a;
        q_old = q_cur;
        q_cur = q_new;
        cq_sum += c_k * q_new;
        d = 1.0 / (b - a * d);
        dh *= b * d - 1.0;
        h += dh;
        const double ds = cq_sum * dh;
        s += ds;
        b_prev = b;
        if (crd::math::fabs(ds) < crd::math::fabs(s) * kBesRelTol)
        {
            break;
        }
    }
    return {s, h, a1};
}
[[nodiscard]] CRD_FORCEINLINE BesPair bes_cf2_k(double x, double mu) noexcept
{
    const BesCf2Sums sums = bes_cf2_k_sums(x, mu);
    const double kmu = crd::math::sqrt(kBesPi / (2.0 * x)) * crd::math::exp(-x) / sums.s;
    return {kmu, kmu * (mu + x + 0.5 - sums.a1 * sums.h) / x};
}

// Carries a function and its neighbour down in order from ν to the base order μ = ν − steps, on the unnormalised scale
// of the CF1 seed. From w_ν (seed) and w'_ν (seed_d) the order ν−1 value is w_{ν−1} = (ν/x)w_ν + w'_ν (J and I alike);
// the order recurrence w_{m−1} = (2m/x)w_m + s·w_{m+1} (s = −1 for J, +1 for I) then lowers the pair (w_{m+1}, w_m),
// and at the end w'_μ = (μ/x)w_μ + s·w_{μ+1}. Returns {w_μ, w'_μ}.
template <bool Oscillatory>
[[nodiscard]] inline BesPair bes_lower_to_base(double seed, double seed_d, double nu, int steps, double inv_x) noexcept
{
    constexpr double kLink = Oscillatory ? -1.0 : 1.0;
    if (steps <= 0)
    {
        return {seed, seed_d};
    }
    const double two_inv_x = 2.0 * inv_x;
    const double mu = nu - steps;
    double hi = seed;                       // order m + 1
    double lo = nu * inv_x * seed + seed_d; // order m = ν − 1
    for (int j = steps - 1; j >= 1; --j)
    {
        const double next = (mu + j) * two_inv_x * lo + kLink * hi;
        hi = lo;
        lo = next;
    }
    return {lo, mu * inv_x * lo + kLink * hi};
}

// Carries a pair (w_μ, w_{μ+1}) up in order to (w_ν, w_{ν+1}), ν = μ + steps, by w_{m+1} = (2m/x)w_m + s·w_{m−1}
// (s = −1 for Y, +1 for K).
template <bool Oscillatory>
[[nodiscard]] inline BesPair bes_raise_from_base(BesPair pair, double mu, int steps, double two_inv_x) noexcept
{
    constexpr double kLink = Oscillatory ? -1.0 : 1.0;
    for (int m = 1; m <= steps; ++m)
    {
        const double upper = (mu + m) * two_inv_x * pair.next + kLink * pair.base;
        pair.base = pair.next;
        pair.next = upper;
    }
    return pair;
}

// Hankel's asymptotic expansion (DLMF 10.17) for J_ν and Y_ν at real x > 0 and any real order: with
// ω = x − (ν/2 + ¼)π and the coefficients a_k(ν) = ∏(4ν² − (2j−1)²)/(8j),
//   J_ν = √(2/πx)(P cos ω − Q sin ω),  Y_ν = √(2/πx)(P sin ω + Q cos ω),
//   P = Σ (−1)^m a_{2m}/x^{2m},  Q = Σ (−1)^m a_{2m+1}/x^{2m+1},
// summed up to the smallest term. While k < ν + ½ the terms may still grow (the first ratio is about ν²/(2x)), so the
// sum is cut only after the numerator 4ν² − (2k−1)² has changed sign; a growth before that point is the hump, not the
// divergence. The result says whether it can be trusted: the smallest term bounds the truncation, the largest bounds
// the cancellation.
struct BesHankel
{
    double p;
    double q;
    bool reliable;
};
// From this order on the coefficient a_k and the power x^{-k} of the sums below leave the doubles separately (a_60 is
// beyond 1e308 at ν = 3000, and the power is zero for x of 1e5 and more), and their product, which is of the size of
// the terms, becomes NaN: the terms are carried as one product there.
inline constexpr double kBesHankelScaledFrom = 2000.0;
[[nodiscard]] inline BesHankel bes_hankel_pq_scaled(double nu, double x) noexcept
{
    const double mu = 4.0 * nu * nu;
    const double inv = 1.0 / x;
    double term = 1.0;
    double psum = 1.0;
    double qsum = 0.0;
    double prev = 1e300;
    double biggest = 1.0;
    // ν ≥ 60 here: every term up to kBesHankelTerms is inside the growth phase, none ends the sum
    for (int k = 1; k <= kBesHankelTerms; ++k)
    {
        const double odd = 2.0 * k - 1.0;
        term *= ((mu - odd * odd) / (8.0 * k)) * inv;
        if ((k & 1) == 0)
        {
            psum += (((k / 2) & 1) == 0) ? term : -term;
        }
        else
        {
            qsum += ((((k - 1) / 2) & 1) == 0) ? term : -term;
        }
        prev = crd::math::fabs(term);
        biggest = (prev > biggest) ? prev : biggest;
    }
    return {psum, qsum, prev <= kBesAsympFloor && biggest <= kBesAsympGrowth};
}
[[nodiscard]] inline BesHankel bes_hankel_pq(double nu, double x) noexcept
{
    if (nu >= kBesHankelScaledFrom) [[unlikely]]
    {
        return bes_hankel_pq_scaled(nu, x);
    }
    const double mu = 4.0 * nu * nu;
    const double inv = 1.0 / x;
    double ak = 1.0;
    double xpow = 1.0;
    double psum = 1.0;
    double qsum = 0.0;
    // One term: updates the coefficient and the two sums, returns the term's size.
    const auto add_term = [&](int k) noexcept
    {
        ak *= (mu - (2.0 * k - 1.0) * (2.0 * k - 1.0)) / (8.0 * k);
        xpow *= inv;
        const double term = ak * xpow;
        if ((k & 1) == 0)
        {
            psum += (((k / 2) & 1) == 0) ? term : -term;
        }
        else
        {
            qsum += ((((k - 1) / 2) & 1) == 0) ? term : -term;
        }
        return crd::math::fabs(term);
    };
    // Growth phase: the k with (2k−1)² ≤ 4ν², that is k ≤ ν + ½ (the count is ⌊ν + ½⌋), where the terms may still
    // grow. The sum is not cut there, and the largest term is looked for there only (after it the terms fall, until
    // the divergence), so for ν < ½ the phase is empty and the loop below is the plain one.
    const int growth = (nu < 60.0) ? static_cast<int>(nu + 0.5) : 60;
    double prev = 1e300;
    double biggest = 1.0;
    for (int k = 1; k <= growth; ++k)
    {
        prev = add_term(k);
        biggest = (prev > biggest) ? prev : biggest;
    }
    for (int k = growth + 1; k <= kBesHankelTerms; ++k)
    {
        const double mag = add_term(k);
        if (mag > prev)
        {
            break; // optimal truncation
        }
        prev = mag;
        if (mag < kBesHankelDone)
        {
            break; // the rest cannot change the sums
        }
    }
    return {psum, qsum, prev <= kBesAsympFloor && biggest <= kBesAsympGrowth};
}

// sin r and cos r on |r| ≤ π/4 from the minimax polynomials of crd-math's trigonometry (kSin1 … kCos7), summed in plain
// arithmetic by Estrin's scheme: crd::math's own routines issue one fused multiply-add per coefficient, and where the
// unit has no such instruction each of those is a call, which costs more than the rest of a reflection coefficient or
// of a Hankel phase. The result differs from crd::math::sin and cos in the last place or two.
[[nodiscard]] inline double bes_sin_reduced(double r) noexcept
{
    const double u = r * r;
    const double u2 = u * u;
    const double p = (crd::math::kSin1 + u * crd::math::kSin2) +
                     u2 * ((crd::math::kSin3 + u * crd::math::kSin4) + u2 * (crd::math::kSin5 + u * crd::math::kSin6));
    return r + (r * u) * p;
}
[[nodiscard]] inline double bes_cos_reduced(double r) noexcept
{
    const double u = r * r;
    const double u2 = u * u;
    const double p = (crd::math::kCos2 + u * crd::math::kCos3) +
                     u2 * ((crd::math::kCos4 + u * crd::math::kCos5) + u2 * (crd::math::kCos6 + u * crd::math::kCos7));
    return (1.0 - 0.5 * u) + u2 * p;
}

// sin x and cos x for any x: below kBesSincosRange the argument is reduced to [−π/4, π/4] by the quarter-turn count
// (Cody and Waite's three-part π/2, whose products are exact up to 2^20 turns), above it crd::math::sincos serves.
inline constexpr double kBesSincosRange = 1.0e6;
inline void bes_sincos(double x, double& s, double& c) noexcept
{
    const double ax = crd::math::fabs(x);
    if (!(ax < kBesSincosRange)) [[unlikely]]
    {
        crd::math::sincos(x, s, c);
        return;
    }
    const long long quarters = static_cast<long long>(ax * crd::math::k2OverPi + 0.5);
    const double n = static_cast<double>(quarters);
    const double r = ((ax - n * crd::math::kPio2Part1) - n * crd::math::kPio2Part2) - n * crd::math::kPio2Part3;
    const double sp = bes_sin_reduced(r);
    const double cp = bes_cos_reduced(r);
    const int phase = static_cast<int>(quarters & 3);
    double sa = sp;
    double ca = cp;
    if (phase == 1)
    {
        sa = cp;
        ca = -sp;
    }
    else if (phase == 2)
    {
        sa = -sp;
        ca = -cp;
    }
    else if (phase == 3)
    {
        sa = -cp;
        ca = sp;
    }
    s = (x < 0.0) ? -sa : sa;
    c = ca;
}

// sin ω and cos ω for ω = x − (ν/2 + ¼)π. Up to kBesPhaseExactFrom the plain difference is accurate enough (its
// rounding is about ε·x). Beyond, sin x and cos x (exact for any x) are combined with the offset, which is reduced
// exactly first: (ν/2 + ¼) mod 2 is exact, so ω is never formed as a difference of large numbers. An argument that
// itself carries rounding (Airy's ζ) passes its tail x_lo, and always takes the second form.
[[nodiscard]] inline BesTrig bes_hankel_phase(double nu, double x, double x_lo = 0.0) noexcept
{
    const double shift = 0.5 * nu + 0.25;
    if (x < kBesPhaseExactFrom && x_lo == 0.0)
    {
        const double omega = x - shift * kBesPi;
        BesTrig t;
        bes_sincos(omega, t.sin, t.cos);
        return t;
    }
    // |frac| ≤ 1, exact; from ν = 2^50 on the ¼ is added after the reduction, where it is no longer lost in ν/2
    const double half = 0.5 * nu;
    const double frac = (nu < 0x1p50) ? shift - 2.0 * crd::math::nearbyint(0.5 * shift)
                                      : (half - 2.0 * crd::math::nearbyint(0.5 * half)) + 0.25;
    // x may come with a tail x_lo (|x_lo| ≤ ½ ulp of x): sin(x + x_lo) = sin x + x_lo cos x to second order
    double sx;
    double cx;
    bes_sincos(x, sx, cx);
    const double sin_x = sx + x_lo * cx;
    const double cos_x = cx - x_lo * sx;
    double sin_f;
    double cos_f;
    bes_sincos(kBesPi * frac, sin_f, cos_f);
    return {sin_x * cos_f - cos_x * sin_f, cos_x * cos_f + sin_x * sin_f};
}

struct BesHankelJY
{
    double j;
    double y;
    bool reliable;
};
[[nodiscard]] inline BesHankelJY bes_hankel_jy(double nu, double x, double x_lo = 0.0) noexcept
{
    const BesHankel h = bes_hankel_pq(nu, x);
    const BesTrig t = bes_hankel_phase(nu, x, x_lo);
    const double amplitude = crd::math::sqrt(2.0 / (kBesPi * x));
    return {amplitude * (t.cos * h.p - t.sin * h.q), amplitude * (t.sin * h.p + t.cos * h.q), h.reliable};
}

// J and Y of the two orders μ and μ+1 (μ ∈ [0, 1)) from Hankel's expansion: the sums of both orders share one phase,
// because ω_{μ+1} = ω_μ − π/2 gives sin ω_{μ+1} = −cos ω_μ and cos ω_{μ+1} = sin ω_μ. At such low orders the expansion
// has no hump and its terms fall at once, which is the start of the recurrence in order below.
struct BesHankelPair
{
    double j0;
    double y0;
    double j1;
    double y1;
    bool reliable;
};
[[nodiscard]] inline BesHankelPair bes_hankel_base_pair(double mu, double x) noexcept
{
    const BesHankel low = bes_hankel_pq(mu, x);
    const BesHankel high = bes_hankel_pq(mu + 1.0, x);
    const BesTrig t = bes_hankel_phase(mu, x);
    const double amplitude = crd::math::sqrt(2.0 / (kBesPi * x));
    return {amplitude * (t.cos * low.p - t.sin * low.q), amplitude * (t.sin * low.p + t.cos * low.q),
            amplitude * (t.sin * high.p + t.cos * high.q), amplitude * (t.sin * high.q - t.cos * high.p),
            low.reliable && high.reliable};
}

// Carries the pair of J (or of Y) from the base order μ to ν = μ + steps by the recurrence in order, which is stable
// upward while the order stays below x (J and Y are both oscillatory there; DLMF 10.6.1).
[[nodiscard]] inline BesPair bes_forward_order(double first, double second, double mu, int steps, double x) noexcept
{
    return bes_raise_from_base<true>(BesPair{first, second}, mu, steps, 2.0 / x);
}

// The ascending series of J_ν (kind −1) or I_ν (kind +1) for real x > 0: (x/2)^ν/Γ(ν+1) Σ (kind·x²/4)^k/(k!(ν+1)_k).
template <bool Guarded> [[nodiscard]] inline double bes_ascending_series(double nu, double x, double kind) noexcept
{
    const double half = 0.5 * x;
    const double log_half = Guarded ? bes_log_half(x) : crd::math::log(half);
    double t = crd::math::exp(nu * log_half - lgamma(nu + 1.0)); // (x/2)^ν / Γ(ν+1)
    double sum = t;
    const double z2 = kind * half * half;
    for (int k = 1; k <= 400; ++k)
    {
        t *= z2 / (k * (nu + k));
        sum += t;
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum))
        {
            break;
        }
    }
    return sum;
}
// The slope of that series, term by term: J'_ν = Σ (−1)^k (ν+2k)(x/2)^{ν+2k−1}/(2 k! Γ(ν+k+1)) (I'_ν: no sign). The
// first term is formed with the power ν − 1 so that it survives where J_ν itself underflows (J' = (ν/x)J_ν − J_{ν+1}
// would not), and no cancellation arises between the terms of the slope.
[[nodiscard]] inline double bes_ascending_slope(double nu, double x, double kind) noexcept
{
    const double half = 0.5 * x;
    const double log_lead = (nu - 1.0) * bes_log_half(x) - lgamma(nu + 1.0);
    double t = 0.5 * crd::math::exp(log_lead); // (x/2)^{ν−1}/(2Γ(ν+1))
    if (t > std::numeric_limits<double>::max()) [[unlikely]]
    {
        // (x/2)^{ν−1} overflows (ν < 1 and x near the bottom of the range): the first nonzero term carries the sum.
        // It is ν·t for ν > 0, formed in the logarithm where it may still fit, and the k = 1 term, kind·x/2, for
        // ν = 0; the next term is smaller by x².
        return (nu > 0.0) ? crd::math::exp(log_lead + crd::math::log(0.5 * nu)) : kind * half;
    }
    double sum = (nu == 0.0) ? 0.0 : nu * t;
    if (sum < 1.0e-290 && nu != 0.0) [[unlikely]]
    {
        // The leading term is a subnormal here, and t, then ν·t, would each round it to a coarse grid: ν·t is formed in
        // the logarithm instead and rounded once.
        sum = crd::math::exp(log_lead + crd::math::log(0.5 * nu));
    }
    for (int k = 1; k <= 400; ++k)
    {
        // (x/2)² would underflow for x < 1e-154 while t is that large, so the factors are applied one at a time
        t *= kind * half;
        t /= k * (nu + k);
        t *= half;
        const double term = (nu + 2.0 * k) * t;
        sum += term;
        if (crd::math::fabs(term) <= 1e-18 * crd::math::fabs(sum))
        {
            break;
        }
    }
    return sum;
}
// ---- dedicated J_ν fast path (real x > 0, ν ≥ 0): ascending series (small x) + Hankel asymptotic (large x). ----
// Avoids computing the Y/J'/Y' quartet of bes_jy_core ⇒ ~3× fewer ops.
[[nodiscard]] inline double besselj_series(double nu, double x) noexcept
{
    return bes_ascending_series<true>(nu, x, -1.0);
}
[[nodiscard]] inline double besseli_series(double nu, double x) noexcept
{
    return bes_ascending_series<true>(nu, x, 1.0);
}

// Y_ν and Y'_ν from the pair (Y_ν, Y_{ν+1}) that the upward recurrence ends with. If the recurrence or the slope
// overflowed, Y_ν(x) is −∞ if it did not fit either and its slope is +∞ (Y_ν < 0 and rising for ν > x), but the
// recurrence leaves ∞ − ∞ behind.
[[nodiscard]] inline BesPair bes_y_from_top([[maybe_unused]] double x, double nu, double inv_x, BesPair top) noexcept
{
    double y_nu = top.base;
    double y_nu_d = nu * inv_x * top.base - top.next;
    if (y_nu_d != y_nu_d && nu - nu == 0.0) [[unlikely]]
    {
        if (y_nu != y_nu)
        {
            y_nu = -std::numeric_limits<double>::infinity();
        }
        y_nu_d = std::numeric_limits<double>::infinity();
    }
    return {y_nu, y_nu_d};
}

// K'_ν from the pair (K_ν, K_{ν+1}); an overflowed K_ν = +∞ is falling, so its slope is −∞.
[[nodiscard]] inline double bes_k_slope([[maybe_unused]] double x, double nu, double inv_x, BesPair top) noexcept
{
    // K' < 0 for every x > 0, so a slope that underflows is −0: it is written as −(K_{ν+1} − (ν/x)K_ν).
    const double slope = -(top.next - nu * inv_x * top.base);
    return (slope != slope && nu - nu == 0.0) ? -std::numeric_limits<double>::infinity() : slope;
}

// Double-double numbers (an unevaluated sum hi + lo, |lo| ≤ ½ ulp(hi); Dekker 1971) for the phase of Debye's expansion,
// where an error of ε·ξ, with ξ as large as x, would be the error of the result.
struct BesDD
{
    double hi;
    double lo;
};
[[nodiscard]] inline BesDD bes_dd_fast_sum(double a, double b) noexcept // |a| ≥ |b|
{
    const double s = a + b;
    return {s, b - (s - a)};
}
[[nodiscard]] inline BesDD bes_dd_two_sum(double a, double b) noexcept
{
    const double s = a + b;
    const double bb = s - a;
    return {s, (a - (s - bb)) + (b - bb)};
}
[[nodiscard]] inline BesDD bes_dd_two_prod(double a, double b) noexcept
{
    const double p = a * b;
    return {p, crd::math::fma(a, b, -p)};
}
[[nodiscard]] inline BesDD bes_dd_add(BesDD a, BesDD b) noexcept
{
    BesDD s = bes_dd_two_sum(a.hi, b.hi);
    const BesDD t = bes_dd_two_sum(a.lo, b.lo);
    s = bes_dd_fast_sum(s.hi, s.lo + t.hi);
    return bes_dd_fast_sum(s.hi, s.lo + t.lo);
}
[[nodiscard]] inline BesDD bes_dd_mul(BesDD a, BesDD b) noexcept
{
    const BesDD p = bes_dd_two_prod(a.hi, b.hi);
    return bes_dd_fast_sum(p.hi, p.lo + (a.hi * b.lo + a.lo * b.hi));
}
[[nodiscard]] inline BesDD bes_dd_mul_d(BesDD a, double b) noexcept
{
    const BesDD p = bes_dd_two_prod(a.hi, b);
    return bes_dd_fast_sum(p.hi, p.lo + a.lo * b);
}
[[nodiscard]] inline BesDD bes_dd_div_d(BesDD a, double b) noexcept
{
    const double q = a.hi / b;
    const BesDD p = bes_dd_two_prod(q, b);
    return bes_dd_fast_sum(q, (((a.hi - p.hi) - p.lo) + a.lo) / b);
}
[[nodiscard]] inline BesDD bes_dd_sqrt(BesDD a) noexcept
{
    const double r = crd::math::sqrt(a.hi);
    const BesDD sq = bes_dd_two_prod(r, r);
    return bes_dd_fast_sum(r, (((a.hi - sq.hi) - sq.lo) + a.lo) / (2.0 * r));
}
// sin t and cos t for 0 ≤ t ≤ π/2 from their Maclaurin series, summed to 1e-35.
inline void bes_dd_sincos(double t, BesDD& sine, BesDD& cosine) noexcept
{
    const BesDD t2 = bes_dd_two_prod(t, t);
    BesDD sin_term{t, 0.0};
    BesDD cos_term{1.0, 0.0};
    sine = sin_term;
    cosine = cos_term;
    for (int n = 1; n <= 18; ++n)
    {
        const double a = 2.0 * n;
        sin_term = bes_dd_div_d(bes_dd_mul(sin_term, t2), -(a * (a + 1.0)));
        cos_term = bes_dd_div_d(bes_dd_mul(cos_term, t2), -((a - 1.0) * a));
        sine = bes_dd_add(sine, sin_term);
        cosine = bes_dd_add(cosine, cos_term);
    }
}

// Debye's polynomials (DLMF 10.41.9): U_0 = 1, U_{k+1}(p) = ½p²(1−p²)U_k'(p) + ⅛∫_0^p (1−5t²)U_k(t) dt. U_k has the
// parity of k and degree 3k, so U_k(p) = p^k Σ_j c_{k,j} p^{2j}, j = 0 … k; the table holds the c_{k,j}, built from
// the recurrence when the program is compiled.
struct BesDebyeTable
{
    double c[kBesDebyeOrder + 1][kBesDebyeOrder + 1];
};
[[nodiscard]] constexpr BesDebyeTable bes_make_debye_table() noexcept
{
    BesDebyeTable table{};
    double u[3 * kBesDebyeOrder + 4] = {1.0}; // coefficients by power of p of U_k
    for (int k = 0; k <= kBesDebyeOrder; ++k)
    {
        for (int j = 0; j <= k; ++j)
        {
            table.c[k][j] = u[k + 2 * j];
        }
        double next[3 * kBesDebyeOrder + 4] = {};
        for (int n = 0; n <= 3 * k; ++n)
        {
            const double slope = 0.5 * n * u[n]; // ½p²(1−p²) times the term n·u_n·p^{n−1} of U_k'
            next[n + 1] += slope + 0.125 * u[n] / (n + 1);
            next[n + 3] -= slope + 0.625 * u[n] / (n + 3);
        }
        for (int n = 0; n < 3 * kBesDebyeOrder + 4; ++n)
        {
            u[n] = next[n];
        }
    }
    return table;
}
inline constexpr BesDebyeTable kBesDebyeTable = bes_make_debye_table();

// The two sums of DLMF 10.19.6 at p = i·cot β = i·q: U_{2m}(iq) is real and U_{2m+1}(iq) imaginary, so with
// U_k(iq) = i^k q^k Σ_j c_{k,j}(−q²)^j the even sum is Σ_{k even} and the odd sum Σ_{k odd} of the real factors of
// i^k, each over ν^k.
[[nodiscard]] inline BesZ bes_debye_sums(double q, double nu) noexcept
{
    const double w = -q * q;
    const double ratio = q / nu;
    double scale = 1.0; // (q/ν)^k
    double sign = 1.0;  // the sign of i^k for even k and of i^{k−1} for odd k: (−1)^⌊k/2⌋
    double even = 0.0;
    double odd = 0.0;
    for (int k = 0; k <= kBesDebyeOrder; ++k)
    {
        double poly = kBesDebyeTable.c[k][k];
        for (int j = k - 1; j >= 0; --j)
        {
            poly = poly * w + kBesDebyeTable.c[k][j];
        }
        const double term = sign * scale * poly;
        if ((k & 1) == 0)
        {
            even += term;
        }
        else
        {
            odd += term;
        }
        scale *= ratio;
        sign = ((k & 1) != 0) ? -sign : sign;
    }
    return {even, odd};
}

// J_ν(x) and Y_ν(x) for ν large and x well above ν (DLMF 10.19.6), with x = ν sec β:
//   J = (2/(πν tan β))^{½}(cos ξ·Σ_even + sin ξ·Σ_odd),  Y = (2/(πν tan β))^{½}(sin ξ·Σ_even − cos ξ·Σ_odd),
//   ξ = ν(tan β − β) − π/4 = √(x² − ν²) − ν·arccos(ν/x) − π/4.
// ξ is formed in double-double arithmetic: √(x²−ν²) from the exact differences, arccos from the double arccos corrected
// by one Newton step on the double-double sine and cosine of it (the residual is of the size of a rounding, so one step
// is exact to 1e-32), and ξ is reduced modulo 2π in double-double.
[[nodiscard]] inline BesHankelJY bes_debye_jy(double nu, double x) noexcept
{
    constexpr double kQuarterPiHi = 0.78539816339744830962;
    constexpr double kQuarterPiLo = 3.0616169978683829e-17;
    constexpr double kTwoPiHi = 6.2831853071795864769;
    constexpr double kTwoPiLo = 2.4492935982947064e-16;
    const BesDD root = bes_dd_sqrt(bes_dd_mul(bes_dd_two_sum(x, -nu), bes_dd_two_sum(x, nu))); // ν tan β
    const double angle = crd::math::atan2(root.hi, nu);
    BesDD sine;
    BesDD cosine;
    bes_dd_sincos(angle, sine, cosine);
    // tan(β − angle) = (root·cos − ν·sin)/(ν·cos + root·sin) is of the size of a rounding: the quotient is a double.
    const BesDD numerator = bes_dd_add(bes_dd_mul(root, cosine), bes_dd_mul_d(sine, -nu));
    const BesDD beta = bes_dd_two_sum(angle, numerator.hi / (nu * cosine.hi + root.hi * sine.hi));
    BesDD phase = bes_dd_add(root, bes_dd_mul_d(beta, -nu));
    phase = bes_dd_add(phase, BesDD{-kQuarterPiHi, -kQuarterPiLo});
    const double turns = crd::math::nearbyint(phase.hi / kTwoPiHi);
    phase = bes_dd_add(phase, bes_dd_mul_d(BesDD{-kTwoPiHi, -kTwoPiLo}, turns));
    double sin_hi;
    double cos_hi;
    bes_sincos(phase.hi, sin_hi, cos_hi);
    const double sin_xi = sin_hi + phase.lo * cos_hi;
    const double cos_xi = cos_hi - phase.lo * sin_hi;
    const BesZ sums = bes_debye_sums(nu / root.hi, nu);
    const double amplitude = crd::math::sqrt(2.0 / (kBesPi * root.hi));
    return {amplitude * (cos_xi * sums.re + sin_xi * sums.im), amplitude * (sin_xi * sums.re - cos_xi * sums.im), true};
}

// J, Y, J' and Y' for ν ≥ kBesForwardSteps and ν ≤ x, where the recurrence from the base order would need ν steps: the
// expansion of Debye at the orders ν₀ and ν₀+1, with ν₀ = ν − s the order s steps below (s the least integer, so that
// ν₀ keeps the fraction of ν, that puts ν₀ at least kBesDebyeMargin·ν^{1/3} below x); the recurrence in order, stable
// while the order is below x (DLMF 10.6.1), carries the pair up. Past kBesDebyeLimit the phase of Debye's expansion is
// no longer exact, and Hankel's expansion at the order itself is tried first.
[[nodiscard]] inline bool bes_jy_large_order(double x, double nu, BesJY& out) noexcept
{
    const double slope = nu / x;
    if (x >= kBesDebyeLimit)
    {
        // The pair at ν and ν+1 shares one phase (ω_{ν+1} = ω_ν − π/2), so that it stays a pair where ν+1 is no longer
        // a double different from ν (ν ≥ 2^53).
        const BesHankelPair pair = bes_hankel_base_pair(nu, x);
        if (pair.reliable)
        {
            out = {pair.j0, pair.y0, slope * pair.j0 - pair.j1, slope * pair.y0 - pair.y1};
            return true;
        }
    }
    if (!(x < kBesDebyeMax))
    {
        // Past the reach of Debye's expansion (its phase, formed in double-double arithmetic, would carry 1e-32·x, a
        // rounding of x's own digits of π) and of Hankel's at the order itself (ν² > 12x): this module does not
        // determine the value, and the answer is NaN, not the wrong one of the Steed fraction. The overflow classes
        // leave it alone (bes_overflow_domain).
        const double undefined = std::numeric_limits<double>::quiet_NaN();
        out = {undefined, undefined, undefined, undefined};
        return true;
    }
    const double margin = kBesDebyeMargin * crd::math::cbrt(nu);
    const double gap = x - nu;
    const int steps = (gap >= margin) ? 0 : static_cast<int>(margin - gap) + 1;
    const double start = nu - steps;
    const BesHankelJY low = bes_debye_jy(start, x);
    const BesHankelJY high = bes_debye_jy(start + 1.0, x);
    // The coefficient 2(ν₀+m)/x of the recurrence is (ν₀+m)·(2/x): the rounding of 2/x is the same in every step and,
    // near the turning point, amplified by 1/sin θ (the sum over the steps reached 1e-10 at ν = 5e7), and so is the
    // rounding of each product. Both are carried: 2/x as a pair of doubles, and each product with its error from one
    // fused multiply-add, so that the coefficient of a step is exact to a rounding of the pair.
    const double over_x = 2.0 / x;
    const double over_x_lo = crd::math::fma(-over_x, x, 2.0) / x;
    double j_low = low.j;
    double j_high = high.j;
    double y_low = low.y;
    double y_high = high.y;
    for (int m = 1; m <= steps; ++m)
    {
        const double order = start + m;
        const double coeff = order * over_x;
        const double coeff_lo = crd::math::fma(order, over_x, -coeff) + order * over_x_lo;
        const double j_up = (coeff * j_high - j_low) + coeff_lo * j_high;
        const double y_up = (coeff * y_high - y_low) + coeff_lo * y_high;
        j_low = j_high;
        j_high = j_up;
        y_low = y_high;
        y_high = y_up;
    }
    out = {j_low, y_low, slope * j_low - j_high, slope * y_low - y_high};
    return true;
}

// The ends of the range, where the Steed/Temme machinery below does not serve (they are kept out of line so that the
// core stays small enough to be inlined and, where only some of its results are used, trimmed):
//  * x ≥ kBesHankelFrom and ν ≤ x: Hankel's expansion at the base order μ = ν − ⌊ν⌋ and μ+1, carried up to ν and ν+1 by
//    the recurrence in order (DLMF 10.6.1), where the expansion has no hump however large ν is; used when it is
//    reliable;
//  * 0 < x < kBesSmallArg: J and J' from their series; Y and Y' from Temme's series at the base order, raised.
CRD_NOINLINE inline bool bes_jy_far(double x, double nu, BesJY& out) noexcept
{
    if (x >= kBesHankelFrom)
    {
        if (!(nu <= x))
        {
            return false; // from an order above x the fraction of the Steed method converges quickly
        }
        if (nu >= kBesForwardSteps)
        {
            return bes_jy_large_order(x, nu, out);
        }
        // Hankel's expansion at the base order μ and μ+1, then the recurrence in order up to ν (stable below x); the
        // slopes follow from J'_ν = (ν/x)J_ν − J_{ν+1} and the same for Y.
        const int steps = static_cast<int>(nu);
        const double mu = nu - steps;
        const BesHankelPair start = bes_hankel_base_pair(mu, x);
        if (!start.reliable)
        {
            return false;
        }
        const BesPair j = bes_forward_order(start.j0, start.j1, mu, steps, x);
        const BesPair y = bes_forward_order(start.y0, start.y1, mu, steps, x);
        const double slope = nu / x;
        out = {j.base, y.base, slope * j.base - j.next, slope * y.base - y.next};
        return true;
    }
    if (!(x > 0.0))
    {
        return false;
    }
    const int low_steps = bes_base_steps(nu);
    const double low_mu = nu - low_steps;
    const double low_inv_x = 1.0 / x;
    const BesPair y_top = bes_raise_from_base<true>(bes_temme_y(x, low_mu), low_mu, low_steps, 2.0 * low_inv_x);
    const BesPair y = bes_y_from_top(x, nu, low_inv_x, y_top);
    out = {besselj_series(nu, x), y.base, bes_ascending_slope(nu, x, -1.0), y.next};
    return true;
}

// J_ν(ζ) and Y_ν(ζ) at ζ = (2/3)a^{3/2} for Airy's negative axis (a = −x ≥ 0, ζ ≥ kBesHankelFrom), by Hankel's
// expansion.
// ζ is formed as a double-double (the square root and the products are corrected with fma), because its rounding,
// ε·ζ, would otherwise be the phase error: 1e-10 at x = −10^4. The result says whether the expansion can be trusted.
[[nodiscard]] inline BesHankelJY bes_jy_zeta(double a, double nu) noexcept
{
    const double root = crd::math::sqrt(a);
    const double root_lo = crd::math::fma(-root, root, a) / (2.0 * root);
    const double cube = a * root;
    const double cube_lo = crd::math::fma(a, root, -cube) + a * root_lo;
    const double zeta = (2.0 * cube) / 3.0;
    const double zeta_lo = (crd::math::fma(-3.0, zeta, 2.0 * cube) + 2.0 * cube_lo) / 3.0;
    return bes_hankel_jy(nu, zeta, zeta_lo);
}

// I and I' from their series, K and K' from Temme's series at the base order, raised: for 0 < x < kBesSmallArg.
CRD_NOINLINE inline BesIK bes_ik_small(double x, double nu) noexcept
{
    const int low_steps = bes_base_steps(nu);
    const double low_mu = nu - low_steps;
    const double low_inv_x = 1.0 / x;
    const BesPair k_top = bes_raise_from_base<false>(bes_temme_k(x, low_mu), low_mu, low_steps, 2.0 * low_inv_x);
    return {besseli_series(nu, x), k_top.base, bes_ascending_slope(nu, x, 1.0), bes_k_slope(x, nu, low_inv_x, k_top)};
}

// J_ν, Y_ν, J'_ν, Y'_ν for x > 0, ν ≥ 0 by the Steed/Temme method (Barnett, Feng, Steed and Goldfarb 1974; Temme 1976;
// Thompson & Barnett 1986): CF1 gives J'/J at order ν, a downward recurrence carries it to the base order μ, there
// Temme's series (x < 2) or Steed's CF2 (x ≥ 2) give Y_μ and Y_{μ+1} and, with the Wronskian J Y' − J' Y = 2/(πx), J_μ;
// J and J' at order ν are rescaled from the unnormalised recurrence, and Y is carried upward to order ν.
[[nodiscard]] CRD_FORCEINLINE BesJY bes_jy_core(double x, double nu) noexcept
{
    const int steps = bes_jy_steps(x, nu);
    const double mu = nu - steps;
    const double inv_x = 1.0 / x;
    const BesCf1 cf1 = bes_cf1_oscillating(inv_x, nu);
    const double seed = cf1.sign * kBesTiny; // J_ν, up to the scale fixed below
    const double seed_d = cf1.ratio * seed;  // J'_ν on the same scale
    const BesPair lowered = bes_lower_to_base<true>(seed, seed_d, nu, steps, inv_x);
    const double j_raw = (lowered.base == 0.0) ? kBesRelTol : lowered.base; // unnormalised J_μ
    const double log_deriv = lowered.next / j_raw;                          // J'_μ/J_μ
    const double wronskian = 2.0 * inv_x / kBesPi;                          // J_μ Y'_μ − J'_μ Y_μ
    double j_mu = 0.0;
    BesPair y{};
    if (x < kBesTemmeLimit)
    {
        y = bes_temme_y(x, mu);
        const double y_mu_d = mu * inv_x * y.base - y.next; // Y'_μ
        j_mu = wronskian / (y_mu_d - log_deriv * y.base);
    }
    else
    {
        // J' = pJ − qY and Y' = qJ + pY give Y/J = (p − f)/q, and the Wronskian q(J² + Y²) = 2/(πx).
        const BesZ pq = bes_cf2_hankel(x, mu * mu);
        const double y_over_j = (pq.re - log_deriv) / pq.im;
        const double mag = crd::math::sqrt(wronskian / (pq.im * (1.0 + y_over_j * y_over_j)));
        j_mu = (j_raw >= 0.0) ? crd::math::fabs(mag) : -crd::math::fabs(mag);
        const double y_mu_d = j_mu * (pq.im + pq.re * y_over_j);
        y.base = j_mu * y_over_j;
        y.next = mu * inv_x * y.base - y_mu_d;
    }
    const double rescale = j_mu / j_raw;
    const BesPair top = bes_raise_from_base<true>(y, mu, steps, 2.0 * inv_x);
    return {seed * rescale, top.base, seed_d * rescale, nu * inv_x * top.base - top.next};
}

// I_ν, K_ν, I'_ν, K'_ν for x > 0, ν ≥ 0 by the same scheme (Temme 1975; Thompson & Barnett 1986): CF1 for I'/I
// (Lentz 1976), downward recurrence to the base order μ, Temme's series (x < 2) or Steed's CF2 for K_μ and K_{μ+1},
// then I from the Wronskian I K' − I' K = −1/x, and K carried upward to order ν.
[[nodiscard]] inline BesIK bes_ik_core(double x, double nu) noexcept
{
    const int steps = bes_base_steps(nu);
    const double mu = nu - steps;
    const double inv_x = 1.0 / x;
    const double seed = kBesTiny; // I_ν, up to the scale fixed below
    const double seed_d = bes_cf1_growing(inv_x, nu) * seed;
    const BesPair lowered = bes_lower_to_base<false>(seed, seed_d, nu, steps, inv_x);
    const double log_deriv = lowered.next / lowered.base; // I'_μ/I_μ
    const BesPair k = (x < kBesTemmeLimit) ? bes_temme_k(x, mu) : bes_cf2_k(x, mu);
    const double k_mu_d = mu * inv_x * k.base - k.next;        // K'_μ
    const double i_mu = inv_x / (log_deriv * k.base - k_mu_d); // I_μ from the Wronskian
    const BesPair top = bes_raise_from_base<false>(k, mu, steps, 2.0 * inv_x);
    // The seed is 1e-300 and I_μ can be large, so the product is formed before the division by the raw I_μ.
    return {i_mu * seed / lowered.base, top.base, i_mu * seed_d / lowered.base, -(top.next - nu * inv_x * top.base)};
}

// I_ν or I'_ν alone, formed as bes_ik_core forms them (the same operations, so the same bits), without the upward
// recurrence for K and the slope of K that only K and K' need. It returns a double, so the public function that calls
// it needs no frame to hold a result.
template <bool Slope> [[nodiscard]] inline double bes_i_part(double x, double nu) noexcept
{
    const int steps = bes_base_steps(nu);
    const double mu = nu - steps;
    const double inv_x = 1.0 / x;
    const double seed = kBesTiny; // I_ν, up to the scale fixed below
    const double seed_d = bes_cf1_growing(inv_x, nu) * seed;
    if (seed_d != seed_d) [[unlikely]]
    {
        return seed_d; // the order is negative: nothing of the rest is of use
    }
    const BesPair lowered = bes_lower_to_base<false>(seed, seed_d, nu, steps, inv_x);
    const double log_deriv = lowered.next / lowered.base; // I'_μ/I_μ
    BesPair k;
    if (x < kBesTemmeLimit)
    {
        if (x < kBesSmallArg) [[unlikely]]
        {
            return std::numeric_limits<double>::quiet_NaN(); // the series answers there
        }
        k = bes_temme_k(x, mu);
    }
    else
    {
        k = bes_cf2_k(x, mu);
    }
    const double k_mu_d = mu * inv_x * k.base - k.next;        // K'_μ
    const double i_mu = inv_x / (log_deriv * k.base - k_mu_d); // I_μ from the Wronskian
    return Slope ? i_mu * seed_d / lowered.base : i_mu * seed / lowered.base;
}

// The entry points of the public functions. They put the two ends of the range around the cores, which stay as they
// were (see the Airy note: an unused result of an inlined core drops out only while the core is left alone). Both are
// forced inline, and the public functions that call them are declared inline: as a call of its own, the entry point put
// a layer between the public function and the core, and a caller with a constant order (the usual call) then no longer
// let the compiler specialise the core for that order, which cost 3 to 15 % on J', Y', I and K'.
[[nodiscard]] CRD_FORCEINLINE BesJY bes_jy(double x, double nu) noexcept
{
    if (x >= kBesHankelFrom || x < kBesSmallArg) [[unlikely]]
    {
        BesJY edge;
        if (bes_jy_far(x, nu, edge))
        {
            return edge;
        }
    }
    return bes_jy_core(x, nu);
}
[[nodiscard]] inline BesIK bes_ik(double x, double nu) noexcept
{
    if (x < kBesTemmeLimit) // one comparison for every x ≥ 2
    {
        if (x < kBesSmallArg && x > 0.0) [[unlikely]]
        {
            return bes_ik_small(x, nu);
        }
    }
    return bes_ik_core(x, nu);
}

// {Y_ν, Y'_ν} for kBesSmallArg ≤ x < kBesTemmeLimit alone: Temme's series for Y at the base order and the recurrence in
// order, as bes_jy_core forms them (the same operations, so the same bits), without the continued fraction for J and
// the Wronskian that only J and J' need.
[[nodiscard]] inline BesPair bes_y_small(double x, double nu) noexcept
{
    const int steps = bes_base_steps(nu);
    const double mu = nu - steps;
    const double inv_x = 1.0 / x;
    const BesPair top = bes_raise_from_base<true>(bes_temme_y(x, mu), mu, steps, 2.0 * inv_x);
    return {top.base, nu * inv_x * top.base - top.next};
}
[[nodiscard]] inline BesPair bes_y_pair(double x, double nu) noexcept
{
    if (x < kBesTemmeLimit && x >= kBesSmallArg)
    {
        return bes_y_small(x, nu);
    }
    const BesJY jy = bes_jy(x, nu);
    return {jy.y, jy.yp};
}

// {K_ν, K'_ν} alone, as the base-order pair and the recurrence in order of bes_ik_core produce them (the same
// operations, so the same bits), without the continued fraction for I that only I and I' need. Temme's series serves
// every x < 2, down to the subnormals, so K needs no entry point of its own for a small argument. The series is run out
// of line: it costs the one call where K and K' now save the continued fraction for I, and the loop of CF2 that every
// x >= 2 runs stays small enough to be inlined into the caller (an inlined series made the caller too big for that).
CRD_NOINLINE inline BesPair bes_temme_k_call(double x, double mu) noexcept
{
    return bes_temme_k(x, mu);
}
// K' < 0 for every x > 0, so the slope is formed as (ν/x)K_ν − K_{ν+1} and bes_kp_value makes a zero of it −0.
[[nodiscard]] inline BesPair bes_k_core(double x, double nu) noexcept
{
    const int steps = bes_base_steps(nu);
    const double mu = nu - steps;
    const double inv_x = 1.0 / x;
    const BesPair k = (x < kBesTemmeLimit) ? bes_temme_k_call(x, mu) : bes_cf2_k(x, mu);
    const BesPair top = bes_raise_from_base<false>(k, mu, steps, 2.0 * inv_x);
    return {top.base, nu * inv_x * top.base - top.next};
}

// The overflow classes, applied where a result is used. When the upward recurrence of Y or K, or the slope formed from
// it, overflows, it leaves ∞ − ∞ = NaN behind: Y_ν(x) is then −∞ (it is negative and growing for ν > x) and Y'_ν = +∞,
// K_ν = +∞ and K'_ν = −∞. Past the overflow of e^x the raw I_μ of the core is infinite and its ratios end in ∞/∞:
// I_ν and I'_ν are +∞. (Kept out of the cores so that their results can still be dropped, unused, where only part of
// them is wanted.) The NaN of a negative, infinite or NaN argument, or of an infinite order, is not an overflow and
// stays NaN, and so does the NaN that bes_jy_large_order answers for x ≥ kBesDebyeMax and ν ≤ x (undetermined).
[[nodiscard]] inline bool bes_overflow_domain(double x, double nu) noexcept
{
    return x > 0.0 && x - x == 0.0 && nu - nu == 0.0 && !(x >= kBesDebyeMax && nu <= x);
}
[[nodiscard]] inline double bes_y_class(double y, double x, double nu) noexcept
{
    if (y != y) [[unlikely]]
    {
        return bes_overflow_domain(x, nu) ? -std::numeric_limits<double>::infinity() : y;
    }
    return y;
}
[[nodiscard]] inline double bes_yp_class(double yp, double x, double nu) noexcept
{
    if (yp != yp) [[unlikely]]
    {
        return bes_overflow_domain(x, nu) ? std::numeric_limits<double>::infinity() : yp;
    }
    return yp;
}
[[nodiscard]] inline double bes_kp_class(double kp, double x, double nu) noexcept
{
    if (kp != kp) [[unlikely]]
    {
        return bes_overflow_domain(x, nu) ? -std::numeric_limits<double>::infinity() : kp;
    }
    return kp;
}
// K'_ν of bes_k_core as the public function returns it. K' < 0 for every x > 0, so a slope that is not below zero is
// not a plain result: a zero is a slope that underflowed, −0, and a NaN goes through the overflow class. One comparison
// serves the common case, which is cheaper than forming −(K_{ν+1} − (ν/x)K_ν) with its sign flip.
[[nodiscard]] inline double bes_kp_value(double kp, double x, double nu) noexcept
{
    if (kp < 0.0) [[likely]]
    {
        return kp;
    }
    return (kp == 0.0) ? -0.0 : bes_kp_class(kp, x, nu);
}

// J_ν from the Hankel expansion above, for the fast path: the value and whether the expansion can be trusted.
struct BesAsymp
{
    double value;
    bool reliable;
};
[[nodiscard]] inline BesAsymp besselj_asymp(double nu, double x) noexcept
{
    const BesHankel h = bes_hankel_pq(nu, x);
    const BesTrig t = bes_hankel_phase(nu, x);
    const double value = crd::math::sqrt(2.0 / (kBesPi * x)) * (t.cos * h.p - t.sin * h.q);
    return {value, h.reliable};
}
// J_ν from Hankel's expansion at the base order μ and μ+1 and the recurrence in order up to ν (for an order whose own
// expansion would have a hump; stable while ν ≤ x).
[[nodiscard]] inline BesAsymp besselj_forward(double nu, double x) noexcept
{
    const int steps = static_cast<int>(nu);
    const double mu = nu - steps;
    const BesHankel low = bes_hankel_pq(mu, x);
    const BesHankel high = bes_hankel_pq(mu + 1.0, x);
    const BesTrig t = bes_hankel_phase(mu, x);
    const double amplitude = crd::math::sqrt(2.0 / (kBesPi * x));
    const BesPair top = bes_forward_order(amplitude * (t.cos * low.p - t.sin * low.q),
                                          amplitude * (t.sin * high.p + t.cos * high.q), mu, steps, x);
    return {top.base, low.reliable && high.reliable};
}
// The expansion at the order itself has a hump of about e^{ν²/2x} (its terms first grow, which costs digits and terms);
// up to ν² = 12x (a hump of e^6) it is summed directly, beyond it the expansion at the base order and the recurrence
// take over.
inline constexpr double kBesHumpLimit = 12.0;
[[nodiscard]] inline double besselj_real(double nu, double x) noexcept
{
    if (crd::math::fabs(x - 4.5009765625) <= 4.4990234375) [[likely]] // 2^-9 ≤ x ≤ 9: no subnormal x/2
    {
        // small x: series accurate to ~1e-13 (cancellation grows beyond)
        return bes_ascending_series<false>(nu, x, -1.0);
    }
    if (x <= 9.0)
    {
        return besselj_series(nu, x);
    }
    if (x >= 17.5 && x >= 2.0 * nu)
    {
        // x ≫ ν: Hankel asymptotic, when its terms allow it
        const BesAsymp tail = (nu * nu < kBesHumpLimit * x) ? besselj_asymp(nu, x)
                              : (nu < kBesForwardSteps)     ? besselj_forward(nu, x)
                                                            : BesAsymp{0.0, false};
        if (tail.reliable)
        {
            return tail.value;
        }
    }
    // hard middle / x≈ν region, or an asymptotic whose terms do not reach the accuracy: fall back to the robust
    // Steed/Temme (still correct, just not faster); x > 9 here, so only the far end of the range needs a look first
    if (x >= kBesHankelFrom) [[unlikely]]
    {
        BesJY edge;
        if (bes_jy_far(x, nu, edge))
        {
            return edge.j;
        }
    }
    return bes_jy_core(x, nu).j;
}

// sin(πa) and cos(πa) for a > 0 with the argument reduced before any rounding: the offset of a from the nearest quarter
// turn is exact, so an integer a gives sin = ±0 and a half-integer a gives cos = ±0 exactly (the product π·a rounded
// first would leave ~1e-16 there, and that residue multiplies a Y or K that is huge at small x).
[[nodiscard]] inline BesTrig bes_sin_cos_pi(double a) noexcept
{
    if (!(a < kBesWholeTurns))
    {
        // Every double this large is an even integer; only an infinity or a NaN has no sine.
        return (a - a == 0.0) ? BesTrig{0.0, 1.0} : BesTrig{a - a, a - a};
    }
    const long long quarters = static_cast<long long>(2.0 * a + 0.5); // the nearest quarter turn
    const double rest = a - 0.5 * static_cast<double>(quarters);      // in [−¼, ¼], exact
    // An integer or half-integer a leaves no offset: the sine and cosine are 0 and 1 exactly, with no polynomial to
    // sum.
    double s = 0.0;
    double c = 1.0;
    if (rest != 0.0)
    {
        const double angle = kBesPi * rest; // |angle| ≤ π/4
        s = bes_sin_reduced(angle);
        c = bes_cos_reduced(angle);
    }
    const int phase = static_cast<int>(quarters & 3);
    if (phase == 0)
    {
        return {s, c};
    }
    if (phase == 1)
    {
        return {c, -s};
    }
    if (phase == 2)
    {
        return {-s, -c};
    }
    return {-c, s};
}

// sin(πa) alone (see above), for the reflection of I that needs no cosine.
[[nodiscard]] inline double bes_sin_pi(double a) noexcept
{
    if (!(a < kBesWholeTurns))
    {
        return (a - a == 0.0) ? 0.0 : a - a;
    }
    const long long quarters = static_cast<long long>(2.0 * a + 0.5);
    const double rest = a - 0.5 * static_cast<double>(quarters);
    const int phase = static_cast<int>(quarters & 3);
    if (rest == 0.0)
    {
        // An integer or half-integer a: the sine is 0 or ±1 exactly, with no polynomial to sum.
        static constexpr double kExact[4] = {0.0, 1.0, -0.0, -1.0};
        return kExact[phase];
    }
    const double angle = kBesPi * rest;
    if ((phase & 1) == 0)
    {
        return (phase == 0) ? bes_sin_reduced(angle) : -bes_sin_reduced(angle);
    }
    return (phase == 1) ? bes_cos_reduced(angle) : -bes_cos_reduced(angle);
}

// I_ν (Slope false) or I'_ν for x ≥ kBesTemmeLimit, ν ≥ 0, by the scheme of the core, where the core's recurrence or
// its e^{±x} has left the doubles: the downward recurrence starts from 1 and is scaled down by exactly 2^−800 whenever
// it grows past 2^800 (a NaN of the core means more than 2000 binary orders over the seed of 1e-300), I_μ comes from
// the Wronskian with the sums of CF2, which do not hold e^{−x}, and e^{x} is applied last as 2^q·e^{r}, |r| ≤ ½ ln 2.
inline constexpr double kBesRescaleFrom = 0x1p800;
inline constexpr double kBesRescaleBy = 0x1p-800;
inline constexpr int kBesRescaleBits = 800;
inline constexpr double kBesLn2Hi = 6.93147180369123816490e-01; // ln 2 in two parts: the high part has 33 bits
inline constexpr double kBesLn2Lo = 1.90821492927058770002e-10;
inline constexpr double kBesRescaledMax = 2.0e9; // the recurrence counts its steps in an int
[[nodiscard]] inline double bes_power_of_two(int power) noexcept
{
    return std::bit_cast<double>(static_cast<long long>(power + 1023) << 52);
}
template <bool Slope> CRD_NOINLINE inline double bes_i_rescaled(double x, double nu) noexcept
{
    const int steps = bes_base_steps(nu);
    const double mu = nu - steps;
    const double inv_x = 1.0 / x;
    const double cf1 = bes_cf1_growing(inv_x, nu); // I'_ν/I_ν
    double base = 1.0;                             // I_μ on the scale I_ν = 1, times 2^(−800·scalings)
    double slope = cf1;                            // I'_μ on that scale
    int scalings = 0;
    if (steps > 0)
    {
        const double two_inv_x = 2.0 * inv_x;
        double upper = 1.0;
        base = nu * inv_x + cf1;
        for (int j = steps - 1; j >= 1; --j)
        {
            const double below = (mu + j) * two_inv_x * base + upper;
            upper = base;
            base = below;
            if (base > kBesRescaleFrom)
            {
                base *= kBesRescaleBy;
                upper *= kBesRescaleBy;
                ++scalings;
            }
        }
        slope = mu * inv_x * base + upper;
    }
    const BesCf2Sums sums = bes_cf2_k_sums(x, mu);
    const double k_base = crd::math::sqrt(kBesPi / (2.0 * x)) / sums.s;                     // K_μ e^{x}
    const double k_next = k_base * (mu + x + 0.5 - sums.a1 * sums.h) * inv_x;               // K_{μ+1} e^{x}
    const double i_mu = inv_x / ((slope / base) * k_base - (mu * inv_x * k_base - k_next)); // I_μ e^{−x}
    const double q = crd::math::round(x / (kBesLn2Hi + kBesLn2Lo));
    const double r = (x - q * kBesLn2Hi) - q * kBesLn2Lo;
    double value = (i_mu / base) * crd::math::exp(r) * (Slope ? cf1 : 1.0);
    int power = static_cast<int>(q) - kBesRescaleBits * scalings;
    for (; power > 1000; power -= 1000)
    {
        value *= 0x1p1000;
    }
    for (; power < -1000; power += 1000)
    {
        value *= 0x1p-1000;
    }
    return value * bes_power_of_two(power);
}

// The classes of a NaN of the core for I and I'. The leading term of Debye's uniform expansion (DLMF 10.41.3), which
// holds for every ratio of ν and x, gives the size: I_ν(x) ≈ e^{√(ν²+x²) + ν ln(x/(ν+√(ν²+x²)))}/√(2π√(ν²+x²)). Above
// e^780 the value is +∞, below e^{−800} it is 0, and between the two the rescaled method above serves; where it cannot
// (x < 2, whose NaN is an underflow far below 1e-600 in any case, or an order or argument beyond its reach) the NaN
// stays.
inline constexpr double kBesClassOver = 780.0;
inline constexpr double kBesClassUnder = -800.0;
template <bool Slope> CRD_NOINLINE inline double bes_i_class_of_nan(double value, double x, double nu) noexcept
{
    if (!(x >= kBesSmallArg && nu - nu == 0.0))
    {
        return value;
    }
    if (x - x != 0.0)
    {
        return std::numeric_limits<double>::infinity();
    }
    const double order = crd::math::fabs(nu);
    const double root = crd::math::hypot(order, x);
    const double ratio = (x / root) / (1.0 + order / root); // x/(ν + √(ν²+x²)), which does not overflow
    const double size = root + order * crd::math::log(ratio) - 0.5 * crd::math::log(2.0 * kBesPi * root);
    if (size > kBesClassOver)
    {
        return std::numeric_limits<double>::infinity();
    }
    if (size < kBesClassUnder)
    {
        return 0.0;
    }
    if (x >= kBesTemmeLimit && x < kBesRescaledMax && order < kBesRescaledMax)
    {
        return bes_i_rescaled<Slope>(x, order);
    }
    return value;
}
template <bool Slope> [[nodiscard]] inline double bes_i_class(double value, double x, double nu) noexcept
{
    if (value != value) [[unlikely]]
    {
        return bes_i_class_of_nan<Slope>(value, x, nu);
    }
    return value;
}

// The public functions of I and I' call the core first and test afterwards: I and I' are positive, and for ν ≥ 0 and
// x ≥ kBesSmallArg a positive result is the answer. Anything else is left to the out-of-line functions: the NaN of an
// overflow or underflow of the recurrence (bes_i_class turns it into the infinity or the zero it stands for), and the
// NaN that the core answers at once for an argument below kBesSmallArg, where the series serves, and for a negative
// order, which is reflected. A test before the call, which holds up the core in a loop over the
// argument, cost 3 % on I' at a constant order (the usual call): such a loop then no longer gets the work that depends
// on the order done once. Y, Y' and J' keep their tests in front, since their core must not run where the Hankel
// expansion or the series answer (it needs x steps of a fraction at a large x): one comparison for the order, 0 ≤ ν ≤
// kBesPlainOrder, and one for the argument, each a distance from the middle of the interval, which a NaN fails.
inline constexpr double kBesPlainOrder = 100.0;
[[nodiscard]] CRD_FORCEINLINE bool bes_order_plain(double nu) noexcept
{
    return crd::math::fabs(nu - 0.5 * kBesPlainOrder) <= 0.5 * kBesPlainOrder;
}
CRD_NOINLINE inline BesIK bes_ik_classed(double x, double nu) noexcept
{
    BesIK ik = bes_ik(x, nu);
    ik.i = bes_i_class<false>(ik.i, x, nu);
    ik.ip = bes_i_class<true>(ik.ip, x, nu);
    return ik;
}

// c·v for a reflection coefficient c: an exactly zero c drops the term even when v has overflowed to ±∞.
[[nodiscard]] inline double bes_weighted(double c, double v) noexcept
{
    if (c == 0.0) [[unlikely]]
    {
        return 0.0;
    }
    return c * v;
}

// J_ν and Y_ν (re, im) for any real order from one core, with the overflow classes and the reflection of a negative
// order.
[[nodiscard]] inline BesZ bes_jy_pair(double x, double nu) noexcept
{
    if (nu >= 0.0)
    {
        const BesJY jy = bes_jy(x, nu);
        return {jy.j, bes_y_class(jy.y, x, nu)};
    }
    const BesJY jy = bes_jy(x, -nu);
    const BesTrig t = bes_sin_cos_pi(-nu);
    const double y = bes_y_class(jy.y, x, nu);
    return {bes_weighted(t.cos, jy.j) - bes_weighted(t.sin, y), bes_weighted(t.sin, jy.j) + bes_weighted(t.cos, y)};
}

// Order-reflection for negative ν: J_{-ν} = cos(νπ)J_ν − sin(νπ)Y_ν ; Y_{-ν} = sin(νπ)J_ν + cos(νπ)Y_ν.
// I_{-ν} = I_ν + (2/π)sin(νπ)K_ν ; K_{-ν} = K_ν. (Used so the public API accepts any real order; a NaN order, which
// fails ν ≥ 0, comes here as well.) The reflections of I and I' are inline: out of line they took the order from the
// caller, which knows it (a constant in the usual call) and so no longer specialises the core for it, 6 to 9 % on I of
// a negative order, where base reflects in line.
// K_ν of order ≥ 100 is 0 here from x of about 742 on (the upward recurrence starts from e^{−x} scaled below the
// doubles), so a reflection through K has lost the term that carries the value: there I and I' return what base
// returned, the NaN or the infinity of its core, and never a finite number, until the scale is carried through K as
// well.
inline constexpr double kBesReflectedLost = -100.0;
CRD_NOINLINE inline double bes_ip_as_base(double nu, double x) noexcept
{
    const double inv_x = 1.0 / x;
    const int steps = bes_base_steps(nu);
    const double mu = nu - steps;
    const double spacing = 2.0 * inv_x;
    double value = nu * inv_x;
    value = (value < kBesTiny) ? kBesTiny : value;
    double num_tail = value;
    double den_tail = 0.0;
    double index = 0.0;
    for (int step = 1; step <= kBesIterCap; ++step)
    {
        index += 1.0;
        const double b = spacing * (nu + index);
        den_tail = 1.0 / (b + den_tail);
        num_tail = b + 1.0 / num_tail;
        const double factor = num_tail * den_tail;
        value *= factor;
        if (crd::math::fabs(factor - 1.0) < kBesRelTol)
        {
            break;
        }
    }
    const double seed = kBesTiny;
    const double seed_d = value * seed;
    const BesPair lowered = bes_lower_to_base<false>(seed, seed_d, nu, steps, inv_x);
    const double log_deriv = lowered.next / lowered.base;
    const BesPair k = (x < kBesTemmeLimit) ? bes_temme_k(x, mu) : bes_cf2_k(x, mu);
    const double i_mu = inv_x / (log_deriv * k.base - (mu * inv_x * k.base - k.next));
    return i_mu * seed_d / lowered.base;
}
CRD_NOINLINE inline double bes_i_reflected_general(double n, double x) noexcept
{
    const BesIK ik = bes_ik(x, -n);
    if (ik.k == 0.0 && n <= kBesReflectedLost) [[unlikely]]
    {
        return ik.i + (2.0 / kBesPi) * crd::math::sin(-n * kBesPi) * ik.k;
    }
    return bes_i_class<false>(ik.i, x, n) + bes_weighted((2.0 / kBesPi) * bes_sin_pi(-n), ik.k);
}
CRD_FORCEINLINE double bes_i_reflected(double n, double x) noexcept
{
    if (x >= kBesSmallArg)
    {
        // The core of base, called as base calls it from 2^-9 on (no entry point between): the sum is the one of the
        // general path with the sine of bes_sin_pi; a NaN (an overflow or underflow that the classes decide) is left to
        // that path, out of line so that the core is in the caller once.
        const BesIK core = bes_ik_core(x, -n);
        const double direct = core.i + (2.0 / kBesPi) * bes_sin_pi(-n) * core.k;
        if (direct == direct) [[likely]]
        {
            return direct;
        }
    }
    return bes_i_reflected_general(n, x);
}
inline double bes_ip_reflected(double n, double x) noexcept
{
    const BesIK ik = bes_ik(x, -n);
    if (ik.k == 0.0 && n <= kBesReflectedLost) [[unlikely]]
    {
        return bes_ip_as_base(n, x);
    }
    return bes_i_class<true>(ik.ip, x, n) + bes_weighted((2.0 / kBesPi) * bes_sin_pi(-n), bes_kp_class(ik.kp, x, n));
}
// I and I' where the core's result is not the answer, as the public functions return them: the series below
// kBesSmallArg and the classes of a NaN result for ν ≥ 0, and the reflection of a negative or NaN order.
CRD_NOINLINE inline double bes_i_outside(double n, double x) noexcept
{
    return (n >= 0.0) ? bes_ik_classed(x, n).i : bes_i_reflected_general(n, x);
}
CRD_NOINLINE inline double bes_ip_outside(double n, double x) noexcept
{
    return (n >= 0.0) ? bes_ik_classed(x, n).ip : bes_ip_reflected(n, x);
}

// J', Y and Y' call the core itself where neither end of its range is needed and no overflow class can arise, one
// comparison each for the order (0 ≤ ν ≤ 100, above) and the argument. Where x > 2 and
// ν ≤ 100 the upward recurrence of Y stays far below 1e308 (Y_100(2) is 1e158), so the raw results are the answers.
// Elsewhere the out-of-line functions below answer, with the ends of the range, the classes and the reflection of a
// negative order (a NaN order or argument fails the tests and comes here too). The argument ranges are written as a
// distance from the middle of the interval with exact middle and half-width, J' inside [kBesSmallArg, kBesHankelFrom).
[[nodiscard]] CRD_FORCEINLINE bool bes_jp_arg_plain(double x) noexcept
{
    return crd::math::fabs(x - 500.0) < 499.998046875; // 2^-9 < x < 1000 − 2^-9
}
[[nodiscard]] CRD_FORCEINLINE bool bes_y_arg_plain(double x) noexcept
{
    return crd::math::fabs(x - 501.0) < 499.0; // 2 < x < 1000
}
CRD_NOINLINE inline double bes_jp_outside(double n, double x) noexcept
{
    if (n >= 0.0)
    {
        return bes_jy(x, n).jp;
    }
    const BesJY jy = bes_jy(x, -n);
    const BesTrig t = bes_sin_cos_pi(-n);
    return bes_weighted(t.cos, jy.jp) - bes_weighted(t.sin, bes_yp_class(jy.yp, x, n));
}
CRD_NOINLINE inline double bes_y_outside(double n, double x) noexcept
{
    if (n >= 0.0)
    {
        return bes_y_class(bes_y_pair(x, n).base, x, n);
    }
    const BesJY jy = bes_jy(x, -n);
    const BesTrig t = bes_sin_cos_pi(-n);
    return bes_weighted(t.sin, jy.j) + bes_weighted(t.cos, bes_y_class(jy.y, x, n));
}
CRD_NOINLINE inline double bes_yp_outside(double n, double x) noexcept
{
    if (n >= 0.0)
    {
        return bes_yp_class(bes_y_pair(x, n).next, x, n);
    }
    const BesJY jy = bes_jy(x, -n);
    const BesTrig t = bes_sin_cos_pi(-n);
    return bes_weighted(t.sin, jy.jp) + bes_weighted(t.cos, bes_yp_class(jy.yp, x, n));
}
} // namespace detail

// ---- cylindrical Bessel (real order ν, x > 0) ----
template <Real T> [[nodiscard]] inline T cyl_bessel_j(T nu, T x) noexcept
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    if (n >= 0.0)
    {
        return static_cast<T>(detail::besselj_real(n, xd)); // dedicated J-only fast path
    }
    const detail::BesJY jy = detail::bes_jy(xd, -n);
    const detail::BesTrig t = detail::bes_sin_cos_pi(-n);
    return static_cast<T>(detail::bes_weighted(t.cos, jy.j) -
                          detail::bes_weighted(t.sin, detail::bes_y_class(jy.y, xd, n)));
}

template <Real T> [[nodiscard]] inline T cyl_neumann(T nu, T x) noexcept // Y_ν
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    if (detail::bes_order_plain(n) & detail::bes_y_arg_plain(xd)) [[likely]]
    {
        return static_cast<T>(detail::bes_jy_core(xd, n).y);
    }
    return static_cast<T>(detail::bes_y_outside(n, xd));
}

template <Real T> [[nodiscard]] inline T cyl_bessel_i(T nu, T x) noexcept
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    const double value = detail::bes_i_part<false>(xd, n);
    if (value > 0.0) [[likely]]
    {
        return static_cast<T>(value);
    }
    return static_cast<T>((n < 0.0) ? detail::bes_i_reflected(n, xd) : detail::bes_i_outside(n, xd));
}

template <Real T> [[nodiscard]] inline T cyl_bessel_k(T nu, T x) noexcept
{
    const double n = crd::math::fabs(static_cast<double>(nu)); // K_{-ν} = K_ν
    const double xd = static_cast<double>(x);
    return static_cast<T>(detail::bes_k_core(xd, n).base);
}

// ---- derivatives (real order ν, x > 0) ----
// A negative order is reflected like the values above: the reflection coefficients do not depend on x, so they carry
// over to the derivatives unchanged (K'_{−ν} = K'_ν).
template <Real T> [[nodiscard]] inline T cyl_bessel_j_prime(T nu, T x) noexcept
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    if ((n >= 0.0) & detail::bes_jp_arg_plain(xd)) [[likely]]
    {
        return static_cast<T>(detail::bes_jy_core(xd, n).jp);
    }
    return static_cast<T>(detail::bes_jp_outside(n, xd));
}
template <Real T> [[nodiscard]] inline T cyl_neumann_prime(T nu, T x) noexcept
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    if (detail::bes_order_plain(n) & detail::bes_y_arg_plain(xd)) [[likely]]
    {
        return static_cast<T>(detail::bes_jy_core(xd, n).yp);
    }
    return static_cast<T>(detail::bes_yp_outside(n, xd));
}
template <Real T> [[nodiscard]] inline T cyl_bessel_i_prime(T nu, T x) noexcept
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    const double value = detail::bes_i_part<true>(xd, n);
    if (value > 0.0) [[likely]]
    {
        return static_cast<T>(value);
    }
    return static_cast<T>((n < 0.0) ? detail::bes_ip_reflected(n, xd) : detail::bes_ip_outside(n, xd));
}
template <Real T> [[nodiscard]] inline T cyl_bessel_k_prime(T nu, T x) noexcept
{
    const double n = crd::math::fabs(static_cast<double>(nu)); // K'_{−ν} = K'_ν
    const double xd = static_cast<double>(x);
    return static_cast<T>(detail::bes_kp_value(detail::bes_k_core(xd, n).next, xd, n));
}

// ---- spherical Bessel j_n, y_n (integer n ≥ 0, x > 0) via the stable recurrences ----
// j_0 = sin x / x, j_1 = sin x / x² − cos x / x; y_0 = −cos x / x, y_1 = −cos x / x² − sin x / x.
// y_n: upward recurrence (stable). j_n: upward is stable for x ≥ n; otherwise use j_n = √(π/2x) J_{n+½}.
template <Real T> [[nodiscard]] T sph_bessel(int n, T x) noexcept
{
    const double xd = static_cast<double>(x);
    if (static_cast<double>(n) <= xd) // upward recurrence stable
    {
        double j0 = crd::math::sin(xd) / xd;
        if (n == 0)
        {
            return static_cast<T>(j0);
        }
        double j1 = crd::math::sin(xd) / (xd * xd) - crd::math::cos(xd) / xd;
        for (int k = 1; k < n; ++k)
        {
            const double jn = (2.0 * k + 1.0) / xd * j1 - j0;
            j0 = j1;
            j1 = jn;
        }
        return static_cast<T>(j1);
    }
    // j_n = √(π/2x) J_{n+½}
    return static_cast<T>(crd::math::sqrt(detail::kBesPi / (2.0 * xd)) * detail::bes_jy(xd, n + 0.5).j);
}

template <Real T> [[nodiscard]] T sph_neumann(int n, T x) noexcept // y_n
{
    const double xd = static_cast<double>(x);
    double y0 = -crd::math::cos(xd) / xd;
    if (n == 0)
    {
        return static_cast<T>(y0);
    }
    double y1 = -crd::math::cos(xd) / (xd * xd) - crd::math::sin(xd) / xd;
    for (int k = 1; k < n; ++k)
    {
        const double yn = (2.0 * k + 1.0) / xd * y1 - y0;
        y0 = y1;
        y1 = yn;
    }
    return static_cast<T>(y1);
}

// ---- Hankel functions H^(1,2)_ν(x) = J_ν(x) ± i Y_ν(x) (real order, x > 0) ----
// J_ν and Y_ν together from one core (the reflection for a negative order shares it too).
template <Real T> [[nodiscard]] std::complex<T> cyl_hankel_1(T nu, T x) noexcept
{
    const detail::BesZ jy = detail::bes_jy_pair(static_cast<double>(x), static_cast<double>(nu));
    return {static_cast<T>(jy.re), static_cast<T>(jy.im)};
}
template <Real T> [[nodiscard]] std::complex<T> cyl_hankel_2(T nu, T x) noexcept
{
    const detail::BesZ jy = detail::bes_jy_pair(static_cast<double>(x), static_cast<double>(nu));
    return {static_cast<T>(jy.re), static_cast<T>(-jy.im)};
}

// =====================================================================================================
// Complex argument z (real order ν). J/I via ascending series (moderate |z|) + Hankel/I-K asymptotic (large |z|);
// Y/K via the connection formulae (non-integer ν) or the large-|z| asymptotic. Gated vs scipy.special over the
// complex plane. Branch: principal (arg z ∈ (−π,π]). For integer ν the connection 0/0 is taken at ν+ε (≈1e-7 there;
// full accuracy for non-integer ν and for all ν via the asymptotic at large |z|). The series and the asymptotic each
// have an error that can be estimated (the series by the cancellation of its terms, the K connection by e^{|z|+Re z};
// the asymptotic by its smallest term and by the rounding of its largest), and the one with the smaller estimate is
// used: for ν ≪ |z| that is the series below |z| ≈ 12, for a large ν the asymptotic from far smaller |z|/ν than the old
// |z| ≥ 2ν rule. Where the two meet the best of them is ~1e-10 for ν ≲ 10. From |z| = 15 on, where neither is at the
// rounding (ν ≳ 35 near |z| ≈ ν was 1e-7), a third method is priced as well: Hankel's expansion at the base order μ and
// μ+1, carried up to ν by the recurrence in order (J and Y); its error is the start's (about e^{−2|z|}) times the
// growth of the recurrence, e^{2(Im z − Im φ_ν(z))} with Debye's phase φ, which stays near 1 while ν ≲ Re z. K between
// |z| = 8 and the rounding of its asymptotic (K_5(9+0.5i) was 1.6e-7 off, where the connection formula loses e^{2|z|}ε)
// comes from Steed's CF2 at the base order and the recurrence, stable upward for K. Left of the imaginary axis J, Y and
// K are reflected to the right half-plane (DLMF 10.11, 10.34). Where that recurrence itself loses more than 1e-10 (ν ≳
// ¾ Re z and Im z ≳ 6: it amplifies the start by e^{2(Im z − Im φ_ν)}, 1e5 at J_35.2(45+20i), 1e11 at J_50(60+30i),
// where it was 1e-5 off), J and Y come from the downward recurrence of J's continued fraction (stable for the solution
// that is minimal as the order grows) normalised by the Wronskian with H^(1), which is K at the rotated argument −iz
// (DLMF 10.27.8) from Steed's CF2 and the upward recurrence.
// =====================================================================================================
namespace detail
{
using cplx = std::complex<double>;

// J (kind=−1) or I (kind=+1) ascending series: Σ (kind·(z/2)²)^k (z/2)^ν / (k! Γ(ν+k+1)).
[[nodiscard]] inline cplx bessel_ji_series(double nu, cplx z, double kind) noexcept
{
    const cplx cz = 0.5 * z;
    cplx t = crd::math::pow(cz, nu) / gamma(nu + 1.0); // (z/2)^ν / Γ(ν+1); SIGNED Γ (ν may be < 0 ⇒ Γ(ν+1) < 0)
    cplx sum = t;
    const cplx z2 = kind * (cz * cz);
    for (int k = 1; k <= 400; ++k)
    {
        t *= z2 / (static_cast<double>(k) * (nu + k));
        sum += t;
        if (std::abs(t) <= 1e-18 * std::abs(sum))
        {
            break;
        }
    }
    return sum;
}

// The ascending series of J as bessel_ji_series sums it (kind −1), and in `loss` its cancellation as bes_series_loss
// defines it, the largest term over the size of the sum, taken from the terms of the sum itself (the ratio does not
// depend on the factor (z/2)^ν/Γ(ν+1) the terms share): the series that is the method as it is costs no second pass.
// `at` is the index of the largest term.
// The sizes are compared as squares (no root per term; the stop test differs from the one of base in the rounding of
// that root only): a start outside 1e-100 .. 1e100, or a sum beyond 1e150, leaves a negative loss, which marks a series
// to be priced as before.
[[nodiscard]] inline cplx bessel_j_series_priced(double nu, cplx z, double& loss, int& at) noexcept
{
    const cplx cz = 0.5 * z;
    cplx t = crd::math::pow(cz, nu) / gamma(nu + 1.0);
    cplx sum = t;
    double biggest = t.real() * t.real() + t.imag() * t.imag();
    const bool start_usable = biggest > 1e-200 && biggest < 1e200;
    const cplx z2 = -(cz * cz);
    double total = biggest;
    at = 0;
    for (int k = 1; k <= 400; ++k)
    {
        t *= z2 / (static_cast<double>(k) * (nu + k));
        sum += t;
        const double term = t.real() * t.real() + t.imag() * t.imag();
        total = sum.real() * sum.real() + sum.imag() * sum.imag();
        if (term > biggest)
        {
            biggest = term;
            at = k;
        }
        if (!(total < 1e300)) [[unlikely]]
        {
            loss = -1.0;
            return sum;
        }
        if (term <= 1e-36 * total)
        {
            break;
        }
    }
    loss = start_usable ? crd::math::sqrt(biggest / total) : -1.0;
    return sum;
}

// Cancellation of the ascending series of J, Y (kind −1) or I (kind +1) at z: the largest term of
// Σ (kind·z²/4)^k/(k!(ν+1)_k) over the size of the sum. Rounding leaves about ε times it in the value, so it prices
// the series against the asymptotic.
[[nodiscard]] inline double bes_series_loss(double nu, cplx z, double kind) noexcept
{
    const cplx z2 = kind * (0.25 * z * z);
    cplx t = 1.0;
    cplx sum = 1.0;
    double biggest = 1.0;
    for (int k = 1; k <= 400; ++k)
    {
        t *= z2 / (static_cast<double>(k) * (nu + k));
        sum += t;
        const double mag = std::abs(t);
        const double total = std::abs(sum);
        if (!(total < std::numeric_limits<double>::max())) [[unlikely]]
        {
            return std::numeric_limits<double>::infinity(); // the terms left the doubles: not usable
        }
        biggest = (mag > biggest) ? mag : biggest;
        if (mag <= 1e-18 * total)
        {
            return biggest / total;
        }
    }
    // Not summed within the cap (|z| of several hundred, whose terms are still growing at the last one): the series is
    // not usable there, however small the ratio looks.
    return std::numeric_limits<double>::infinity();
}

// J_ν(z) (want_y false) or Y_ν(z) from Hankel's sums P, Q at ω = z − (ν/2 + ¼)π with |Im ω| ≥ kBesFarAxis: there
// cos ω = ½e^{−iω} and sin ω = ½i·e^{−iω} for Im ω > 0, cos ω = ½e^{iω} and sin ω = −½i·e^{iω} for Im ω < 0, up to the
// other exponential, which is below e^{−2·600} of it. So J = A·½·u·(P − i·σ·Q) and Y = A·½·u·(i·σ·P + Q), with
// σ = sign(Im ω), the unit u = e^{−iσ Re ω}, the amplitude A = √(2/(πz)), all times e^{|Im ω|}.
[[nodiscard]] inline cplx bes_hankel_far_axis(cplx omega, cplx p, cplx q, cplx amplitude, bool want_y) noexcept
{
    const double sigma = (omega.imag() > 0.0) ? 1.0 : -1.0;
    double sine;
    double cosine;
    bes_sincos(omega.real(), sine, cosine);
    const cplx unit(cosine, -sigma * sine);
    const cplx weight(0.0, sigma);
    const cplx moderate = (0.5 * amplitude * unit) * (want_y ? (weight * p + q) : (p - weight * q));
    const double scale = crd::math::exp(crd::math::fabs(omega.imag()) - 700.0);
    const cplx scaled = moderate * scale;
    return cplx(scaled.real() * kBesExp700, scaled.imag() * kBesExp700);
}

// J_ν(z) (want_y false) or Y_ν(z) from Hankel's sums P and Q at ω = z − (ν/2 + ¼)π, and in `error` the relative error
// of the expansion: the larger of the smallest term and the rounding of the largest (raw_error, absolute in P and Q)
// times the factor by which the combination with cos ω and sin ω magnifies it, (|cos ω| + |sin ω|)/|bracket|. The
// bracket cancels where J is small against e^{|Im z|}/√|z| (the transition of a large order at a large imaginary part,
// 1e7 at J_20(8.5 + 20i)): the factor is taken up to kBesCancelCap, which prices such a point above the bar of a method
// taken as it is while a zero of J on the real axis does not.
inline constexpr double kBesCancelCap = 100.0;
// The second exponential of I (DLMF 10.40.5) is below the rounding of the first where 2 Re z − μ/(8 Re z) is above
// this.
inline constexpr double kBesSecondExpBits = 44.0;
[[nodiscard]] inline cplx bes_hankel_value(cplx omega, cplx p, cplx q, cplx amplitude, double raw_error, bool want_y,
                                           double& error) noexcept
{
    const auto l1 = [](cplx v) noexcept
    {
        return crd::math::fabs(v.real()) + crd::math::fabs(v.imag());
    };
    // Where the capped factor cannot lift the error above the bar (the usual call: the sums of a small order and a
    // large |z| are at the rounding), it is not formed.
    const bool priced = raw_error * kBesCancelCap > kBesComplexGood;
    if (crd::math::fabs(omega.imag()) >= kBesFarAxis) [[unlikely]]
    {
        // one exponential: |cos ω| + |sin ω| = |e^{∓iω}|, the bracket is half of it times (P − iσQ)
        const cplx weight(0.0, (omega.imag() > 0.0) ? 1.0 : -1.0);
        const double combination = l1(p - weight * q);
        error = raw_error * ((combination > 2.0 / kBesCancelCap) ? 2.0 / combination : kBesCancelCap);
        return bes_hankel_far_axis(omega, p, q, amplitude, want_y);
    }
    const cplx c = crd::math::cos(omega);
    const cplx sn = crd::math::sin(omega);
    const cplx bracket = want_y ? (sn * p + c * q) : (c * p - sn * q);
    error = raw_error;
    if (priced) [[unlikely]]
    {
        const double size = l1(c) + l1(sn);
        const double smallest = size / kBesCancelCap;
        const double got = l1(bracket);
        error = raw_error * ((got > smallest) ? size / got : kBesCancelCap);
    }
    return amplitude * bracket;
}

// Hankel-type asymptotic coefficients shared by J/Y: returns P and Q (complex) for order ν at z, and the estimated
// error of the sums: the larger of the smallest term kept and the rounding of the largest term met.
inline double besseljy_asymp_pq(double nu, cplx z, cplx& p, cplx& q) noexcept
{
    const double mu = 4.0 * nu * nu;
    const cplx inv = 1.0 / z;
    cplx xpow = 1.0;
    double ak = 1.0;
    p = 1.0;
    q = 0.0;
    // One term: updates the coefficient, the power and the two sums, returns the square of the term's size (the sizes
    // are only compared with each other, and a hypot per term would be most of the cost of the sums).
    const auto add_term = [&](int k) noexcept
    {
        const double odd = 2.0 * k - 1.0;
        ak *= (mu - odd * odd) / (8.0 * k);
        xpow *= inv;
        const cplx term = ak * xpow;
        if ((k & 1) == 0)
        {
            p += (((k / 2) & 1) == 0) ? term : -term;
        }
        else
        {
            q += ((((k - 1) / 2) & 1) == 0) ? term : -term;
        }
        return term.real() * term.real() + term.imag() * term.imag();
    };
    // Growth phase (see bes_hankel_pq): the k up to |ν| + ½, where the terms may still grow: the sum is not cut and the
    // largest term is looked for there only; for |ν| < ½ the phase is empty and the loop below is the plain one.
    const double order = crd::math::fabs(nu);
    const int growth = (order < 60.0) ? static_cast<int>(order + 0.5) : 60;
    double prev_sq = std::numeric_limits<double>::infinity();
    double biggest_sq = 1.0;
    for (int k = 1; k <= growth; ++k)
    {
        prev_sq = add_term(k);
        biggest_sq = (prev_sq > biggest_sq) ? prev_sq : biggest_sq;
    }
    for (int k = growth + 1; k <= 60; ++k)
    {
        const double mag_sq = add_term(k);
        if (mag_sq > prev_sq) // optimal truncation, past the hump
        {
            break;
        }
        prev_sq = mag_sq;
    }
    const double prev = crd::math::sqrt(prev_sq);
    const double biggest = crd::math::sqrt(biggest_sq);
    return (prev > kBesRoundoff * biggest) ? prev : kBesRoundoff * biggest;
}

// e^{i·turns·ν·π} for a half-turn count turns = ±1 and any real ν, with the trigonometry of νπ exact at integers and
// half-integers.
[[nodiscard]] inline cplx bes_turn_phase(double nu, double turns) noexcept
{
    const BesTrig t = bes_sin_cos_pi(crd::math::fabs(nu));
    return {t.cos, turns * ((nu < 0.0) ? -t.sin : t.sin)};
}

// I (kind=+1) or K (kind=−1) large-|z| asymptotic Σ coefficients (a_k = ∏(4ν²−(2j−1)²)/(8j)). K is the single
// exponential e^{−z}. I carries a second one, e^{−z} with the phase ± i e^{±νπi} (DLMF 10.40.5; the sign of Im z
// picks the sign): it is negligible while Re z is large and positive, and it is the whole answer when Re z is negative
// and the first term is the small one; on the imaginary axis both are of the same size.
template <bool IsI> inline cplx besselik_asymp(double nu, cplx z, double& error) noexcept
{
    const double mu = 4.0 * nu * nu;
    const cplx inv = 1.0 / z;
    cplx xpow = 1.0;
    cplx sum = 1.0;
    cplx plain = 1.0; // Σ a_k/z^k, the sum of the second exponential of I
    // The second exponential of I is e^{−2 Re z} of the first, times its sum: at most 60 terms, none above e^{c} with
    // c = μ/(8 Re z) (the product of the ratios μ/(8 k Re z) is below c^K/K!). It is below the rounding of the first
    // where 60 e^{c − 2 Re z} < 1e-17, that is 2 Re z − c > kBesSecondExpBits, and is then left out.
    const double re_z = z.real();
    const bool with_second = IsI && !(re_z > 0.0 && 2.0 * re_z - mu / (8.0 * re_z) > kBesSecondExpBits);
    // The terms may still grow up to k = |ν| + ½ (see bes_hankel_pq): the sum is not cut there and the largest term is
    // looked for there only; past it the sum is cut at the first term that grows.
    const double order = crd::math::fabs(nu);
    const int growth = (order < 60.0) ? static_cast<int>(order + 0.5) : 60;
    double ak = 1.0;
    // The sizes of the terms are compared as squares (the comparisons only need their order): a hypot per term would
    // be most of the cost of the sum. The sizes themselves, which the error estimate below needs, are taken from the
    // two squares that are kept.
    double prev_sq = std::numeric_limits<double>::infinity();
    double biggest_sq = 1.0;
    // At a half-integer order ν = n + ½ the coefficient 4ν² − (2k−1)² vanishes at k = n + 1 and so does every later
    // term: the sum is then finished after n terms, which is the end the loop reaches below without a test of its own.
    int last = 60;
    bool ends = false;
    const double terminating = order + 0.5;
    if (terminating == crd::math::floor(terminating) && terminating <= 60.0)
    {
        last = static_cast<int>(terminating) - 1;
        ends = true;
    }
    int k = 1;
    for (; k <= last; ++k)
    {
        const double odd = 2.0 * k - 1.0;
        ak *= (mu - odd * odd) / (8.0 * k);
        xpow *= inv;
        const double coeff = (!IsI || (k & 1) == 0) ? ak : -ak; // K: +a_k ; I: (−1)^k a_k
        const cplx term = coeff * xpow;
        sum += term;
        if (with_second)
        {
            plain += ak * xpow;
        }
        const double mag_sq = term.real() * term.real() + term.imag() * term.imag();
        if (k <= growth)
        {
            biggest_sq = (mag_sq > biggest_sq) ? mag_sq : biggest_sq;
        }
        else if (mag_sq > prev_sq)
        {
            break;
        }
        prev_sq = mag_sq;
    }
    if (ends && k > last)
    {
        prev_sq = 0.0; // the zero terms that follow
    }
    const double prev = crd::math::sqrt(prev_sq);
    const double biggest = crd::math::sqrt(biggest_sq);
    // The error of the sums, absolute (their terms are of the size of 1); in the value it is relative to the size of
    // the value, which is less than that of the terms where they cancel (the alternating terms of I at a large order
    // and a large imaginary part: 1e-10 at I_20(25 + 4i)).
    const double raw = (prev > kBesRoundoff * biggest) ? prev : kBesRoundoff * biggest;
    const auto l1 = [](cplx v) noexcept
    {
        return crd::math::fabs(v.real()) + crd::math::fabs(v.imag());
    };
    const bool priced = raw * kBesCancelCap > kBesComplexGood; // as bes_hankel_value
    if constexpr (IsI) // I: e^z/√(2πz) · Σ(−1)^k a_k/z^k  ± i e^{±νπi} e^{−z}/√(2πz) · Σ a_k/z^k
    {
        const cplx root = crd::math::sqrt(2.0 * kBesPi * z);
        const cplx grow = crd::math::exp(z);
        error = raw;
        if (!with_second) [[likely]]
        {
            const cplx value = grow / root * sum;
            if (priced) [[unlikely]]
            {
                error = raw * l1(grow) / l1(root) / l1(value);
            }
            return value;
        }
        const double side = (z.imag() >= 0.0) ? 1.0 : -1.0;
        const cplx decay = crd::math::exp(-z);
        const cplx second = cplx(0.0, side) * bes_turn_phase(nu, side) * decay / root * plain;
        const cplx value = grow / root * sum + second;
        if (priced) [[unlikely]]
        {
            error = raw * (l1(grow) + l1(decay)) / l1(root) / l1(value);
        }
        return value;
    }
    else
    {
        error = priced ? raw / l1(sum) : raw;
        return crd::math::sqrt(kBesPi / (2.0 * z)) * crd::math::exp(-z) * sum; // K: √(π/2z) e^{−z} Σ a_k/z^k
    }
}

// Steed's CF2 for K at complex z with Re z ≥ 0 (|z| ≥ 2, |μ| ≤ ½): the recurrences of bes_cf2_k with b_k = 2(z + k)
// complex (Thompson and Barnett 1986). Returns K_μ(z) and K_{μ+1}(z).
struct BesZPair
{
    cplx base;
    cplx next;
};
[[nodiscard]] inline BesZPair bes_cf2_k_complex(cplx z, double mu) noexcept
{
    const double mu2 = mu * mu;
    const double a1 = 0.25 - mu2;
    cplx b_prev = 2.0 * (z + 1.0);
    cplx d = 1.0 / b_prev;
    cplx dh = d;
    cplx h = dh;
    cplx q_old = 0.0;
    cplx q_cur = 1.0;
    double c_k = a1;
    cplx cq_sum = a1 * q_cur;
    cplx s = 1.0 + cq_sum * dh;
    for (int k = 2; k <= kBesIterCap; ++k)
    {
        const double kd = k;
        const double a = (kd - 0.5) * (kd - 0.5) - mu2;
        const cplx b = 2.0 * (z + kd);
        c_k *= a / kd;
        const cplx q_new = (b_prev * q_cur - q_old) / a;
        q_old = q_cur;
        q_cur = q_new;
        cq_sum += c_k * q_new;
        d = 1.0 / (b - a * d);
        dh *= b * d - 1.0;
        h += dh;
        const cplx ds = cq_sum * dh;
        s += ds;
        b_prev = b;
        if (std::abs(ds) < std::abs(s) * kBesRelTol)
        {
            break;
        }
    }
    const cplx kmu = crd::math::sqrt(kBesPi / (2.0 * z)) * crd::math::exp(-z) / s;
    return {kmu, kmu * (mu + z + 0.5 - a1 * h) / z};
}

// The factor by which rounding at the base order is amplified when J (or Y) is carried up to order ν by the recurrence
// in order: the ratio of the two Hankel solutions, e^{−2 Im φ_n(z)} with φ_n(z) = √(z² − n²) − n·arccos(n/z) (the phase
// of Debye's expansion, principal branches, Im z ≥ 0), grows from e^{−2 Im φ_0} = e^{−2 Im z} to its value at ν. Within
// a factor of two of the measured growth at 12 points from ν ≲ Re z (amplification 1) to ν ≈ 2|z| (1e30).
[[nodiscard]] inline double bes_forward_amplification(double nu, cplx z) noexcept
{
    const cplx w(z.real(), (crd::math::fabs(z.imag()) > 1e-300) ? crd::math::fabs(z.imag()) : 1e-300);
    const cplx ratio = nu / w;
    const cplx acos = cplx(0.0, -1.0) * crd::math::log(ratio + cplx(0.0, 1.0) * crd::math::sqrt(1.0 - ratio * ratio));
    const cplx phase = crd::math::sqrt(w * w - nu * nu) - nu * acos;
    const double exponent = 2.0 * (w.imag() - phase.imag());
    return (exponent < 600.0) ? crd::math::exp(exponent) : 1.0e260;
}

// J_ν(z) and Y_ν(z), ν ≥ 0, Re z ≥ 0, from Hankel's expansion at the base order μ = ν − ⌊ν⌋ and μ+1, carried up by
// the recurrence in order. Returns the error estimate of the start (the larger smallest-term bound of the two orders).
inline double bes_complex_forward(double nu, cplx z, cplx& j, cplx& y) noexcept
{
    const int steps = static_cast<int>(nu);
    const double mu = nu - steps;
    cplx p0;
    cplx q0;
    cplx p1;
    cplx q1;
    const double e0 = besseljy_asymp_pq(mu, z, p0, q0);
    const double e1 = besseljy_asymp_pq(mu + 1.0, z, p1, q1);
    const cplx omega = z - (0.5 * mu + 0.25) * kBesPi;
    const cplx c = crd::math::cos(omega);
    const cplx sn = crd::math::sin(omega);
    const cplx amplitude = crd::math::sqrt(2.0 / (kBesPi * z));
    cplx j_lo = amplitude * (c * p0 - sn * q0);
    cplx y_lo = amplitude * (sn * p0 + c * q0);
    cplx j_hi = amplitude * (sn * p1 + c * q1); // ω_{μ+1} = ω_μ − π/2
    cplx y_hi = amplitude * (sn * q1 - c * p1);
    const cplx two_over_z = 2.0 / z;
    for (int m = 1; m <= steps; ++m)
    {
        const cplx factor = (mu + m) * two_over_z;
        const cplx j_up = factor * j_hi - j_lo;
        const cplx y_up = factor * y_hi - y_lo;
        j_lo = j_hi;
        j_hi = j_up;
        y_lo = y_hi;
        y_hi = y_up;
    }
    j = j_lo;
    y = y_lo;
    return (e0 > e1) ? e0 : e1;
}

// Is the recurrence from the base order a better method than `best` (the error estimate of the series or asymptotic
// already in hand)? Its own estimate: the start of Hankel's expansion at |z| ≥ kBesComplexForwardFrom is at the
// rounding, and the recurrence amplifies it (bes_forward_amplification), plus the rounding of its steps.
[[nodiscard]] inline bool bes_complex_forward_wins(double nu, cplx z, double best) noexcept
{
    if (!(best > kBesComplexGood) || !(std::abs(z) >= kBesComplexForwardFrom) || !(nu < kBesForwardSteps))
    {
        return false;
    }
    return kBesRoundoff * (20.0 + 4.0 * bes_forward_amplification(nu, z)) < best;
}

// J_ν(z) and Y_ν(z) for any real ν by the recurrence from the base order (a negative order is reflected, DLMF 10.2.3);
// ok is false when the start of the expansion turned out worse than `best`.
struct BesComplexJY
{
    cplx j;
    cplx y;
    bool ok;
};
[[nodiscard]] inline BesComplexJY bes_complex_forward_jy(double nu, cplx z, double best) noexcept
{
    cplx j;
    cplx y;
    const double start_error = bes_complex_forward(crd::math::fabs(nu), z, j, y);
    if (!(start_error < best))
    {
        return {j, y, false};
    }
    if (nu >= 0.0)
    {
        return {j, y, true};
    }
    const BesTrig t = bes_sin_cos_pi(-nu);
    return {t.cos * j - t.sin * y, t.sin * j + t.cos * y, true};
}

// The ratio J_{ν+1}(z)/J_ν(z) at complex z by its continued fraction 1/(b_1 − 1/(b_2 − 1/(b_3 − …))), b_k = 2(ν+k)/z
// (the recurrence in order, DLMF 10.6.1), evaluated by the modified Lentz algorithm (Lentz 1976; Thompson and Barnett
// 1986). J is the minimal solution of the recurrence as the order grows, so the fraction converges for every z
// (Pincherle's theorem). False when it has not converged within kBesComplexCf1Cap terms.
[[nodiscard]] inline bool bes_cf1_complex(double nu, cplx z, cplx& ratio) noexcept
{
    const cplx spacing = 2.0 / z;
    cplx value = kBesTiny; // b_0 = 0
    cplx num_tail = value;
    cplx den_tail = 0.0;
    for (int k = 1; k <= kBesComplexCf1Cap; ++k)
    {
        const cplx b = spacing * (nu + k);
        const double a = (k == 1) ? 1.0 : -1.0;
        cplx den = b + a * den_tail;
        cplx num = b + a / num_tail;
        if (std::abs(den) < kBesTiny) [[unlikely]]
        {
            den = kBesTiny;
        }
        if (std::abs(num) < kBesTiny) [[unlikely]]
        {
            num = kBesTiny;
        }
        den_tail = 1.0 / den;
        num_tail = num;
        const cplx factor = num_tail * den_tail;
        value *= factor;
        if (std::abs(factor - 1.0) < kBesComplexCf1Tol)
        {
            ratio = value;
            return true;
        }
    }
    return false;
}

// J_ν(z) and Y_ν(z) for any real ν at Re z ≥ 0 by the method that is stable where the recurrence from the base order is
// not (ν ≳ ¾ Re z, Im z ≳ 6, where that recurrence amplifies the start by e^{2(Im z − Im φ_ν)}): J is carried DOWN from
// order ν to the base order μ = ν − s, μ ∈ [−½, ½), from the ratio of its continued fraction (the downward recurrence
// is stable for the solution that is minimal as the order grows, and the contamination by the other one decays); K at
// the rotated argument w = −iz, Re w = Im z > 0, which is H^(1) up to a factor (DLMF 10.27.8), comes from Steed's CF2
// at the base order and the recurrence upward, stable for K; and the Wronskian of J and H^(1),
//   J_μ H^(1)_{μ+1} − J_{μ+1} H^(1)_μ = −2i/(πz),
// fixes J_μ from the two ratios without a square root, so that no branch is to be chosen. J_ν = J_μ times the
// unnormalised recurrence, and Y_ν = −i(H^(1)_ν − J_ν). Im z < 0 is the conjugate of Im z > 0. ok is false when the
// fraction has not converged.
[[nodiscard]] inline BesComplexJY bes_complex_stable_jy(double nu, cplx z) noexcept
{
    const double order = crd::math::fabs(nu);
    const bool lower = z.imag() < 0.0;
    const cplx zu = lower ? std::conj(z) : z;
    cplx ratio;
    if (!bes_cf1_complex(order, zu, ratio))
    {
        return {0.0, 0.0, false};
    }
    const int steps = bes_base_steps(order);
    const double mu = order - steps;
    // Down from J_{ν+1}/J_ν = ratio, on the scale J_ν = 1; the scale is held below 2^500 in shift units of 2^500.
    const cplx two_over_z = 2.0 / zu;
    cplx upper = ratio;
    cplx base = 1.0;
    int shift = 0;
    for (int k = steps; k >= 1; --k)
    {
        const cplx below = ((mu + k) * two_over_z) * base - upper;
        upper = base;
        base = below;
        if (std::abs(base.real()) + std::abs(base.imag()) > 0x1p500) [[unlikely]]
        {
            upper *= 0x1p-500;
            base *= 0x1p-500;
            ++shift;
        }
    }
    // K_μ(w) and K_{μ+1}(w), w = −iz.
    const cplx w(zu.imag(), -zu.real());
    BesZPair k = bes_cf2_k_complex(w, mu);
    const cplx hankel_ratio = cplx(0.0, -1.0) * (k.next / k.base); // H^(1)_{μ+1}/H^(1)_μ = −i K_{μ+1}/K_μ
    const cplx half_turn(crd::math::cos(0.5 * mu * kBesPi), crd::math::sin(0.5 * mu * kBesPi));
    // J_μ = −e^{iμπ/2}/(z K_μ(w)(J_{μ+1}/J_μ − H^(1)_{μ+1}/H^(1)_μ)), from H^(1)_μ = (2/(πi)) e^{−iμπ/2} K_μ(w).
    const cplx j_mu = -half_turn / (zu * k.base * (upper / base - hankel_ratio));
    const cplx scaled = j_mu / base;
    cplx j(std::ldexp(scaled.real(), -500 * shift), std::ldexp(scaled.imag(), -500 * shift));
    const cplx two_over_w = 2.0 / w;
    for (int m = 1; m <= steps; ++m)
    {
        const cplx up = (mu + m) * two_over_w * k.next + k.base;
        k.base = k.next;
        k.next = up;
    }
    // H^(1)_ν = (2/(πi)) e^{−iνπ/2} K_ν(w), with e^{−iνπ/2} = e^{−iμπ/2}(−i)^s exactly.
    static constexpr cplx kTurns[4] = {{1.0, 0.0}, {0.0, -1.0}, {-1.0, 0.0}, {0.0, 1.0}};
    const cplx hankel = cplx(0.0, -2.0 / kBesPi) * std::conj(half_turn) * kTurns[steps & 3] * k.base;
    cplx y = cplx(0.0, -1.0) * (hankel - j);
    if (lower)
    {
        j = std::conj(j);
        y = std::conj(y);
    }
    if (nu >= 0.0)
    {
        return {j, y, true};
    }
    const BesTrig t = bes_sin_cos_pi(-nu);
    return {t.cos * j - t.sin * y, t.sin * j + t.cos * y, true};
}

// Does the recurrence from the base order lose more than kBesComplexGood to its amplification of the start, so that
// the method above is the one to take? (Where the series or the asymptotic is already good, or |z| is below the
// recurrence's range, or Im z is so large that J and the factors of the method are outside the doubles, it is not.)
[[nodiscard]] inline bool bes_complex_stable_wanted(double nu, cplx z, double best) noexcept
{
    if (!(best > kBesComplexGood) || !(std::abs(z) >= kBesComplexForwardFrom) || !(nu < kBesForwardSteps) ||
        !(crd::math::fabs(z.imag()) < kBesComplexStableMax))
    {
        return false;
    }
    return !(kBesRoundoff * (20.0 + 4.0 * bes_forward_amplification(nu, z)) < kBesComplexStableFrom);
}

// J_ν(z), any real ν — series (moderate |z|) or Hankel asymptotic (large |z|), whichever has the smaller error
// estimate. The series Σ(z/2)^{ν+2k}/… is valid for any ν (negative-integer terms vanish via 1/Γ(non-positive)=0).
[[nodiscard]] inline cplx besselj_c(double nu, cplx z, bool mirrored = false) noexcept
{
    const double magnitude = std::abs(z);
    if (magnitude >= kBesComplexAsympFrom)
    {
        if (z.real() < 0.0)
        {
            // The expansion is not uniform near the negative real axis (ν = 0 is off by O(1) there). J_ν(w e^{±πi}) =
            // e^{±νπi} J_ν(w) (DLMF 10.11.1) moves it to the right half-plane; the sign follows the side of the cut.
            return bes_turn_phase(nu, (z.imag() >= 0.0) ? 1.0 : -1.0) * besselj_c(nu, -z, true);
        }
        double summed_loss = -1.0; // the cancellation of the series, once it has been summed
        if (!mirrored && nu >= 0.0 && magnitude < 2.0 * nu)
        {
            // The range of the series in base (at the point itself: a mirrored point is not base's), where Hankel's
            // expansion is never the better method: the series is summed once and priced by its own terms (the rounding
            // of the terms up to the largest, see kBesSeriesIndexUnit), and taken while that is within the module's
            // bar.
            int largest_at = 0;
            const cplx summed = bessel_j_series_priced(nu, z, summed_loss, largest_at);
            const double index_factor = (largest_at > kBesSeriesIndexUnit) ? largest_at / kBesSeriesIndexUnit : 1.0;
            if (summed_loss >= 0.0 && kBesRoundoff * summed_loss * index_factor <= kBesComplexUsable)
            {
                return summed;
            }
        }
        cplx p;
        cplx q;
        const double raw_error = besseljy_asymp_pq(nu, z, p, q);
        const cplx omega = z - (0.5 * nu + 0.25) * kBesPi;
        double asymptotic_error;
        const cplx asymptotic =
            bes_hankel_value(omega, p, q, crd::math::sqrt(2.0 / (kBesPi * z)), raw_error, false, asymptotic_error);
        // An expansion within kBesComplexGood is taken as it is: the series is priced (a loop of up to 400 terms) only
        // where it could be the better method.
        const double series_error = (asymptotic_error <= kBesComplexGood) ? std::numeric_limits<double>::infinity()
                                    : (summed_loss >= 0.0)
                                        ? kBesRoundoff * summed_loss
                                        : kBesRoundoff * bes_series_loss(crd::math::fabs(nu), z, -1.0);
        const double best = (asymptotic_error < series_error) ? asymptotic_error : series_error;
        if (crd::math::fabs(nu) >= kBesForwardSteps && !(best <= kBesComplexUsable)) [[unlikely]]
        {
            return cplx(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());
        }
        if (bes_complex_stable_wanted(crd::math::fabs(nu), z, best))
        {
            const BesComplexJY pair = bes_complex_stable_jy(nu, z);
            if (pair.ok)
            {
                return pair.j;
            }
        }
        if (bes_complex_forward_wins(crd::math::fabs(nu), z, best))
        {
            const BesComplexJY pair = bes_complex_forward_jy(nu, z, best);
            if (pair.ok)
            {
                return pair.j;
            }
        }
        if (asymptotic_error < series_error)
        {
            return asymptotic;
        }
    }
    return bessel_ji_series(nu, z, -1.0);
}

// Integer-order Y_n(z) ascending series (n ≥ 0), DLMF 10.8.1 — exact (log + digamma), no ε-offset cancellation.
[[nodiscard]] inline cplx bessely_n_int(int n, cplx z) noexcept
{
    const cplx cz = 0.5 * z;
    const cplx z2 = cz * cz;
    cplx r = (2.0 / kBesPi) * crd::math::log(cz) * besselj_c(static_cast<double>(n), z);
    if (n > 0)
    {
        cplx finite = 0.0;
        cplx pw = 1.0; // (z²/4)^k
        for (int k = 0; k < n; ++k)
        {
            finite += (gamma(static_cast<double>(n - k)) / gamma(static_cast<double>(k + 1))) * pw;
            pw *= z2;
        }
        r -= (1.0 / kBesPi) * crd::math::pow(cz, -static_cast<double>(n)) * finite;
    }
    cplx s = 0.0;
    cplx pw = 1.0;
    for (int k = 0; k <= 400; ++k)
    {
        const double psi = digamma(static_cast<double>(k + 1)) + digamma(static_cast<double>(n + k + 1));
        const cplx term = ((k & 1) ? -1.0 : 1.0) * psi /
                          (gamma(static_cast<double>(k + 1)) * gamma(static_cast<double>(n + k + 1))) * pw;
        s += term;
        if (k >= n && std::abs(term) <= 1e-18 * std::abs(s))
        {
            break;
        }
        pw *= z2;
    }
    return r - (1.0 / kBesPi) * crd::math::pow(cz, static_cast<double>(n)) * s;
}

// Integer-order K_n(z) ascending series (n ≥ 0), DLMF 10.31.2 — exact (log + digamma).
[[nodiscard]] inline cplx besselk_n_int(int n, cplx z) noexcept
{
    const cplx cz = 0.5 * z;
    const cplx z2 = cz * cz;
    cplx r = ((n & 1) ? 1.0 : -1.0) * crd::math::log(cz) *
             bessel_ji_series(static_cast<double>(n), z, 1.0); // (−1)^{n+1} ln·I_n
    if (n > 0)
    {
        cplx finite = 0.0;
        cplx pw = 1.0; // (−z²/4)^k
        for (int k = 0; k < n; ++k)
        {
            finite += (gamma(static_cast<double>(n - k)) / gamma(static_cast<double>(k + 1))) * pw;
            pw *= (-z2);
        }
        r += 0.5 * crd::math::pow(cz, -static_cast<double>(n)) * finite;
    }
    cplx s = 0.0;
    cplx pw = 1.0;
    for (int k = 0; k <= 400; ++k)
    {
        const double psi = digamma(static_cast<double>(k + 1)) + digamma(static_cast<double>(n + k + 1));
        const cplx term = psi / (gamma(static_cast<double>(k + 1)) * gamma(static_cast<double>(n + k + 1))) * pw;
        s += term;
        if (k >= n && std::abs(term) <= 1e-18 * std::abs(s))
        {
            break;
        }
        pw *= z2;
    }
    return r + ((n & 1) ? -1.0 : 1.0) * 0.5 * crd::math::pow(cz, static_cast<double>(n)) * s; // (−1)^n
}

// Y_ν(z), any real ν. Large |z|: asymptotic (signed ω). Moderate |z|: connection (J_ν cosνπ − J_{−ν})/sinνπ
// (integer ν ⇒ the exact integer series above).
[[nodiscard]] inline cplx bessely_c(double nu, cplx z) noexcept
{
    if (std::abs(z) >= kBesComplexAsympFrom)
    {
        if (z.real() < 0.0)
        {
            // Y_ν(w e^{±πi}) = e^{∓νπi} Y_ν(w) ± 2i cos(νπ) J_ν(w) (DLMF 10.11.2, with sin(±νπ)cot(νπ) = ±cos(νπ))
            const double whole = crd::math::round(nu);
            const bool near_negative_integer =
                whole < 0.0 && crd::math::fabs(nu - whole) < 1e-12 && -whole < kBesForwardSteps;
            if (near_negative_integer && nu != whole && (std::abs(z) < 17.5 || std::abs(z) < -2.0 * nu))
            {
                // Within 1e-12 of a negative integer (the test of base) but not on it, the reflection would reach J's
                // series at ν + k of about 1e-14 (whose rounding is the whole value, up to 1e8 times the bar): base's
                // route.
                const int n = static_cast<int>(-whole);
                const cplx yn = bessely_n_int(n, z);
                return ((n & 1) != 0) ? -yn : yn;
            }
            const double turn = (z.imag() >= 0.0) ? 1.0 : -1.0;
            const cplx reflected =
                bes_turn_phase(nu, -turn) * bessely_c(nu, -z) +
                cplx(0.0, 2.0 * turn * bes_sin_cos_pi(crd::math::fabs(nu)).cos) * besselj_c(nu, -z, true);
            if (reflected != reflected && near_negative_integer)
            {
                // At a negative integer order J's series divides by ν + k = 0 where it is the method: the integer
                // series of base serves those inputs (Y_{−n} = (−1)^n Y_n).
                const int n = static_cast<int>(-whole);
                const cplx yn = bessely_n_int(n, z);
                return ((n & 1) != 0) ? -yn : yn;
            }
            return reflected;
        }
        cplx p;
        cplx q;
        const double raw_error = besseljy_asymp_pq(nu, z, p, q);
        const cplx omega = z - (0.5 * nu + 0.25) * kBesPi;
        double asymptotic_error;
        const cplx asymptotic =
            bes_hankel_value(omega, p, q, crd::math::sqrt(2.0 / (kBesPi * z)), raw_error, true, asymptotic_error);
        double series_error = (asymptotic_error <= kBesComplexGood)
                                  ? std::numeric_limits<double>::infinity()
                                  : kBesRoundoff * bes_series_loss(crd::math::fabs(nu), z, -1.0);
        if (!(asymptotic_error < series_error) && crd::math::fabs(nu - crd::math::round(nu)) >= 1e-12)
        {
            // The series serves Y through J_{−ν} as well, whose terms alternate while ν + k < 0: it is priced by the
            // larger of the two losses.
            const double reflected = kBesRoundoff * bes_series_loss(-crd::math::fabs(nu), z, -1.0);
            series_error = (reflected > series_error || reflected != reflected) ? reflected : series_error;
        }
        const double best = (asymptotic_error < series_error) ? asymptotic_error : series_error;
        if (crd::math::fabs(nu) >= kBesForwardSteps && !(best <= kBesComplexUsable)) [[unlikely]]
        {
            return cplx(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());
        }
        if (bes_complex_stable_wanted(crd::math::fabs(nu), z, best))
        {
            const BesComplexJY pair = bes_complex_stable_jy(nu, z);
            if (pair.ok)
            {
                return pair.y;
            }
        }
        if (bes_complex_forward_wins(crd::math::fabs(nu), z, best))
        {
            const BesComplexJY pair = bes_complex_forward_jy(nu, z, best);
            if (pair.ok)
            {
                return pair.y;
            }
        }
        if (asymptotic_error < series_error)
        {
            return asymptotic;
        }
    }
    const double rn = crd::math::round(nu);
    if (crd::math::fabs(nu - rn) < 1e-12) // integer order: exact ascending series
    {
        const int n = static_cast<int>(crd::math::fabs(rn));
        const cplx yn = bessely_n_int(n, z);
        return (rn < 0.0 && (n & 1)) ? -yn : yn; // Y_{−n} = (−1)^n Y_n
    }
    return (besselj_c(nu, z) * crd::math::cos(nu * kBesPi) - besselj_c(-nu, z)) / crd::math::sin(nu * kBesPi);
}

// I_ν(z), any real ν — series (moderate |z|) or dominant asymptotic (large |z|, where I_{−ν}≈I_ν).
[[nodiscard]] inline cplx besseli_c(double nu, cplx z) noexcept
{
    if (std::abs(z) >= kBesComplexAsympFrom)
    {
        double error = 0.0;
        const cplx value = besselik_asymp<true>(nu, z, error);
        if (error <= kBesComplexGood || error < kBesRoundoff * bes_series_loss(crd::math::fabs(nu), z, 1.0))
        {
            return value;
        }
    }
    return bessel_ji_series(nu, z, 1.0);
}

// I_ν(w) for the reflection of K at Re w > 0. At a half-integer order the ascending series of I, summed where |w| < 2ν,
// carries rounding of the size of the bar of K (its estimate is 1e-9 at K_80.5(−33.3 − 57.2i), and its error up to
// three times that). Where the estimate is over kBesReflectedSeries, a third of the bar of K, I comes from the method
// that is stable for J, at the rotated argument: I_ν(w) = e^{−iνπ/2} J_ν(iw) (DLMF 10.27.6).
inline constexpr double kBesReflectedSeries = 3.0e-10;
[[nodiscard]] inline cplx bes_i_of_reflection(double n, cplx w) noexcept
{
    const double twice = 2.0 * n;
    if (twice == crd::math::round(twice) && n != crd::math::round(n) && n < kBesForwardSteps && std::abs(w) < twice &&
        crd::math::fabs(w.real()) < kBesComplexStableMax &&
        kBesRoundoff * bes_series_loss(n, w, 1.0) > kBesReflectedSeries)
    {
        const BesComplexJY pair = bes_complex_stable_jy(n, cplx(-w.imag(), w.real()));
        if (pair.ok)
        {
            const BesTrig t = bes_sin_cos_pi(0.5 * n);
            return cplx(t.cos, -t.sin) * pair.j;
        }
    }
    return besseli_c(n, w);
}

// K_ν(z) = K_{−ν}(z). Large |z|: asymptotic. Moderate |z|: (π/2)(I_{−ν}−I_ν)/sinνπ (integer ν ⇒ ε-offset).
[[nodiscard]] inline cplx besselk_c(double nu, cplx z) noexcept
{
    const double n = crd::math::fabs(nu);
    if (std::abs(z) >= kBesComplexAsympFrom)
    {
        if (z.real() < 0.0)
        {
            // K_ν(w e^{±πi}) = e^{∓νπi} K_ν(w) ∓ πi I_ν(w) (DLMF 10.34.2): in the left half-plane K is the I of the
            // mirrored point, whose expansion carries both exponentials, so nothing small is dropped near Re z = 0.
            const double turn = (z.imag() >= 0.0) ? 1.0 : -1.0;
            return bes_turn_phase(n, -turn) * besselk_c(n, -z) - cplx(0.0, turn * kBesPi) * bes_i_of_reflection(n, -z);
        }
        // K is exp-small for Re z > 0: the series and the connection hold terms of size e^{|z|} to make e^{−Re z}.
        // Between the two (the asymptotic is not yet at the rounding, the connection already loses e^{2|z|}ε), Steed's
        // CF2 gives K at the base order and the recurrence in order, stable upward for K, carries it up to ν.
        double error = 0.0;
        const cplx value = besselik_asymp<false>(n, z, error);
        // |z| + Re z is at least 8 here, where kBesRoundoff · e^{|z| + Re z} is above kBesComplexGood: only the first
        // test counts
        if (error < kBesComplexGood)
        {
            return value;
        }
        if (n < kBesForwardSteps)
        {
            const int steps = static_cast<int>(n);
            const double mu = n - steps;
            BesZPair k = bes_cf2_k_complex(z, mu);
            const cplx two_over_z = 2.0 / z;
            for (int m = 1; m <= steps; ++m)
            {
                const cplx up = (mu + m) * two_over_z * k.next + k.base;
                k.base = k.next;
                k.next = up;
            }
            return k.base;
        }
        if (error < kBesRoundoff * crd::math::exp(std::abs(z) + z.real()))
        {
            return value;
        }
    }
    const double rn = crd::math::round(n);
    if (crd::math::fabs(n - rn) < 1e-12) // integer order: exact ascending series
    {
        return besselk_n_int(static_cast<int>(rn), z);
    }
    return (0.5 * kBesPi) * (bessel_ji_series(-n, z, 1.0) - bessel_ji_series(n, z, 1.0)) / crd::math::sin(n * kBesPi);
}
} // namespace detail

namespace detail
{
template <Real T>
[[nodiscard]] inline std::complex<T> to_user(cplx r) noexcept
{
    return {static_cast<T>(r.real()), static_cast<T>(r.imag())};
}
template <Real T>
[[nodiscard]] inline cplx to_d(std::complex<T> z) noexcept
{
    return {static_cast<double>(z.real()), static_cast<double>(z.imag())};
}
} // namespace detail

template <Real T>
[[nodiscard]] std::complex<T> cyl_bessel_j(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::besselj_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_neumann(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::bessely_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_bessel_i(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::besseli_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_bessel_k(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::besselk_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_hankel_1(T nu, std::complex<T> z) noexcept // J + iY
{
    const detail::cplx j = detail::besselj_c(static_cast<double>(nu), detail::to_d(z));
    const detail::cplx y = detail::bessely_c(static_cast<double>(nu), detail::to_d(z));
    return detail::to_user<T>(j + detail::cplx(0.0, 1.0) * y);
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_hankel_2(T nu, std::complex<T> z) noexcept // J − iY
{
    const detail::cplx j = detail::besselj_c(static_cast<double>(nu), detail::to_d(z));
    const detail::cplx y = detail::bessely_c(static_cast<double>(nu), detail::to_d(z));
    return detail::to_user<T>(j - detail::cplx(0.0, 1.0) * y);
}

} // namespace crd::hesap::special
