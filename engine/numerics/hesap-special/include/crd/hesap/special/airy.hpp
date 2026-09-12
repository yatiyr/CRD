#pragma once

// crd-hesap-special v12-b — Airy Ai/Bi (+ derivatives) and Kelvin ber/bei/ker/kei.
//
// Airy via the Bessel connection (NR §6.7 `airy`): with ζ = (2/3)|x|^{3/2}, Ai/Bi reduce to K_{1/3},I_{1/3} (x>0) or
// J_{±1/3},Y_{±1/3} (x<0) of order 1/3 and 2/3 — riding the gold-standard `bessjy`/`bessik` core. Exact at x=0.
// Kelvin ber/bei/ker/kei via their ascending series (exact factorial coefficients + digamma for the 2nd-kind ψ
// terms); accurate for moderate |x| (the common regime), gated vs scipy. All gated vs scipy.special (airy, ker/kei…).

#include <crd/hesap/special/bessel.hpp>
#include <crd/hesap/special/gamma.hpp> // digamma for ker/kei

#include <crd/math/cmath.hpp>
#include <complex>

namespace crd::hesap::special
{
namespace detail
{
inline constexpr double kAiInvRt3 = 0.577350269189625764509148780501957456; // 1/√3
inline constexpr double kAiThird = 1.0 / 3.0;
inline constexpr double kAiTwoThird = 2.0 / 3.0;
inline constexpr double kAi0 = 0.355028053887817239260063186004183682;   // Ai(0)  = 3^{-2/3}/Γ(2/3)
inline constexpr double kAip0 = -0.258819403792806798405183560189525976; // Ai'(0) = −3^{-1/3}/Γ(1/3)

// Ai, Bi, Ai', Bi' at once.
inline void airy_all(double x, double& ai, double& bi, double& aip, double& bip) noexcept
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
    // cutoff: for x>0 Ai/Bi decay so the series cancels earlier (x<4.5); for x<0 it oscillates (accurate to |x|<9).
    if (x < 4.5 && x > -9.0)
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
            fp += (3.0 * k) * cf / x; // 3k·c_k x^{3k−1}
            cg *= x3 / ((3.0 * k) * (3.0 * k + 1.0));
            g += cg;
            gp += (3.0 * k + 1.0) * cg / x;
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
    const double rootx = crd::math::sqrt(absx);
    const double z = kAiTwoThird * absx * rootx;
    if (x > 0.0)
    {
        double ri = 0.0;
        double rk = 0.0;
        double rip = 0.0;
        double rkp = 0.0;
        bessik(z, kAiThird, ri, rk, rip, rkp);
        ai = rootx * kAiInvRt3 * rk / kBesPi;
        bi = rootx * (rk / kBesPi + 2.0 * kAiInvRt3 * ri);
        bessik(z, kAiTwoThird, ri, rk, rip, rkp);
        aip = -x * kAiInvRt3 * rk / kBesPi;
        bip = x * (rk / kBesPi + 2.0 * kAiInvRt3 * ri);
    }
    else if (x < 0.0)
    {
        double rj = 0.0;
        double ry = 0.0;
        double rjp = 0.0;
        double ryp = 0.0;
        bessjy(z, kAiThird, rj, ry, rjp, ryp);
        ai = 0.5 * rootx * (rj - kAiInvRt3 * ry);
        bi = -0.5 * rootx * (ry + kAiInvRt3 * rj);
        bessjy(z, kAiTwoThird, rj, ry, rjp, ryp);
        aip = 0.5 * absx * (kAiInvRt3 * ry + rj);
        bip = 0.5 * absx * (kAiInvRt3 * rj - ry);
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
    double ai = 0.0;
    double bi = 0.0;
    double aip = 0.0;
    double bip = 0.0;
    detail::airy_all(static_cast<double>(x), ai, bi, aip, bip);
    return static_cast<T>(ai);
}
template <Real T>
[[nodiscard]] T airy_bi(T x) noexcept
{
    double ai = 0.0;
    double bi = 0.0;
    double aip = 0.0;
    double bip = 0.0;
    detail::airy_all(static_cast<double>(x), ai, bi, aip, bip);
    return static_cast<T>(bi);
}
template <Real T>
[[nodiscard]] T airy_ai_prime(T x) noexcept
{
    double ai = 0.0;
    double bi = 0.0;
    double aip = 0.0;
    double bip = 0.0;
    detail::airy_all(static_cast<double>(x), ai, bi, aip, bip);
    return static_cast<T>(aip);
}
template <Real T>
[[nodiscard]] T airy_bi_prime(T x) noexcept
{
    double ai = 0.0;
    double bi = 0.0;
    double aip = 0.0;
    double bip = 0.0;
    detail::airy_all(static_cast<double>(x), ai, bi, aip, bip);
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

inline void kelvin_all(double x, double& ber, double& bei, double& ker, double& kei) noexcept
{
    if (x >= 18.0) // asymptotic regime (series cancellation would dominate)
    {
        kelvin_asymptotic(x, ber, bei, ker, kei);
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
    double sker = digamma(1.0) * 1.0;          // k=0: ψ(1)·1
    double skei = digamma(2.0) * t;            // k=0: ψ(2)·t
    for (int k = 1; k <= 60; ++k)
    {
        // ber term k: (−1)^k t^{2k} / [(2k)!]²  →  ratio from k−1: × (−1) t² / [(2k−1)(2k)]²
        const double d2k = (2.0 * k - 1.0) * (2.0 * k);
        term_br *= -t * t / (d2k * d2k);
        sber += term_br;
        sker += digamma(2.0 * k + 1.0) * term_br;
        // bei term k: (−1)^k t^{2k+1} / [(2k+1)!]² → ratio from k−1: × (−1) t² / [(2k)(2k+1)]²
        const double e2k = (2.0 * k) * (2.0 * k + 1.0);
        term_bi *= -t * t / (e2k * e2k);
        sbei += term_bi;
        skei += digamma(2.0 * k + 2.0) * term_bi;
        if (crd::math::fabs(term_br) + crd::math::fabs(term_bi) < 1e-18 * (crd::math::fabs(sber) + crd::math::fabs(sbei)))
        {
            break;
        }
    }
    ber = sber;
    bei = sbei;
    const double lg = crd::math::log(0.5 * x);
    ker = -lg * sber + 0.25 * kBesPi * sbei + sker;
    kei = -lg * sbei - 0.25 * kBesPi * sber + skei;
}
} // namespace detail

template <Real T>
[[nodiscard]] T kelvin_ber(T x) noexcept
{
    double ber = 0.0;
    double bei = 0.0;
    double ker = 0.0;
    double kei = 0.0;
    detail::kelvin_all(static_cast<double>(x), ber, bei, ker, kei);
    return static_cast<T>(ber);
}
template <Real T>
[[nodiscard]] T kelvin_bei(T x) noexcept
{
    double ber = 0.0;
    double bei = 0.0;
    double ker = 0.0;
    double kei = 0.0;
    detail::kelvin_all(static_cast<double>(x), ber, bei, ker, kei);
    return static_cast<T>(bei);
}
template <Real T>
[[nodiscard]] T kelvin_ker(T x) noexcept
{
    double ber = 0.0;
    double bei = 0.0;
    double ker = 0.0;
    double kei = 0.0;
    detail::kelvin_all(static_cast<double>(x), ber, bei, ker, kei);
    return static_cast<T>(ker);
}
template <Real T>
[[nodiscard]] T kelvin_kei(T x) noexcept
{
    double ber = 0.0;
    double bei = 0.0;
    double ker = 0.0;
    double kei = 0.0;
    detail::kelvin_all(static_cast<double>(x), ber, bei, ker, kei);
    return static_cast<T>(kei);
}

} // namespace crd::hesap::special
