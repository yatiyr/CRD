#pragma once

// crd-hesap-special v12-b — Airy Ai/Bi (+ derivatives) and Kelvin ber/bei/ker/kei.
//
// Airy via the Bessel connection (DLMF §9.6), the route Numerical Recipes §6.7 `airy` also takes: with
// ζ = (2/3)|x|^{3/2}, Ai/Bi reduce to K_{1/3},I_{±1/3} (x>0) or J_{±1/3} (x<0) of order 1/3 and 2/3 — riding the
// gold-standard bes_ik_core/bes_jy_core of bessel.hpp. The branch formulas below are written from DLMF 9.6.2–9.6.9 and
// the reflections I_{−ν} (DLMF 10.27.2) and J_{−ν} (DLMF 10.2.3). Exact at x=0.
// Kelvin ber/bei/ker/kei via their ascending series (exact factorial coefficients + ψ at the integers for the 2nd-kind
// terms, from a table of compensated sums); accurate for moderate |x| (the common regime), gated vs scipy. All gated vs
// scipy.special (airy, ker/kei…).

#include <crd/hesap/special/bessel.hpp>

#include <crd/math/cmath.hpp>
#include <complex>

namespace crd::hesap::special
{
namespace detail
{
inline constexpr double kAiInvRt3 = 0.577350269189625764509148780501957456; // 1/√3
inline constexpr double kAiThird = 1.0 / 3.0;
inline constexpr double kAiTwoThird = 2.0 / 3.0;
inline constexpr double kAiSinThird = 0.866025403784438646763723170752936183; // sin(π/3) = sin(2π/3) = √3/2
inline constexpr double kAi0 = 0.355028053887817239260063186004183682;        // Ai(0)  = 3^{-2/3}/Γ(2/3)
inline constexpr double kAip0 = -0.258819403792806798405183560189525976;      // Ai'(0) = −3^{-1/3}/Γ(1/3)

// Values and slopes of Ai and Bi at the nodes −6, −7, −8, −9 (the published function values, to double precision),
// where Taylor's series takes over below the Maclaurin range: with y(x0 + t) = Σ c_k t^k the equation y'' = x y gives
// (k+1)(k+2) c_{k+2} = x0 c_k + c_{k−1}, so two coefficient sequences (c_0, c_1 = 1, 0 and 0, 1) serve all four
// functions. For |t| ≤ ½ its terms are of the size of the function (no e^ζ growth) and fall below 1e-17 by k = 21.
struct AiryNode
{
    double ai;
    double aip;
    double bi;
    double bip;
};
inline constexpr AiryNode kAiryNodes[4] = {
    {-0.3291451736298231, 0.3459354872813429, -0.14669837667055705, -0.812898785105067},
    {0.18428083525050565, -0.7710081684101265, 0.293762071854414, 0.4982445900581135},
    {-0.0527050503563862, 0.9355609381983065, -0.33125158075113786, -0.1594504978129814},
    {-0.022133721547341403, -0.9756639809263316, 0.3249473234552449, -0.05740051384366925}};
inline constexpr double kAiryTaylorEdge = -5.5; // from here down to −9 the Taylor series serves
inline constexpr int kAiryTaylorTerms = 22;     // coefficients c_0 … c_21

// Ai, Bi, Ai', Bi' on the far negative axis (ζ ≥ kBesHankelFrom), where the continued fraction of the Bessel core
// needs about ζ steps (its cap is reached near x = −600) and ζ itself carries ε·ζ of rounding: Hankel's expansion, with
// ζ as a double-double (bes_jy_zeta). It returns its four values by value, and is kept out of line. `ok` says whether
// the expansion can be trusted.
struct AiryFour
{
    double ai;
    double bi;
    double aip;
    double bip;
    bool ok;
};
CRD_NOINLINE inline AiryFour airy_far_negative(double z) noexcept
{
    const BesHankelJY third = bes_jy_zeta(z, kAiThird);
    const BesHankelJY two = bes_jy_zeta(z, kAiTwoThird);
    const double j_neg_third = 0.5 * third.j - kAiSinThird * third.y; // as in the Bessel branch of airy_all
    const double j_neg_two = -0.5 * two.j - kAiSinThird * two.y;
    const double root = crd::math::sqrt(z);
    return {(root / 3.0) * (third.j + j_neg_third), (root * kAiInvRt3) * (j_neg_third - third.j),
            (z / 3.0) * (two.j - j_neg_two), (z * kAiInvRt3) * (j_neg_two + two.j), third.reliable && two.reliable};
}

// Below this x (ζ ≥ about 1000) the far negative axis begins; the public functions test for it before airy_all.
inline constexpr double kAiFarNegative = -132.0;

// Which of the four values a caller wants. The Bessel branches below need one core for Ai and Bi (order 1/3) and the
// other for Ai' and Bi' (order 2/3), and skip the one that is not wanted: a compile-time choice, so it does not depend
// on the compiler inlining a core and then dropping its unused result.
inline constexpr unsigned kAiWantAi = 1U;
inline constexpr unsigned kAiWantBi = 2U;
inline constexpr unsigned kAiWantAip = 4U;
inline constexpr unsigned kAiWantBip = 8U;

// Ai, Bi, Ai', Bi' at once (the ones named in Want; the others are left unspecified on the Bessel branches).
template <unsigned Want>
CRD_FORCEINLINE void airy_all(double x, double& ai, double& bi, double& aip, double& bip) noexcept
{
    const double absx = crd::math::fabs(x);
    if (x == 0.0)
    {
        ai = kAi0;
        bi = kAi0 / kAiInvRt3; // Bi(0) = √3·Ai(0)
        aip = kAip0;
        bip = -kAip0 / kAiInvRt3;
        return;
    }
    // Direct Maclaurin series Ai=c1 f − c2 g, Bi=√3(c1 f + c2 g) — far faster than the Bessel route. Asymmetric
    // cutoff: for x>0 Ai/Bi decay so the series cancels earlier (x<4.5); for x<0 it oscillates, but its terms reach
    // about e^ζ with ζ = (2/3)|x|^{3/2}, so the rounding they leave is ~e^ζ·ε. Against scipy it stays inside the
    // 1e-11 relative / 1e-13 absolute gate (at worst half of it, at a zero) down to x = −5.5; by x = −9 it is 2.6e3
    // times the gate. Below −5.5 Taylor's series about the nodes −6 … −9 serves (next block) and, from −9, the Bessel
    // route.
    if (x < 4.5 && x > -5.5)
    {
        const double x3 = x * x * x;
        double cf = 1.0; // current f-term c_k x^{3k}
        double cg = x;   // current g-term d_k x^{3k+1}
        double f = 1.0;
        double g = x;
        double fp = 0.0;
        double gp = 1.0;
        for (int k = 1; k <= 80; ++k)
        {
            cf *= x3 / ((3.0 * k - 1.0) * (3.0 * k));
            f += cf;
            if constexpr ((Want & (kAiWantAip | kAiWantBip)) != 0U)
            {
                fp += (3.0 * k) * cf / x; // 3k·c_k x^{3k−1}
            }
            cg *= x3 / ((3.0 * k) * (3.0 * k + 1.0));
            g += cg;
            if constexpr ((Want & (kAiWantAip | kAiWantBip)) != 0U)
            {
                gp += (3.0 * k + 1.0) * cg / x;
            }
            if (crd::math::fabs(cf) + crd::math::fabs(cg) < 1e-18 * (crd::math::fabs(f) + crd::math::fabs(g)))
            {
                break;
            }
        }
        const double c1 = kAi0;   // Ai(0)
        const double c2 = -kAip0; // −Ai'(0)
        ai = c1 * f - c2 * g;
        bi = (c1 * f + c2 * g) / kAiInvRt3;
        aip = c1 * fp - c2 * gp;
        bip = (c1 * fp + c2 * gp) / kAiInvRt3;
        return;
    }
    if (x <= kAiryTaylorEdge && x > -9.0)
    {
        const int node = static_cast<int>(0.5 - x) - 6; // the nearest of −6 … −9
        const double x0 = -6.0 - node;
        const double t = x - x0;
        double c[kAiryTaylorTerms];
        double d[kAiryTaylorTerms];
        c[0] = 1.0;
        c[1] = 0.0;
        d[0] = 0.0;
        d[1] = 1.0;
        c[2] = x0 * c[0] * 0.5;
        d[2] = x0 * d[0] * 0.5;
        for (int k = 1; k + 2 < kAiryTaylorTerms; ++k)
        {
            const double inv = 1.0 / ((k + 1.0) * (k + 2.0));
            c[k + 2] = (x0 * c[k] + c[k - 1]) * inv;
            d[k + 2] = (x0 * d[k] + d[k - 1]) * inv;
        }
        const AiryNode& n = kAiryNodes[node];
        if ((Want & (kAiWantAi | kAiWantBi)) != 0U)
        {
            double f = c[kAiryTaylorTerms - 1];
            double g = d[kAiryTaylorTerms - 1];
            for (int k = kAiryTaylorTerms - 2; k >= 0; --k)
            {
                f = f * t + c[k];
                g = g * t + d[k];
            }
            ai = n.ai * f + n.aip * g;
            bi = n.bi * f + n.bip * g;
        }
        if ((Want & (kAiWantAip | kAiWantBip)) != 0U)
        {
            double fp = (kAiryTaylorTerms - 1.0) * c[kAiryTaylorTerms - 1];
            double gp = (kAiryTaylorTerms - 1.0) * d[kAiryTaylorTerms - 1];
            for (int k = kAiryTaylorTerms - 2; k >= 1; --k)
            {
                fp = fp * t + k * c[k];
                gp = gp * t + k * d[k];
            }
            aip = n.ai * fp + n.aip * gp;
            bip = n.bi * fp + n.bip * gp;
        }
        return;
    }
    const double rootx = crd::math::sqrt(absx);
    const double zeta = kAiTwoThird * absx * rootx; // ζ = (2/3)|x|^{3/2}
    if (x > 0.0)
    {
        // DLMF 9.6.2–9.6.5 for ζ = (2/3)x^{3/2}:
        //   Ai = (1/π)√(x/3) K_{1/3},          Ai' = −(x/(π√3)) K_{2/3},
        //   Bi = √(x/3) (I_{−1/3} + I_{1/3}),  Bi' = (x/√3) (I_{−2/3} + I_{2/3}).
        // I_{−ν} is not computed by the core; DLMF 10.27.2 gives I_{−ν} = I_ν + (2/π) sin(νπ) K_ν, and sin(νπ) is
        // √3/2 for both ν = 1/3 and ν = 2/3.
        const BesIK third = ((Want & (kAiWantAi | kAiWantBi)) != 0U) ? bes_ik_core(zeta, kAiThird) : BesIK{};
        const BesIK two = ((Want & (kAiWantAip | kAiWantBip)) != 0U) ? bes_ik_core(zeta, kAiTwoThird) : BesIK{};
        const double i_neg_third = third.i + (2.0 / kBesPi) * kAiSinThird * third.k;
        const double i_neg_two = two.i + (2.0 / kBesPi) * kAiSinThird * two.k;
        const double root_x_third = rootx * kAiInvRt3; // √(x/3)
        const double x_over_rt3 = x * kAiInvRt3;       // x/√3
        ai = root_x_third * third.k / kBesPi;
        aip = -x_over_rt3 * two.k / kBesPi;
        bi = root_x_third * (i_neg_third + third.i);
        bip = x_over_rt3 * (i_neg_two + two.i);
    }
    else if (x < 0.0)
    {
        // DLMF 9.6.6–9.6.9 for z = −x > 0 and ζ = (2/3)z^{3/2}:
        //   Ai(−z) = (√z/3) (J_{1/3} + J_{−1/3}),          Ai'(−z) = (z/3) (J_{2/3} − J_{−2/3}),
        //   Bi(−z) = √(z/3) (J_{−1/3} − J_{1/3}),          Bi'(−z) = (z/√3) (J_{−2/3} + J_{2/3}).
        // J_{−ν} comes from the reflection J_{−ν} = cos(νπ) J_ν − sin(νπ) Y_ν (DLMF 10.2.3 with 10.2.4's Y), where
        // cos(π/3) = ½, cos(2π/3) = −½ and sin is √3/2 for both orders.
        const BesJY third = ((Want & (kAiWantAi | kAiWantBi)) != 0U) ? bes_jy_core(zeta, kAiThird) : BesJY{};
        const BesJY two = ((Want & (kAiWantAip | kAiWantBip)) != 0U) ? bes_jy_core(zeta, kAiTwoThird) : BesJY{};
        const double j_neg_third = 0.5 * third.j - kAiSinThird * third.y;
        const double j_neg_two = -0.5 * two.j - kAiSinThird * two.y;
        const double root_z_third = rootx * kAiInvRt3; // √(z/3)
        const double z_over_rt3 = absx * kAiInvRt3;    // z/√3
        ai = (rootx / 3.0) * (third.j + j_neg_third);
        aip = (absx / 3.0) * (two.j - j_neg_two);
        bi = root_z_third * (j_neg_third - third.j);
        bip = z_over_rt3 * (j_neg_two + two.j);
    }
    else
    {
        ai = kAi0;
        bi = kAi0 / kAiInvRt3; // Bi(0) = √3 · Ai(0)
        aip = kAip0;
        bip = -kAip0 / kAiInvRt3; // Bi'(0) = −√3 · Ai'(0)
    }
}
} // namespace detail

template <Real T>
[[nodiscard]] T airy_ai(T x) noexcept
{
    const double xd = static_cast<double>(x);
    if (xd < detail::kAiFarNegative) [[unlikely]]
    {
        const detail::AiryFour edge = detail::airy_far_negative(-xd);
        if (edge.ok)
        {
            return static_cast<T>(edge.ai);
        }
    }
    double ai = 0.0;
    double bi = 0.0;
    double aip = 0.0;
    double bip = 0.0;
    detail::airy_all<detail::kAiWantAi>(xd, ai, bi, aip, bip);
    return static_cast<T>(ai);
}
template <Real T>
[[nodiscard]] T airy_bi(T x) noexcept
{
    const double xd = static_cast<double>(x);
    if (xd < detail::kAiFarNegative) [[unlikely]]
    {
        const detail::AiryFour edge = detail::airy_far_negative(-xd);
        if (edge.ok)
        {
            return static_cast<T>(edge.bi);
        }
    }
    double ai = 0.0;
    double bi = 0.0;
    double aip = 0.0;
    double bip = 0.0;
    detail::airy_all<detail::kAiWantBi>(xd, ai, bi, aip, bip);
    return static_cast<T>(bi);
}
template <Real T>
[[nodiscard]] T airy_ai_prime(T x) noexcept
{
    const double xd = static_cast<double>(x);
    if (xd < detail::kAiFarNegative) [[unlikely]]
    {
        const detail::AiryFour edge = detail::airy_far_negative(-xd);
        if (edge.ok)
        {
            return static_cast<T>(edge.aip);
        }
    }
    double ai = 0.0;
    double bi = 0.0;
    double aip = 0.0;
    double bip = 0.0;
    detail::airy_all<detail::kAiWantAip>(xd, ai, bi, aip, bip);
    return static_cast<T>(aip);
}
template <Real T>
[[nodiscard]] T airy_bi_prime(T x) noexcept
{
    const double xd = static_cast<double>(x);
    if (xd < detail::kAiFarNegative) [[unlikely]]
    {
        const detail::AiryFour edge = detail::airy_far_negative(-xd);
        if (edge.ok)
        {
            return static_cast<T>(edge.bip);
        }
    }
    double ai = 0.0;
    double bi = 0.0;
    double aip = 0.0;
    double bip = 0.0;
    detail::airy_all<detail::kAiWantBip>(xd, ai, bi, aip, bip);
    return static_cast<T>(bip);
}

// ---- Kelvin functions ber/bei/ker/kei (x ≥ 0) ----
// ber(x)=Σ(−1)^k (x/2)^{4k}/[(2k)!]² , bei(x)=Σ(−1)^k (x/2)^{4k+2}/[(2k+1)!]².
// ker(x)=−ln(x/2)ber(x)+(π/4)bei(x)+Σ(−1)^k ψ(2k+1)(x/2)^{4k}/[(2k)!]².
// kei(x)=−ln(x/2)bei(x)−(π/4)ber(x)+Σ(−1)^k ψ(2k+2)(x/2)^{4k+2}/[(2k+1)!]².
namespace detail
{
// Large-x path: ber+i·bei = I₀(x e^{iπ/4}), ker+i·kei = K₀(x e^{iπ/4}); both via the standard modified-Bessel
// asymptotic Σ a_k/zᵏ with optimal truncation — avoids the catastrophic cancellation of the ascending series (ker/kei
// are exponentially small while the series terms are exponentially large).
inline void kelvin_asymptotic(double x, double& ber, double& bei, double& ker, double& kei) noexcept
{
    const std::complex<double> z(x / crd::math::sqrt(2.0), x / crd::math::sqrt(2.0)); // x·e^{iπ/4} = (x/√2)(1+i)
    const std::complex<double> invz = 1.0 / z;
    std::complex<double> sump(1.0, 0.0);
    std::complex<double> sumq(1.0, 0.0);
    std::complex<double> tp(1.0, 0.0);
    std::complex<double> tq(1.0, 0.0);
    double prev = 1e300;
    for (int k = 1; k <= 60; ++k)
    {
        const double f = (2.0 * k - 1.0) * (2.0 * k - 1.0) / (8.0 * k);
        tp *= f * invz;  // I₀ coefficients (all +)
        tq *= -f * invz; // K₀ coefficients (alternating)
        sump += tp;
        sumq += tq;
        const double m = std::abs(tq);
        if (m > prev) // optimal truncation: stop when the asymptotic terms start growing
        {
            break;
        }
        prev = m;
    }
    const std::complex<double> iz = crd::math::exp(z) / crd::math::sqrt(2.0 * kBesPi * z) * sump;
    const std::complex<double> kz = crd::math::sqrt(kBesPi / (2.0 * z)) * crd::math::exp(-z) * sumq;
    ber = iz.real();
    bei = iz.imag();
    ker = kz.real();
    kei = kz.imag();
}

// ψ(n) = −γ + Σ_{j<n} 1/j for n = 1 … 2·60 + 2 (DLMF 5.4.14), the weights of the terms of the ker and kei series, as
// a table made at compile time: the harmonic sum is compensated (Neumaier 1974), so each entry is good to the rounding
// of its last step, and a call of ψ per term (two per iteration of the series) is a load.
inline constexpr int kKelvinPsiCount = 2 * 60 + 3;
struct KelvinPsiTable
{
    double psi[kKelvinPsiCount];
};
[[nodiscard]] constexpr KelvinPsiTable kelvin_make_psi_table() noexcept
{
    KelvinPsiTable table{};
    constexpr double kEulerGamma = 0.57721566490153286060651209008240243;
    double sum = 0.0;   // Σ_{j<n} 1/j, the larger part
    double carry = 0.0; // what the additions rounded away
    for (int n = 1; n < kKelvinPsiCount; ++n)
    {
        table.psi[n] = (sum + carry) - kEulerGamma;
        const double term = 1.0 / n;
        const double next = sum + term;
        carry += (sum >= term) ? ((sum - next) + term) : ((term - next) + sum);
        sum = next;
    }
    return table;
}
inline constexpr KelvinPsiTable kKelvinPsi = kelvin_make_psi_table();

// ker and kei decay like e^{−x/√2} while the terms of their series reach e^{x/√2}, so the series loses e^{√2 x}·ε
// (3e-7 at x = 11, 1e-5 at x = 18) and the asymptotic, whose floor is e^{−2x}, is the better one from x ≈ 9 (the
// crossover is ~2e-8 at worst, at x = 8.2, for every switch between 8.4 and 9.2: mpmath sweep). ber and bei grow, so
// their series cancels far less, and the one-term I₀ expansion drops an e^{−√2 x} part (2e-7 at x = 11): they keep the
// series up to x = 18.
inline constexpr double kKelvinKerAsymptoticFrom = 9.0;

// ber, bei, ker, kei at once; with WantK false only ber and bei are formed (ker and kei are left unspecified), with
// WantK true ker and kei are formed (and ber and bei are not wanted from x = kKelvinKerAsymptoticFrom on, where the
// asymptotic serves ker and kei alone).
template <bool WantK>
CRD_FORCEINLINE void kelvin_all(double x, double& ber, double& bei, double& ker, double& kei) noexcept
{
    if (x >= 18.0) // asymptotic regime (series cancellation would dominate)
    {
        kelvin_asymptotic(x, ber, bei, ker, kei);
        return;
    }
    if (WantK && x >= kKelvinKerAsymptoticFrom)
    {
        double ber_asymptotic = 0.0;
        double bei_asymptotic = 0.0;
        kelvin_asymptotic(x, ber_asymptotic, bei_asymptotic, ker, kei);
        return;
    }
    const double t = 0.25 * x * x; // (x/2)²; (x/2)^{4k} = t^{2k}
    // ber / bei series.
    double br = 1.0;        // k=0 term of ber: (x/2)^0/[(0)!]² = 1
    double term_br = 1.0;   // current ber term
    double bi = t;          // k=0 term of bei: (x/2)²/[1!]² = t
    double term_bi = t;
    double sber = br;
    double sbei = bi;
    // ker / kei series accumulators (ψ-weighted).
    double sker = kKelvinPsi.psi[1] * 1.0; // k=0: ψ(1)·1
    double skei = kKelvinPsi.psi[2] * t;   // k=0: ψ(2)·t
    for (int k = 1; k <= 60; ++k)
    {
        // ber term k: (−1)^k t^{2k} / [(2k)!]²  →  ratio from k−1: × (−1) t² / [(2k−1)(2k)]²
        const double d2k = (2.0 * k - 1.0) * (2.0 * k);
        term_br *= -t * t / (d2k * d2k);
        sber += term_br;
        if constexpr (WantK)
        {
            sker += kKelvinPsi.psi[2 * k + 1] * term_br;
        }
        // bei term k: (−1)^k t^{2k+1} / [(2k+1)!]² → ratio from k−1: × (−1) t² / [(2k)(2k+1)]²
        const double e2k = (2.0 * k) * (2.0 * k + 1.0);
        term_bi *= -t * t / (e2k * e2k);
        sbei += term_bi;
        if constexpr (WantK)
        {
            skei += kKelvinPsi.psi[2 * k + 2] * term_bi;
        }
        if (crd::math::fabs(term_br) + crd::math::fabs(term_bi) <
            1e-18 * (crd::math::fabs(sber) + crd::math::fabs(sbei)))
        {
            break;
        }
    }
    ber = sber;
    bei = sbei;
    if constexpr (WantK)
    {
        const double lg = crd::math::log(0.5 * x);
        ker = -lg * sber + 0.25 * kBesPi * sbei + sker;
        kei = -lg * sbei - 0.25 * kBesPi * sber + skei;
    }
}
} // namespace detail

template <Real T>
[[nodiscard]] T kelvin_ber(T x) noexcept
{
    double ber = 0.0;
    double bei = 0.0;
    double ker = 0.0;
    double kei = 0.0;
    detail::kelvin_all<false>(static_cast<double>(x), ber, bei, ker, kei);
    return static_cast<T>(ber);
}
template <Real T>
[[nodiscard]] T kelvin_bei(T x) noexcept
{
    double ber = 0.0;
    double bei = 0.0;
    double ker = 0.0;
    double kei = 0.0;
    detail::kelvin_all<false>(static_cast<double>(x), ber, bei, ker, kei);
    return static_cast<T>(bei);
}
template <Real T>
[[nodiscard]] T kelvin_ker(T x) noexcept
{
    double ber = 0.0;
    double bei = 0.0;
    double ker = 0.0;
    double kei = 0.0;
    detail::kelvin_all<true>(static_cast<double>(x), ber, bei, ker, kei);
    return static_cast<T>(ker);
}
template <Real T>
[[nodiscard]] T kelvin_kei(T x) noexcept
{
    double ber = 0.0;
    double bei = 0.0;
    double ker = 0.0;
    double kei = 0.0;
    detail::kelvin_all<true>(static_cast<double>(x), ber, bei, ker, kei);
    return static_cast<T>(kei);
}

} // namespace crd::hesap::special
