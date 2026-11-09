#pragma once

// crd-hesap-special v12-d — exponential & trigonometric integrals.
//   E₁(x)=∫ₓ^∞ e^{−t}/t dt · Eₙ(x)=∫₁^∞ e^{−xt}/tⁿ dt · Ei(x)=−∫_{−x}^∞ e^{−t}/t dt (Cauchy PV, x>0)
//   Si(x)=∫₀ˣ sin t/t dt · Ci(x)=γ+ln x+∫₀ˣ (cos t−1)/t dt
// Eₙ/E₁ via the convergent series (x≤1, DLMF §8.19(iv)) + the contracted continued fraction (x>1, DLMF §8.19(vii));
// Ei via minimax (small/mid x) + the asymptotic series (large x, DLMF §6.12(i)); Si/Ci via the Maclaurin series
// (|x|≤2, DLMF §6.6(i)) + the continued fraction of E₁(ix) (|x|>2, DLMF §6.9 with the §6.5 link to Si/Ci). Both
// fractions are summed from their convergents' successive differences (Euler–Wallis form). Gated vs scipy.special
// (exp1/expi/expn/sici) to <1e-13. (Internals f64, public templates cast for f32.)

#include <crd/hesap/special/gamma.hpp> // Real concept

#include <crd/math/cmath.hpp>
#include <limits>

#include <crd/hesap/special/poly_eval.hpp> // detail::horner_t for the iteration-free fast paths
#include "expint_poly.inc"                  // GENERATED E1/Ei minimax coeffs (gen_expint_poly.py; v12-d perf)

namespace crd::hesap::special
{
namespace detail
{
inline constexpr double kEuler = 0.577215664901532860606512090082402431; // γ
inline constexpr double kEiEps = 1.0e-16;
inline constexpr double kEiFpMin = 1.0e-300;
inline constexpr int kEiMaxIt = 10000;

// Eₙ(x), n ≥ 0, x ≥ 0 (DLMF §8.19): the convergent series for x ≤ 1, the continued fraction for x > 1.
[[nodiscard]] inline double expint_en_impl(int n, double x) noexcept
{
    if (n < 0 || x < 0.0 || (x == 0.0 && n < 2) || x != x)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (n == 0)
    {
        return crd::math::exp(-x) / x; // E₀(x) = e^{−x}/x
    }
    const int order = n - 1;
    if (x == 0.0)
    {
        return 1.0 / order; // Eₙ(0) = 1/(n−1), n>1
    }
    if (x > 1.0)
    {
        // Eₙ(x) = e^{−x}/(β₁ + α₂/(β₂ + α₃/(β₃ + …))), β_k = x+n+2(k−1), α_k = −(k−1)(n+k−2) (the even part of the
        // fraction in DLMF §8.19(vii)). With q_k = β_k·q_{k−1} + α_k·q_{k−2} the convergents differ by
        // Δ_k = −α_k·(q_{k−2}/q_k)·Δ_{k−1}, Δ_1 = 1/β₁, and the fraction's value is ΣΔ_k: all terms are positive
        // and each step costs a single division.
        double q_lag = 1.0;
        double q_now = x + n;
        double beta = q_now;
        double step = 1.0 / q_now;
        double total = step;
        for (int k = 2; k <= kEiMaxIt; ++k)
        {
            beta += 2.0;
            const double alpha = -static_cast<double>(k - 1) * (static_cast<double>(order) + (k - 1));
            const double q_next = beta * q_now + alpha * q_lag;
            step *= -alpha * q_lag / q_next;
            total += step;
            if (step <= kEiEps * total)
            {
                break;
            }
            q_lag = q_now;
            q_now = q_next;
            if (q_now > 1.0e100) // the q's grow factorially: rescale (only their ratios enter)
            {
                q_now *= 1.0e-100;
                q_lag *= 1.0e-100;
            }
        }
        return total * crd::math::exp(-x);
    }
    // Series (DLMF §8.19(iv), 8.19.8): Eₙ(x) = (−x)^{n−1}/(n−1)!·(ψ(n) − ln x) − Σ_{k≥0, k≠n−1} (−x)^k / (k!·(k−n+1)).
    // The ordinary terms are (−x)^k/k! / (n−1−k); the one logarithmic term sits at k = n−1 and is added once, from its
    // closed coefficient, between the sum below that index and the sum above it.
    double total = (order != 0) ? 1.0 / order : 0.0; // the ordinary k = 0 term
    double series_factor = 1.0;                      // (−x)^k / k!
    for (int k = 1; k < order; ++k)
    {
        series_factor *= -x / k;
        const double step = series_factor / (order - k);
        total += step;
        if (crd::math::fabs(step) < crd::math::fabs(total) * kEiEps)
        {
            return total; // the terms above, the logarithmic one included, are smaller still
        }
    }
    if (order != 0)
    {
        series_factor *= -x / order; // (−x)^{n−1}/(n−1)!
    }
    double digamma = -kEuler; // ψ(n) = −γ + Σ_{j<n} 1/j
    for (int j = 1; j <= order; ++j)
    {
        digamma += 1.0 / j;
    }
    total += series_factor * (digamma - crd::math::log(x));
    for (int k = order + 1; k <= kEiMaxIt; ++k)
    {
        series_factor *= -x / k;
        const double step = series_factor / (order - k);
        total += step;
        if (crd::math::fabs(step) < crd::math::fabs(total) * kEiEps)
        {
            break;
        }
    }
    return total;
}

// E1(x), x > 0 — iteration-free minimax (v12-d perf): −ln x + A(x) near 0, then e^−x/x·rational(x e^x E1);
// the Cody decomposition factors out the e^−x decay so relative accuracy holds where E1 is tiny. CF fallback x>40.
[[nodiscard]] inline double expint_e1_fast(double x) noexcept
{
    if (x <= 1.0)
    {
        return -crd::math::log(x) + horner_t(kE1lowA, 2.0 * x - 1.0); // A = E1 + ln x is entire ⇒ fast polynomial
    }
    if (x <= 8.0)
    {
        const double t = (2.0 * x - 9.0) / 7.0;
        return crd::math::exp(-x) / x * (horner_t(kE1B1P, t) / horner_t(kE1B1Q, t));
    }
    if (x <= kExpintHi)
    {
        const double t = (2.0 * x - 48.0) / 32.0;
        return crd::math::exp(-x) / x * (horner_t(kE1B2P, t) / horner_t(kE1B2Q, t));
    }
    return expint_en_impl(1, x); // x > 40 (rare, not perf-critical): the continued fraction
}

// Ei(x), x > 0 — iteration-free minimax (v12-d perf): ln x + C(x) near 0 (C analytic; weight capped at Ei's zero),
// then e^x/x·rational(x e^−x Ei). Asymptotic fallback x>40.
[[nodiscard]] inline double expint_ei_impl(double x) noexcept
{
    if (x <= 0.0)
    {
        if (x == 0.0)
        {
            return -std::numeric_limits<double>::infinity();
        }
        return -expint_en_impl(1, -x); // Ei(x) = −E₁(−x) for x < 0
    }
    if (x <= 2.0)
    {
        if (x < kEiFpMin)
        {
            return crd::math::log(x) + kEuler;
        }
        return crd::math::log(x) + horner_t(kEilowC, x - 1.0); // C = Ei − ln x is entire ⇒ fast polynomial
    }
    if (x <= 8.0)
    {
        const double t = (2.0 * x - 10.0) / 6.0;
        return crd::math::exp(x) / x * (horner_t(kEiD1P, t) / horner_t(kEiD1Q, t));
    }
    if (x <= 18.0)
    {
        const double t = (2.0 * x - 26.0) / 10.0;
        return crd::math::exp(x) / x * (horner_t(kEiD2P, t) / horner_t(kEiD2Q, t));
    }
    if (x <= kExpintHi)
    {
        const double t = (2.0 * x - 58.0) / 22.0;
        return crd::math::exp(x) / x * (horner_t(kEiD3P, t) / horner_t(kEiD3Q, t));
    }
    // x > 40: Ei(x) ~ (e^x/x)·Σ_k k!/x^k (DLMF §6.12(i)), cut at the smallest term (or once a term drops below ε).
    // The terms t_k = k!/x^k fall while k < x, so the sum is cut at the first term below ε (at the latest when k
    // reaches x, where the smallest term sits); each term is advanced from the one before it, t_{k+1} = t_k·(k+1)/x.
    const double inv_x = 1.0 / x;
    double tail = 0.0; // Σ k!/x^k over the kept terms
    double term = inv_x;
    for (int k = 2; term >= kEiEps && k <= kEiMaxIt && static_cast<double>(k) <= x; ++k)
    {
        tail += term;
        term *= static_cast<double>(k) * inv_x;
    }
    return crd::math::exp(x) * (1.0 + tail) / x;
}

// Si(x), Ci(x); x real (si odd, ci for x>0).
inline void cisi_impl(double x, double& ci, double& si) noexcept
{
    const double t = crd::math::fabs(x);
    if (t == 0.0)
    {
        ci = -std::numeric_limits<double>::infinity();
        si = 0.0;
        return;
    }
    if (t > 2.0)
    {
        // E₁(it) = e^{−it}·K, K = 1/(β₁ + α₂/(β₂ + …)), β_k = 2k−1 + it, α_k = −(k−1)² (DLMF §6.9), and
        // Ci(t) = −Re E₁(it), Si(t) = π/2 + Im E₁(it) (§6.5). K is summed from its convergents' differences
        // Δ_k = −α_k·(q_{k−2}/q_k)·Δ_{k−1} (Δ₁ = 1/β₁, q_k = β_k·q_{k−1} + α_k·q_{k−2}), all complex, one division a
        // step.
        double lag_re = 1.0;
        double lag_im = 0.0;
        double now_re = 1.0;
        double now_im = t;
        const double inv_t = 1.0 / t; // 1/β₁ = (1 − it)/(1 + t²), t > 2
        const double norm1 = 1.0 / (1.0 + inv_t * inv_t);
        double d_re = inv_t * inv_t * norm1;
        double d_im = -inv_t * norm1;
        double k_re = d_re;
        double k_im = d_im;
        double beta_re = 1.0;
        // for t beyond 1e100 the second term is below 1e-200 of the first (and q would overflow): β₁ alone is exact
        const int last = (t < 1.0e100) ? kEiMaxIt : 1;
        for (int k = 2; k <= last; ++k)
        {
            beta_re += 2.0;
            const double alpha = -static_cast<double>((k - 1) * (k - 1));
            const double next_re = beta_re * now_re - t * now_im + alpha * lag_re;
            const double next_im = beta_re * now_im + t * now_re + alpha * lag_im;
            const double weight = -alpha / (next_re * next_re + next_im * next_im);
            const double ratio_re = (lag_re * next_re + lag_im * next_im) * weight; // −α·q_{k−2}/q_k
            const double ratio_im = (lag_im * next_re - lag_re * next_im) * weight;
            const double step_re = d_re * ratio_re - d_im * ratio_im;
            d_im = d_re * ratio_im + d_im * ratio_re;
            d_re = step_re;
            k_re += d_re;
            k_im += d_im;
            if (crd::math::fabs(d_re) + crd::math::fabs(d_im) <=
                kEiEps * (crd::math::fabs(k_re) + crd::math::fabs(k_im)))
            {
                break;
            }
            lag_re = now_re;
            lag_im = now_im;
            now_re = next_re;
            now_im = next_im;
            if (crd::math::fabs(now_re) + crd::math::fabs(now_im) > 1.0e100) // only the q ratios enter: rescale
            {
                now_re *= 1.0e-100;
                now_im *= 1.0e-100;
                lag_re *= 1.0e-100;
                lag_im *= 1.0e-100;
            }
        }
        double s;
        double c;
        crd::math::sincos(t, s, c);
        ci = -(k_re * c + k_im * s);
        si = 1.5707963267948966 + (k_im * c - k_re * s);
    }
    else if (t < crd::math::sqrt(kEiFpMin))
    {
        si = t;
        ci = crd::math::log(t) + kEuler;
    }
    else
    {
        // Maclaurin series (DLMF §6.6): Si = Σ (−1)^k t^{2k+1}/((2k+1)(2k+1)!), Ci = γ + ln t + Σ_{k≥1} (−1)^k
        // t^{2k}/(2k·(2k)!)
        const double t2 = t * t;
        double odd = t; // (−1)^k t^{2k+1}/(2k+1)!
        double sine = t;
        for (int k = 1; k <= kEiMaxIt; ++k)
        {
            odd *= -t2 / (static_cast<double>(2 * k) * (2 * k + 1));
            const double part = odd / (2 * k + 1);
            sine += part;
            if (crd::math::fabs(part) <= kEiEps * crd::math::fabs(sine))
            {
                break;
            }
        }
        double even = 1.0; // (−1)^k t^{2k}/(2k)!
        double cosine = 0.0;
        for (int k = 1; k <= kEiMaxIt; ++k)
        {
            even *= -t2 / (static_cast<double>(2 * k - 1) * (2 * k));
            const double part = even / (2 * k);
            cosine += part;
            if (crd::math::fabs(part) <= kEiEps * crd::math::fabs(cosine))
            {
                break;
            }
        }
        si = sine;
        ci = cosine + crd::math::log(t) + kEuler;
    }
    if (x < 0.0)
    {
        si = -si;
    }
}
} // namespace detail

template <Real T>
[[nodiscard]] T expint_en(int n, T x) noexcept
{
    return static_cast<T>(detail::expint_en_impl(n, static_cast<double>(x)));
}
template <Real T>
[[nodiscard]] T expint_e1(T x) noexcept
{
    const double xd = static_cast<double>(x);
    return static_cast<T>(xd > 0.0 ? detail::expint_e1_fast(xd) : detail::expint_en_impl(1, xd));
}
template <Real T>
[[nodiscard]] T expint_ei(T x) noexcept
{
    return static_cast<T>(detail::expint_ei_impl(static_cast<double>(x)));
}
template <Real T>
[[nodiscard]] T sinint(T x) noexcept // Si
{
    double ci = 0.0;
    double si = 0.0;
    detail::cisi_impl(static_cast<double>(x), ci, si);
    return static_cast<T>(si);
}
template <Real T>
[[nodiscard]] T cosint(T x) noexcept // Ci
{
    double ci = 0.0;
    double si = 0.0;
    detail::cisi_impl(static_cast<double>(x), ci, si);
    return static_cast<T>(ci);
}

} // namespace crd::hesap::special
