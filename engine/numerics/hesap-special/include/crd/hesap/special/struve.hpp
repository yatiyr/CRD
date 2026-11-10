#pragma once

// crd-hesap-special v12-d — Struve H_ν(x) and modified Struve L_ν(x), x ≥ 0.
//   H_ν(x) = Σ_{m≥0} (−1)^m (x/2)^{2m+ν+1} / (Γ(m+3/2)Γ(m+ν+3/2))   (alternating ⇒ cancels for large x)
//   L_ν(x) = Σ_{m≥0}        (x/2)^{2m+ν+1} / (Γ(m+3/2)Γ(m+ν+3/2))   (all-positive for ν > −3/2 ⇒ accurate everywhere)
// For L at a more negative order Γ(m+ν+3/2) changes sign for m < −ν − 3/2, so the sum alternates there and loses digits
// (1e-9 relative at ν = −100, x ≈ 63; ν > −35 stays inside 1e-12). Below ν = −30 the sum of the ratios of the terms to
// the first is checked against its largest term, and where it loses more than a couple of digits it is formed again in
// double-double arithmetic (Dekker 1971; Hida, Li and Bailey 2001), whose 32 digits leave the result at the rounding of
// its first term; that term takes 1/Γ(s) at a negative s from sin(πs) with the turn reduced exactly, and is carried as
// a logarithm where it is not a normal double. A half-integer order −(n+½) whose series leaves the doubles is
// I_{n+½} (DLMF 11.4.4).
// H_ν: the ascending series for x < 16 (its cancellation, about e^{x²/(4ν+4)}, makes the crossover x ≈ 10–16 ~1e-10 at
// worst: the documented bar). Beyond it the series would lose too many digits, so H_ν is taken as Y_ν + R_ν,
// R_ν = H_ν − Y_ν (DLMF 11.5.2, a positive integral, summed by a 16-point Gauss–Laguerre rule), or, where Y_ν and R_ν
// would cancel (ν ≥ x/2), by the downward recurrence DLMF 11.4.23 from an order large enough for the series to be safe.
// For x ≥ 28 and ν < 0.9x the asymptotic H_ν = Y_ν + (1/π)Σ Γ(m+½)/Γ(ν−m+½)(x/2)^{ν−2m−1} (DLMF 11.6.1, optimal
// truncation) is used while its smallest term is below 1e-13. A negative order goes through the reflected cyl_neumann,
// and stays on the series while x² < 16(½ − ν). Gated vs scipy.special.struve / modstruve (H to ~1e-12 relative or
// 1e-13 absolute beyond the crossover; the reference's own error is ~1e-13 near x = 25). Internals f64.

#include <crd/hesap/special/bessel.hpp> // cyl_neumann (Y_ν for H) and BesAsymp
#include <crd/hesap/special/gamma.hpp>
#include <crd/math/cmath.hpp>

#include <limits>

namespace crd::hesap::special
{
namespace detail
{
inline constexpr double kStruveSeriesLimit = 16.0;    // below: ascending series
inline constexpr double kStruveAsymptoticFrom = 28.0; // from here (and ν < 0.9x): asymptotic when its terms allow it
inline constexpr double kStruveAsymptoticFloor = 1.0e-13;
inline constexpr double kStruveSeedLoss = 3.0; // the recurrence starts where the series loses at most e^3
inline constexpr int kStruveMaxSteps = 200000; // bound on the recurrence length (x²/12 grows fast)
inline constexpr double kStruveBig = 0x1p500;  // power-of-two scale factors: exact, so a rescale adds no rounding
inline constexpr double kStruveSmall = 0x1p-500;
inline constexpr double kStruveLossFrom = -30.0;  // below this order the L series is checked for cancellation
inline constexpr double kStruveLossLimit = 100.0; // largest term over the sum from which it is formed again
inline constexpr double kStruveLn2 = 0.69314718055994530942;
// The sums of ratios of L below kStruveLossFrom stop at this many terms: they peak at m ≈ (−ν + √(ν² + x²))/2 and fall
// below 1e-18 of the sum some 6.4√m terms later (m ≈ 630 at ν = −300, x = 600, where 500 terms left L 3% off).
inline constexpr int kStruveRatioCap = 4000;

// Nodes and weights of the 16-point Gauss–Laguerre rule for ∫_0^∞ e^{−s} f(s) ds (Abramowitz & Stegun 25.4.45: the
// roots of L_16 and w_i = s_i/(17² L_17(s_i)²)).
inline constexpr double kStruveNode[16] = {
    0.0876494104789278403602, 0.462696328915080831881, 1.14105777483122685688, 2.12928364509838061633,
    3.43708663389320664524,   5.07801861454976791292,  7.0703385350482341304,  9.43831433639193878395,
    12.2142233688661587369,   15.4415273687816170768,  19.1801568567531348547, 23.5159056939919085318,
    28.5787297428821403675,   34.5833987022866258145,  41.9404526476883326355, 51.7011603395433183643};
inline constexpr double kStruveWeight[16] = {
    0.206151714957800994334,     0.331057854950884165993,    0.2657957776442141526,     0.136296934296377539976,
    0.0473289286941252189781,    0.0112999000803394532312,   0.00184907094352631086429, 0.000204271915308278460126,
    0.0000148445868739812987714, 6.8283193308711995644e-7,   1.88102484107967321388e-8, 2.86235024297388161963e-10,
    2.12707903322410296739e-12,  6.29796700251786778717e-15, 5.0504737000355128204e-18, 4.16146237037285519043e-22};

// What the checked series reports: its leading term, the index of that term, and the largest term met.
struct StruveTrace
{
    double lead;
    int first;
    double biggest;
};

// The first term (x/2)^{ν+1}/(Γ(3/2)Γ(ν+3/2)) of the series where s = ν + 3/2 is negative and not a pole: 1/Γ(s) =
// sin(πs)Γ(1−s)/π by the reflection, with sin(πs) from the exactly reduced turn (bes_sin_pi). gamma()'s own reflection
// rounds π·s first, and that rounding, ε·|πs|, is the whole of sin(πs) near a pole (2.5e-11 relative at ν =
// −145.50007). `value` is the term where it and its factors are normal doubles, and 0 where they are not: the term is
// then sign·e^{struve_lead_log}.
struct StruveLead
{
    double value;
    double sign;
};
[[nodiscard]] inline StruveLead struve_lead_negative(double nu, double half) noexcept
{
    const double shifted = nu + 1.5;
    const double sine = -bes_sin_pi(-shifted); // sin(π s)
    const double reflected = gamma(1.0 - shifted);
    const double power = crd::math::pow(half, nu + 1.0);
    const double term = power * (sine * reflected / (kBesPi * gamma(1.5)));
    constexpr double kMin = (std::numeric_limits<double>::min)();
    constexpr double kMax = (std::numeric_limits<double>::max)();
    const double size = crd::math::fabs(term);
    const bool normal = power >= kMin && power <= kMax && reflected <= kMax && size >= kMin && size <= kMax;
    return {normal ? term : 0.0, (sine < 0.0) ? -1.0 : 1.0};
}
// ln|first term| for the same s: (ν+1) ln(x/2) + ln Γ(1−s) + ln(|sin πs|/π) − ln Γ(3/2).
[[nodiscard]] inline double struve_lead_log(double nu, double half) noexcept
{
    const double shifted = nu + 1.5;
    return (nu + 1.0) * crd::math::log(half) + lgamma(1.0 - shifted) +
           crd::math::log(crd::math::fabs(bes_sin_pi(-shifted)) / kBesPi) - lgamma(1.5);
}

// May the series stop at the term m, which is below 1e-18 of the sum? At a negative ν that is not a half-integer the
// factor m + ν + ½ of the terms passes its smallest sizes f and 1 − f (f the fraction of c = −ν − ½) at m = ⌊c⌋ and
// ⌈c⌉, where the terms grow by (x/2)²/((m + ½)f) and can be far above those before (L at ν = −60.500003, x = 32.3 was
// 1.1e-10 off: the sum stopped before them). It may stop where the terms after it stay below it: from m to M = ⌈c⌉
// their ratios multiply to at most r^{K+1}/(f(1 − f)(K − 1)!), r = (x/2)²/√((m + 3/2)(M + ½)), K = ⌊c⌋ − m, and from
// M on each further ratio is at most a/i at the i-th step, a = (x/2)²/(M + ½), so that all the terms after M add at
// most e^a times the one at M. With (K − 1)! ≥ ((K − 1)/e)^{K−1} and the logarithms bounded by bes_log_lower (within
// 0.002 of ln q for any positive q), the whole is taken below e^{−2} of the term at m. Called only where the term is
// that small, and, once it has refused, not again before the crossing: each call costs a few logarithms, and the terms
// up to the crossing are all that small (H at ν = −60.7 ran 11 % slower asking at each of them).
[[nodiscard]] inline bool struve_may_stop(int m, double nu, double z2) noexcept
{
    const double c = -nu - 0.5;
    if (!(c > 1.0) || m > c)
    {
        return true; // no crossing ahead (or the order is not negative enough to have one)
    }
    const double whole = crd::math::floor(c);
    const double f = c - whole;
    if (f == 0.0)
    {
        return true; // a half-integer: the series starts at the crossing
    }
    const double near = (f < 1.0 - f) ? f : 1.0 - f;
    if (z2 < 0.5 * (whole + 0.5) * near)
    {
        return true; // no ratio reaches 1 at the crossing, nor after it
    }
    const double steps = whole - m; // K ≥ 0
    // First a test without a logarithm, a division or a root, which settles the usual case: where r < (K − 1)/e², the
    // tail term below is less than −(K − 1), and ln of `small` is below (its binary exponents' difference + 2)·ln 2.
    const double spread = (m + 1.5) * (whole + 1.5); // r² = (x/2)⁴/spread
    if (steps > 1.0 && (z2 * 7.39) * (z2 * 7.39) < (steps - 1.0) * (steps - 1.0) * spread)
    {
        const double head_bound =
            (bes_binary_floor(z2 * z2) - bes_binary_floor(spread * f * (1.0 - f)) + 2.0) * kBesLn2;
        if (z2 < (steps - 3.0 - head_bound) * (whole + 1.5))
        {
            return true;
        }
    }
    // Π(j + ½) over the K + 1 steps is at least ((m + 3/2)(M + ½))^{(K+1)/2}, ln being concave (Jensen)
    const double r = z2 / crd::math::sqrt((m + 1.5) * (whole + 1.5));
    const double small = r * r / (f * (1.0 - f)); // r² over the two smallest factors
    // (K − 1)·ln(r/(K − 1)) + (K − 1) bounds (K − 1)·ln r − ln (K − 1)!, and ln of `small` the rest; upper bounds both
    const double tail = (steps > 1.0) ? (steps - 1.0) * (bes_log_lower(r / (steps - 1.0)) + 0.002 + 1.0) : 0.0;
    const double head = (steps > 1.0)
                            ? bes_log_lower(small) + 0.002
                            : (bes_log_lower(r) + 0.002) * (steps + 1.0) + bes_log_lower(small / (r * r)) + 0.002;
    return tail + head + z2 / (whole + 1.5) < -2.0;
}

// Is s, negative, near a pole of Γ: within kStruveNearPole of it and within kStruveNearPoleScale·|s|? There gamma() and
// lgamma(), which round π·s before the sine of their reflection, carry ε·|πs|/|sin πs| (H_−99.5000000105(42.9) was
// 2.5e-9 off through its asymptotic expansion), and 1/Γ(s) and ln|Γ(s)| come from the reflection with the turn reduced
// exactly; elsewhere that error is below 2e-13 of the value and the plain functions serve, at their cost.
inline constexpr double kStruveNearPole = 0.25;
inline constexpr double kStruveNearPoleScale = 1.0e-3;
// H's series from x = kStruveCrossoverFrom down to the order kStruveCrossoverOrder cancels by up to 1e6 (its crossover,
// 122 units at worst on base): a new rounding of its first term moves its result by that much, so the first term there
// is formed as before.
inline constexpr double kStruveCrossoverFrom = 10.0;
inline constexpr double kStruveCrossoverOrder = -13.0;
[[nodiscard]] inline bool struve_near_pole(double s) noexcept
{
    const double distance = crd::math::fabs(s - crd::math::round(s));
    return s < 0.0 && distance < kStruveNearPole && distance < kStruveNearPoleScale * -s;
}
[[nodiscard]] inline double struve_rgamma_near_pole(double s) noexcept
{
    return -bes_sin_pi(-s) * gamma(1.0 - s) / kBesPi;
}
[[nodiscard]] inline double struve_lgamma_near_pole(double s) noexcept
{
    return crd::math::log(kBesPi / crd::math::fabs(bes_sin_pi(-s))) - lgamma(1.0 - s);
}

// The ascending series of H_ν (sign −1) or L_ν (sign +1). When Γ(ν+3/2) is at a pole (ν = −3/2, −5/2, …) the first
// terms vanish (1/Γ = 0) and the series starts at m = −ν − ½. With Trace set, the size of its largest term is reported
// as well (a compile-time choice: the plain series keeps its loop as it was).
template <bool Trace>
[[nodiscard]] inline double struve_series_core(double nu, double x, double sign, StruveTrace* trace) noexcept
{
    const double half = 0.5 * x;
    const double shifted = nu + 1.5;
    int first = 0; // index of the first non-zero term
    double t = 0.0;
    if (shifted <= 0.0 && shifted > -1000.0 && shifted == crd::math::floor(shifted))
    {
        first = static_cast<int>(-shifted) + 1;
        t = crd::math::pow(half, 2.0 * first + nu + 1.0) / gamma(first + 1.5); // Γ(first + ν + 3/2) = Γ(1)
        if (sign < 0.0 && (first & 1) != 0)
        {
            t = -t;
        }
    }
    else if (shifted < 0.0 &&
             (sign > 0.0 || (struve_near_pole(shifted) && (x < kStruveCrossoverFrom || nu < kStruveCrossoverOrder))))
    {
        const StruveLead lead = struve_lead_negative(nu, half);
        t = (lead.value != 0.0) ? lead.value : lead.sign * crd::math::exp(struve_lead_log(nu, half));
    }
    else
    {
        const double gammas = gamma(1.5) * gamma(nu + 1.5);
        t = crd::math::pow(half, nu + 1.0) / gammas;
        constexpr double kMax = (std::numeric_limits<double>::max)();
        if (!(gammas <= kMax) || !(crd::math::fabs(t) <= kMax))
        {
            // Γ or the power left the double range: the first term through logarithms (lgamma is ln|Γ|, and Γ is
            // negative at a negative argument whose floor is odd)
            t = crd::math::exp((nu + 1.0) * crd::math::log(half) - lgamma(1.5) - lgamma(nu + 1.5));
            if (shifted < 0.0 && (static_cast<long long>(crd::math::floor(shifted)) & 1) != 0)
            {
                t = -t;
            }
        }
    }
    [[maybe_unused]] const double lead = t;
    double sum = t;
    [[maybe_unused]] double biggest = crd::math::fabs(t);
    const double z2 = sign * half * half;
    int ask_from = 0; // see struve_may_stop
    for (int m = first + 1; m <= 500 + first; ++m)
    {
        t *= z2 / ((m + 0.5) * (m + nu + 0.5));
        sum += t;
        if constexpr (Trace)
        {
            biggest = (crd::math::fabs(t) > biggest) ? crd::math::fabs(t) : biggest;
        }
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum) && m >= ask_from)
        {
            if (struve_may_stop(m, nu, half * half))
            {
                break;
            }
            ask_from = static_cast<int>(-nu - 0.5) + 1;
        }
    }
    if constexpr (Trace)
    {
        *trace = {lead, first, biggest};
    }
    return sum;
}
[[nodiscard]] inline double struve_series(double nu, double x, double sign) noexcept
{
    return struve_series_core<false>(nu, x, sign, nullptr);
}

// Double-double numbers (an unevaluated sum hi + lo, |lo| ≤ ½ ulp(hi)): only what the negative-order L series needs.
struct StruveDD
{
    double hi;
    double lo;
};
[[nodiscard]] inline StruveDD struve_dd_fast_sum(double a, double b) noexcept // |a| ≥ |b|
{
    const double s = a + b;
    return {s, b - (s - a)};
}
[[nodiscard]] inline StruveDD struve_dd_two_sum(double a, double b) noexcept
{
    const double s = a + b;
    const double bb = s - a;
    return {s, (a - (s - bb)) + (b - bb)};
}
[[nodiscard]] inline StruveDD struve_dd_add(StruveDD a, StruveDD b) noexcept
{
    StruveDD s = struve_dd_two_sum(a.hi, b.hi);
    const StruveDD t = struve_dd_two_sum(a.lo, b.lo);
    s = struve_dd_fast_sum(s.hi, s.lo + t.hi);
    return struve_dd_fast_sum(s.hi, s.lo + t.lo);
}
[[nodiscard]] inline StruveDD struve_dd_mul(StruveDD a, StruveDD b) noexcept
{
    const double p = a.hi * b.hi;
    const double e = crd::math::fma(a.hi, b.hi, -p) + (a.hi * b.lo + a.lo * b.hi);
    return struve_dd_fast_sum(p, e);
}
[[nodiscard]] inline StruveDD struve_dd_div(StruveDD a, StruveDD b) noexcept
{
    const double q1 = a.hi / b.hi;
    StruveDD r = struve_dd_add(a, struve_dd_mul({-q1, 0.0}, b));
    const double q2 = r.hi / b.hi;
    r = struve_dd_add(r, struve_dd_mul({-q2, 0.0}, b));
    const double q3 = r.hi / b.hi;
    return struve_dd_add(struve_dd_fast_sum(q1, q2), {q3, 0.0});
}

// Σ_m r_m of the L series divided by its first term at ν < kStruveLossFrom (not a half-integer): r_0 = 1,
// r_m = r_{m−1}(x/2)²/((m+½)(m+ν+½)); with the largest |r_m|. Where the ratios leave the doubles (x well above −ν,
// where the first term is far below L), they are scaled down by exactly 2^−600 whenever the sum passes 2^600, and the
// count of those scalings is returned.
struct StruveRatios
{
    double sum;
    double biggest;
    int scalings;
};
template <bool Scaled> [[nodiscard]] inline StruveRatios struve_l_ratios(double nu, double half) noexcept
{
    const double z2 = half * half;
    double t = 1.0;
    double sum = 1.0;
    double biggest = 1.0;
    int scalings = 0;
    int ask_from = 0; // see struve_may_stop
    for (int m = 1; m <= kStruveRatioCap; ++m)
    {
        t *= z2 / ((m + 0.5) * (m + nu + 0.5));
        sum += t;
        biggest = (crd::math::fabs(t) > biggest) ? crd::math::fabs(t) : biggest;
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum) && m >= ask_from)
        {
            if (struve_may_stop(m, nu, z2))
            {
                break;
            }
            ask_from = static_cast<int>(-nu - 0.5) + 1;
        }
        if constexpr (Scaled)
        {
            if (crd::math::fabs(sum) > 0x1p600)
            {
                sum *= 0x1p-600;
                t *= 0x1p-600;
                biggest *= 0x1p-600;
                ++scalings;
            }
        }
    }
    return {sum, biggest, scalings};
}
CRD_NOINLINE inline StruveRatios struve_l_ratios_scaled(double nu, double half) noexcept
{
    return struve_l_ratios<true>(nu, half);
}

// L_ν for ν < kStruveLossFrom. At a half-integer ν = −(n + ½) the series starts at m = n with positive terms; where its
// first term or its sum leaves the doubles, L_{−(n+½)} = I_{n+½} (DLMF 11.4.4) answers. Otherwise the sum of the ratios
// Σ t_m/t_0 is formed and multiplied by the first term t_0 (struve_lead_negative). The sum alternates for
// m < −ν − 3/2, so it is checked against its largest term; where it would lose more than about two digits, it is
// carried in double-double arithmetic (Dekker 1971; Hida, Li and Bailey 2001). Where t_0 is not a normal double (its
// power or Γ(1−s) out of the doubles, or (x/2)^{ν+1} a subnormal that has lost digits: 5e-12 at ν = −150, x = 250; or
// exactly 0, where L is 1e52 to 1e156 at ν ≈ −170, x of 200 to 390), the result is sign·e^{ln|t_0| + ln|Σ|}.
[[nodiscard]] inline double struve_l_negative(double nu, double x) noexcept
{
    const double half = 0.5 * x;
    const double shifted = nu + 1.5;
    if (shifted == crd::math::floor(shifted))
    {
        StruveTrace trace{};
        const double plain = struve_series_core<true>(nu, x, 1.0, &trace);
        constexpr double kMin = (std::numeric_limits<double>::min)();
        if (trace.first != 0 && plain - plain == 0.0 && crd::math::fabs(trace.lead) >= kMin)
        {
            return plain;
        }
        return cyl_bessel_i(-nu, x);
    }
    const StruveLead lead = struve_lead_negative(nu, half);
    StruveRatios ratios = struve_l_ratios<false>(nu, half);
    double sum = ratios.sum;
    if (!(sum - sum == 0.0)) [[unlikely]]
    {
        ratios = struve_l_ratios_scaled(nu, half);
        sum = ratios.sum;
    }
    if (ratios.scalings == 0 && ratios.biggest > kStruveLossLimit * crd::math::fabs(sum))
    {
        const StruveDD z2{half * half, crd::math::fma(half, half, -(half * half))}; // (x/2)² exactly
        StruveDD t{1.0, 0.0};
        StruveDD total{1.0, 0.0};
        for (int m = 1; m <= kStruveRatioCap; ++m)
        {
            const StruveDD a = struve_dd_two_sum(static_cast<double>(m), 0.5);
            const StruveDD b = struve_dd_two_sum(static_cast<double>(m) + 0.5, nu);
            t = struve_dd_div(struve_dd_mul(t, z2), struve_dd_mul(a, b));
            total = struve_dd_add(total, t);
            if (crd::math::fabs(t.hi) <= 1e-34 * crd::math::fabs(total.hi) && m > -nu - 1.5)
            {
                break;
            }
        }
        sum = total.hi + total.lo;
    }
    if (lead.value != 0.0 && ratios.scalings == 0)
    {
        return lead.value * sum;
    }
    const double size = crd::math::exp(struve_lead_log(nu, half) + crd::math::log(crd::math::fabs(sum)) +
                                       600.0 * ratios.scalings * kStruveLn2);
    return (lead.sign * sum < 0.0) ? -size : size;
}

// H_ν = Y_ν + R_ν with R_ν = (2(x/2)^ν/(√π Γ(ν+½)))∫_0^∞ e^{−xt}(1+t²)^{ν−½} dt (DLMF 11.5.2). With s = xt the integral
// is (1/x)∫ e^{−s}(1+s²/x²)^{ν−½} ds, whose factor is analytic within |s| < x, so the Gauss–Laguerre rule converges to
// full precision for x ≥ 10 and ν < x/2. At the poles of Γ(ν+½) (ν = −½, −3/2, …) 1/Γ vanishes: H_ν = Y_ν.
[[nodiscard]] inline double struve_h_quadrature(double nu, double x) noexcept
{
    const double shifted = nu + 0.5;
    double correction = 0.0;
    if (!(shifted <= 0.0 && shifted == crd::math::floor(shifted)))
    {
        const double a = nu - 0.5;
        double sum = 0.0;
        for (int i = 0; i < 16; ++i)
        {
            const double u = kStruveNode[i] / x;
            sum += kStruveWeight[i] * crd::math::pow(1.0 + u * u, a);
        }
        // Γ(ν+½) < 0 on the intervals of negative argument where sin(π|ν+½|) > 0
        const double sign = (shifted < 0.0 && bes_sin_cos_pi(-shifted).sin > 0.0) ? -1.0 : 1.0;
        const double ln_gamma = struve_near_pole(shifted) ? struve_lgamma_near_pole(shifted) : lgamma(shifted);
        const double scale =
            sign * 2.0 * crd::math::exp(nu * crd::math::log(0.5 * x) - ln_gamma) / crd::math::sqrt(kBesPi);
        correction = scale * sum / x;
    }
    return cyl_neumann(nu, x) + correction;
}

// H_ν by the asymptotic of DLMF 11.6.1 summed to its smallest term: Σ_m t_m with t_m = Γ(m+½)/Γ(ν−m+½)·(x/2)^{ν−2m−1}.
// Consecutive terms differ by t_{m+1} = t_m·(m+½)(ν−m−½)/(x/2)² (Γ(m+3/2) = (m+½)Γ(m+½), Γ(ν−m−½) = Γ(ν−m+½)/(ν−m−½)),
// so only the first needs Γ and a power, and a Γ(ν−m+½) at a pole (a half-integer ν) makes the factor ν−m−½ vanish
// and with it every later term, exactly. Where the first term is not a finite non-zero number (a pole at m = 0, or Γ
// or the power out of the doubles) the terms are formed one by one, which tells those cases as they are. The result
// says whether the smallest term is small enough to trust. It is kept out of line: its loop is the whole cost of H at
// x ≥ 28, and the one copy costs the same wherever it is called from.
CRD_NOINLINE inline BesAsymp struve_h_asymptotic(double nu, double x) noexcept
{
    const double ry = cyl_neumann(nu, x); // Y_ν
    const double half = 0.5 * x;
    double asum = 0.0;
    double prev = 1e300;
    double lead =
        (struve_near_pole(nu + 0.5) ? gamma(0.5) * struve_rgamma_near_pole(nu + 0.5) : gamma(0.5) / gamma(nu + 0.5)) *
        crd::math::pow(half, nu - 1.0);
    if (crd::math::fabs(lead) <= (std::numeric_limits<double>::max)() && lead != 0.0)
    {
        const double inv_square = 1.0 / (half * half);
        for (int m = 0; m <= 60; ++m)
        {
            if (crd::math::fabs(lead) > prev)
            {
                break; // optimal truncation
            }
            asum += lead;
            prev = crd::math::fabs(lead);
            lead *= (m + 0.5) * (nu - m - 0.5) * inv_square;
        }
        return {ry + asum / kBesPi, prev <= kStruveAsymptoticFloor};
    }
    // A half-integer order ends the sum at the first Γ(ν−m+½) that sits at a pole (m ≥ ν + ½), so only the terms before
    // it are formed, and the loop below carries no test of its own.
    int last = 60;
    bool pole_ends = false;
    if (nu + 0.5 == crd::math::floor(nu + 0.5) && nu + 0.5 <= 61.0)
    {
        last = (nu + 0.5 <= 0.0) ? -1 : static_cast<int>(nu + 0.5) - 1;
        pole_ends = last < 60;
    }
    int m = 0;
    for (; m <= last; ++m)
    {
        const double g = struve_near_pole(nu - m + 0.5) ? gamma(m + 0.5) * struve_rgamma_near_pole(nu - m + 0.5)
                                                        : gamma(m + 0.5) / gamma(nu - m + 0.5);
        const double term = g * crd::math::pow(half, nu - 2.0 * m - 1.0);
        if (crd::math::fabs(term) > prev)
        {
            break; // optimal truncation
        }
        asum += term;
        prev = crd::math::fabs(term);
    }
    if (pole_ends && m > last)
    {
        prev = 0.0;
    }
    return {ry + asum / kBesPi, prev <= kStruveAsymptoticFloor};
}

// H_ν for ν ≥ x/2 by the downward recurrence H_{μ−1} = (2μ/x)H_μ − H_{μ+1} + (x/2)^μ/(√πΓ(μ+3/2)) (DLMF 11.4.23), run
// from the order ν₀ = ν + n at which the ascending series, which cancels by about e^{x²/(4ν₀)}, loses at most
// e^{kStruveSeedLoss}. From ν ≥ x/2 down to ν the values only grow, so the recurrence adds no cancellation. The first
// series term at ν₀ is reached from the one at the fractional order by one factor (x/2)/(μ+3/2) per order, so its tiny
// size costs no more than the rounding of those factors (a logarithm of it would lose digits as its magnitude grows).
// Everything is carried in units of that term (the source term is then 1/x), and the power-of-two factors that keep it
// in range are counted and applied once at the end.
[[nodiscard]] inline double struve_h_down(double nu, double x) noexcept
{
    const double half = 0.5 * x;
    const double wanted = x * x / (4.0 * kStruveSeedLoss) - nu;
    const int steps =
        (wanted <= 0.0) ? 0 : ((wanted >= kStruveMaxSteps) ? kStruveMaxSteps : static_cast<int>(wanted) + 1);
    const double start = nu + steps; // ν₀
    double unit = 0.0;               // (x/2)^{ν₀+1}/(Γ(3/2)Γ(ν₀+3/2)), held as unit·2^{500·scale}
    int scale = 0;
    if (start <= kStruveMaxSteps)
    {
        const int whole = static_cast<int>(start);
        const double frac = start - whole;
        unit = crd::math::pow(half, frac + 1.0) / (gamma(1.5) * gamma(frac + 1.5));
        for (int j = 0; j < whole; ++j)
        {
            unit *= half / (frac + j + 1.5);
            if (unit < kStruveSmall)
            {
                unit *= kStruveBig;
                --scale;
            }
        }
    }
    else
    {
        unit = crd::math::exp((start + 1.0) * crd::math::log(half) - lgamma(1.5) - lgamma(start + 1.5));
    }
    // Σ_m (−(x/2)²)^m / ∏_{j≤m}((j+½)(j+ν₀+½)): the series of H_{ν₀} divided by its first term; the same for ν₀+1.
    double series_lo = 1.0;
    double series_hi = 1.0;
    {
        double t_lo = 1.0;
        double t_hi = 1.0;
        const double z2 = -half * half;
        for (int m = 1; m <= 500; ++m)
        {
            t_lo *= z2 / ((m + 0.5) * (m + start + 0.5));
            t_hi *= z2 / ((m + 0.5) * (m + start + 1.5));
            series_lo += t_lo;
            series_hi += t_hi;
            if (crd::math::fabs(t_lo) <= 1e-18 * crd::math::fabs(series_lo) &&
                crd::math::fabs(t_hi) <= 1e-18 * crd::math::fabs(series_hi))
            {
                break;
            }
        }
    }
    double lo = series_lo * unit;                        // H_μ, μ = ν₀
    double hi = series_hi * unit * half / (start + 1.5); // H_{μ+1}
    double source = unit / x;                            // (x/2)^μ/(√πΓ(μ+3/2))
    double mu = start;
    for (int j = 0; j < steps; ++j)
    {
        const double next = (2.0 * mu / x) * lo - hi + source;
        hi = lo;
        lo = next;
        source *= (mu + 0.5) / half;
        mu -= 1.0;
        if (crd::math::fabs(lo) > kStruveBig)
        {
            lo *= kStruveSmall;
            hi *= kStruveSmall;
            source *= kStruveSmall;
            ++scale;
        }
    }
    for (; scale > 0; --scale)
    {
        lo *= kStruveBig;
    }
    for (; scale < 0; ++scale)
    {
        lo *= kStruveSmall;
    }
    return lo;
}

[[nodiscard]] inline double struve_h_impl(double nu, double x) noexcept
{
    // The series also serves a very negative order while x² < 16(½ − ν): there it does not cancel, and the quadrature
    // below, whose factor (1+s²/x²)^{ν−½} narrows with |ν − ½|, needs x² of about 16|ν − ½|.
    if (x < kStruveSeriesLimit || (nu < 0.0 && x * x < 16.0 * (0.5 - nu)))
    {
        return struve_series(nu, x, -1.0);
    }
    if (x >= kStruveAsymptoticFrom && nu < 0.9 * x)
    {
        const BesAsymp tail = struve_h_asymptotic(nu, x);
        if (tail.reliable)
        {
            return tail.value;
        }
    }
    return (nu >= 0.5 * x) ? struve_h_down(nu, x) : struve_h_quadrature(nu, x);
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
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    if (n < detail::kStruveLossFrom) [[unlikely]]
    {
        return static_cast<T>(detail::struve_l_negative(n, xd));
    }
    return static_cast<T>(detail::struve_series(n, xd, 1.0));
}

} // namespace crd::hesap::special
