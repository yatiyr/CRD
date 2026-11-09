#pragma once

// crd-hesap-special v12-d — Struve H_ν(x) and modified Struve L_ν(x), x ≥ 0.
//   H_ν(x) = Σ_{m≥0} (−1)^m (x/2)^{2m+ν+1} / (Γ(m+3/2)Γ(m+ν+3/2))   (alternating ⇒ cancels for large x)
//   L_ν(x) = Σ_{m≥0}        (x/2)^{2m+ν+1} / (Γ(m+3/2)Γ(m+ν+3/2))   (all-positive for ν > −3/2 ⇒ accurate everywhere)
// For L at a more negative order Γ(m+ν+3/2) changes sign for m < −ν − 3/2, so the sum alternates there and loses digits
// (1e-9 relative at ν = −100, x ≈ 63; ν > −35 stays inside 1e-12). Below ν = −30 the sum is checked against its largest
// term, and where it loses more than a couple of digits it is formed again in double-double arithmetic (Dekker 1971;
// Hida, Li and Bailey 2001), whose 32 digits leave the result at the rounding of its leading factor.
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
    for (int m = first + 1; m <= 500 + first; ++m)
    {
        t *= z2 / ((m + 0.5) * (m + nu + 0.5));
        sum += t;
        if constexpr (Trace)
        {
            biggest = (crd::math::fabs(t) > biggest) ? crd::math::fabs(t) : biggest;
        }
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum))
        {
            break;
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

// L_ν for ν < kStruveLossFrom. The sum alternates for m < −ν − 3/2, so it is checked against its largest term; where
// it would lose more than about two digits, the sum of ratios Σ t_m/t_first is carried in double-double arithmetic (the
// leading factor, which scales every term alike, stays a double: it is the whole error left).
[[nodiscard]] inline double struve_l_negative(double nu, double x) noexcept
{
    StruveTrace trace{};
    double plain = struve_series_core<true>(nu, x, 1.0, &trace);
    if (plain != plain && crd::math::fabs(trace.lead) == std::numeric_limits<double>::infinity())
    {
        return trace.lead; // the leading term overflowed: it is the whole result, with its sign
    }
    const double half = 0.5 * x;
    const double log_power = (nu + 1.0) * crd::math::log(half);
    if (trace.first == 0 && log_power < -700.0 && trace.lead != 0.0)
    {
        // (x/2)^{ν+1} is a subnormal here and has lost digits (5e-12 at ν = −150, x = 250): the leading factor, which
        // scales every term alike, is formed again through logarithms (ln|Γ|, and Γ < 0 where the floor is odd)
        double accurate = crd::math::exp(log_power - lgamma(1.5) - lgamma(nu + 1.5));
        if ((static_cast<long long>(crd::math::floor(nu + 1.5)) & 1) != 0)
        {
            accurate = -accurate;
        }
        plain *= accurate / trace.lead;
        trace.lead = accurate;
    }
    if (!(trace.biggest > kStruveLossLimit * crd::math::fabs(plain)) || trace.first != 0)
    {
        return plain;
    }
    const StruveDD z2{half * half, crd::math::fma(half, half, -(half * half))}; // (x/2)² exactly
    StruveDD t{1.0, 0.0};
    StruveDD sum{1.0, 0.0};
    for (int m = 1; m <= 600; ++m)
    {
        const StruveDD a = struve_dd_two_sum(static_cast<double>(m), 0.5);
        const StruveDD b = struve_dd_two_sum(static_cast<double>(m) + 0.5, nu);
        t = struve_dd_div(struve_dd_mul(t, z2), struve_dd_mul(a, b));
        sum = struve_dd_add(sum, t);
        if (crd::math::fabs(t.hi) <= 1e-34 * crd::math::fabs(sum.hi) && m > -nu - 1.5)
        {
            break;
        }
    }
    return trace.lead * (sum.hi + sum.lo);
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
        const double scale =
            sign * 2.0 * crd::math::exp(nu * crd::math::log(0.5 * x) - lgamma(shifted)) / crd::math::sqrt(kBesPi);
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
    double lead = gamma(0.5) / gamma(nu + 0.5) * crd::math::pow(half, nu - 1.0);
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
        const double g = gamma(m + 0.5) / gamma(nu - m + 0.5);
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
