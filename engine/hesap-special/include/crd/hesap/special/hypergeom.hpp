#pragma once

// crd-hesap-special v12-d — (confluent) hypergeometric functions.
//   ₀F₁(;b;z) = Σ zᵏ/((b)ₖ k!)            (entire)
//   ₁F₁(a;b;z) = M(a,b,z) = Σ (a)ₖ/((b)ₖ k!) zᵏ   (entire; z<0 via Kummer M(a,b,z)=e^z M(b−a,b,−z) to kill cancellation)
//   ₂F₁(a,b;c;z) = Σ (a)ₖ(b)ₖ/((c)ₖ k!) zᵏ (|z|<1; Pfaff (1−z)^{−a}₂F₁(a,c−b;c;z/(z−1)) maps z∈(−∞,½] to |·|≤½)
// Gated vs scipy.special hyp0f1/hyp1f1/hyp2f1 to <1e-12 in the convergent region (the z→1 connection formula for
// ₂F₁ near the unit circle is a documented follow-on). Internals f64.

#include <crd/hesap/special/gamma.hpp> // Real concept

#include <crd/math/cmath.hpp>

namespace crd::hesap::special
{
namespace detail
{
[[nodiscard]] inline double hyp0f1_impl(double b, double z) noexcept
{
    double t = 1.0;
    double sum = 1.0;
    for (int k = 1; k <= 1000; ++k)
    {
        t *= z / ((b + k - 1.0) * k);
        sum += t;
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum))
        {
            break;
        }
    }
    return sum;
}

[[nodiscard]] inline double hyp1f1_series(double a, double b, double z) noexcept
{
    double t = 1.0;
    double sum = 1.0;
    for (int k = 1; k <= 5000; ++k)
    {
        t *= (a + k - 1.0) / (b + k - 1.0) * z / k;
        sum += t;
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum))
        {
            break;
        }
    }
    return sum;
}
[[nodiscard]] inline double hyp1f1_impl(double a, double b, double z) noexcept
{
    if (z < 0.0)
    {
        return crd::math::exp(z) * hyp1f1_series(b - a, b, -z); // Kummer transform (positive arg ⇒ no cancellation)
    }
    return hyp1f1_series(a, b, z);
}

[[nodiscard]] inline double hyp2f1_series(double a, double b, double c, double z) noexcept
{
    double t = 1.0;
    double sum = 1.0;
    for (int k = 1; k <= 5000; ++k)
    {
        t *= (a + k - 1.0) * (b + k - 1.0) / ((c + k - 1.0) * k) * z;
        sum += t;
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum))
        {
            break;
        }
    }
    return sum;
}
// Degenerate z→1 connection, c = a+b+m, m ≥ 0 integer (DLMF 15.8.8 — the exact log form, no ε-shift).
[[nodiscard]] inline double hyp2f1_conn_deg(double a, double b, int m, double z) noexcept
{
    const double omz = 1.0 - z; // > 0 for z < 1
    const double c = a + b + m;
    double res = 0.0;
    if (m >= 1) // finite sum
    {
        const double pref1 = gamma(static_cast<double>(m)) * gamma(c) / (gamma(a + m) * gamma(b + m));
        double s1 = 0.0;
        double term = 1.0;
        for (int k = 0; k < m; ++k)
        {
            s1 += term;
            term *= (a + k) * (b + k) / ((k + 1.0) * (1.0 - m + k)) * omz;
        }
        res += pref1 * s1;
    }
    const double pref2 = gamma(c) / (gamma(a) * gamma(b));
    const double zm = ((m & 1) ? -1.0 : 1.0) * crd::math::pow(omz, static_cast<double>(m)); // (z−1)^m
    double s2 = 0.0;
    double ck = 1.0 / gamma(m + 1.0); // (a+m)_0(b+m)_0/(0!·m!) = 1/m!
    double zk = 1.0;                  // (1−z)^k
    for (int k = 0; k <= 5000; ++k)
    {
        const double lg = crd::math::log(omz) - digamma(k + 1.0) - digamma(k + m + 1.0) + digamma(a + k + m) +
                          digamma(b + k + m);
        const double term = ck * zk * lg;
        s2 += term;
        if (k > 0 && crd::math::fabs(term) <= 1e-18 * crd::math::fabs(s2))
        {
            break;
        }
        ck *= (a + m + k) * (b + m + k) / ((k + 1.0) * (k + m + 1.0));
        zk *= omz;
    }
    return res - pref2 * zm * s2;
}

// z→1 connection (z∈(0.5,1)): exact log form when c−a−b is integer, else the two-term Γ formula (DLMF 15.8.4).
[[nodiscard]] inline double hyp2f1_connection(double a, double b, double c, double z) noexcept
{
    const double cab = c - a - b;
    const double rm = crd::math::round(cab);
    if (crd::math::fabs(cab - rm) < 1e-9) // degenerate
    {
        const int m = static_cast<int>(rm);
        if (m >= 0)
        {
            return hyp2f1_conn_deg(a, b, m, z);
        }
        // c−a−b = −n < 0: Euler F = (1−z)^{c−a−b} F(c−a,c−b;c;z), inner has c−a−b = n > 0.
        return crd::math::pow(1.0 - z, cab) * hyp2f1_conn_deg(c - a, c - b, -m, z);
    }
    const double w1 = gamma(c) * gamma(cab) / (gamma(c - a) * gamma(c - b));
    const double w2 = gamma(c) * gamma(-cab) / (gamma(a) * gamma(b));
    return w1 * hyp2f1_series(a, b, 1.0 - cab, 1.0 - z) +
           w2 * crd::math::pow(1.0 - z, cab) * hyp2f1_series(c - a, c - b, 1.0 + cab, 1.0 - z);
}

[[nodiscard]] inline double hyp2f1_impl(double a, double b, double c, double z) noexcept
{
    if (z >= -0.5 && z <= 0.5)
    {
        return hyp2f1_series(a, b, c, z); // fast region
    }
    if (z > 0.5 && z < 1.0)
    {
        return hyp2f1_connection(a, b, c, z); // exact even for integer c−a−b
    }
    if (z < -0.5) // Pfaff → w=z/(z−1)∈(⅓,1); recurse into series/connection (covers |z|>1 + degenerate b−a)
    {
        const double w = z / (z - 1.0);
        return crd::math::pow(1.0 - z, -a) * hyp2f1_impl(a, c - b, c, w);
    }
    return hyp2f1_series(a, b, c, z); // z ≥ 1: on the branch cut (real ₂F₁ multivalued) — best-effort
}

// General ₚFq(a₁..aₚ; b₁..b_q; z) = Σ_k (∏(aᵢ)ₖ / ∏(bⱼ)ₖ) zᵏ/k! (convergent: p≤q entire, p=q+1 needs |z|<1).
[[nodiscard]] inline double pfq_impl(const double* a, int p, const double* b, int q, double z) noexcept
{
    double t = 1.0;
    double sum = 1.0;
    for (int k = 1; k <= 5000; ++k)
    {
        double r = z / k;
        for (int i = 0; i < p; ++i)
        {
            r *= (a[i] + k - 1.0);
        }
        for (int j = 0; j < q; ++j)
        {
            r /= (b[j] + k - 1.0);
        }
        t *= r;
        sum += t;
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum))
        {
            break;
        }
    }
    return sum;
}
} // namespace detail

template <Real T>
[[nodiscard]] T hyp0f1(T b, T z) noexcept
{
    return static_cast<T>(detail::hyp0f1_impl(static_cast<double>(b), static_cast<double>(z)));
}
template <Real T>
[[nodiscard]] T hyp1f1(T a, T b, T z) noexcept
{
    return static_cast<T>(detail::hyp1f1_impl(static_cast<double>(a), static_cast<double>(b), static_cast<double>(z)));
}
template <Real T>
[[nodiscard]] T hyp2f1(T a, T b, T c, T z) noexcept
{
    return static_cast<T>(
        detail::hyp2f1_impl(static_cast<double>(a), static_cast<double>(b), static_cast<double>(c),
                            static_cast<double>(z)));
}

// General ₚFq (p, q ≤ 16). Convergent regimes: p ≤ q (entire), p = q+1 (|z| < 1).
template <Real T>
[[nodiscard]] T pfq(const T* a, int p, const T* b, int q, T z) noexcept
{
    double ad[16];
    double bd[16];
    for (int i = 0; i < p; ++i)
    {
        ad[i] = static_cast<double>(a[i]);
    }
    for (int j = 0; j < q; ++j)
    {
        bd[j] = static_cast<double>(b[j]);
    }
    return static_cast<T>(detail::pfq_impl(ad, p, bd, q, static_cast<double>(z)));
}

} // namespace crd::hesap::special
