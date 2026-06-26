#pragma once

// crd-hesap-special v12-d — exponential & trigonometric integrals.
//   E₁(x)=∫ₓ^∞ e^{−t}/t dt · Eₙ(x)=∫₁^∞ e^{−xt}/tⁿ dt · Ei(x)=−∫_{−x}^∞ e^{−t}/t dt (Cauchy PV, x>0)
//   Si(x)=∫₀ˣ sin t/t dt · Ci(x)=γ+ln x+∫₀ˣ (cos t−1)/t dt
// Eₙ/E₁ via series (x≤1) + modified-Lentz continued fraction (x>1); Ei via series (small x) + asymptotic (large x);
// Si/Ci via series (|x|≤2) + the complex continued fraction of NR `cisi` (|x|>2). Gated vs scipy.special
// (exp1/expi/expn/sici) to <1e-13. (NR §6.3 algorithms; internals f64, public templates cast for f32.)

#include <crd/hesap/special/gamma.hpp> // Real concept

#include <crd/math/cmath.hpp>
#include <complex>
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

// Eₙ(x), n ≥ 0, x ≥ 0 (NR `expint`).
[[nodiscard]] inline double expint_en_impl(int n, double x) noexcept
{
    const int nm1 = n - 1;
    if (n < 0 || x < 0.0 || (x == 0.0 && (n == 0 || n == 1)))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (n == 0)
    {
        return crd::math::exp(-x) / x; // E₀(x) = e^{−x}/x
    }
    if (x == 0.0)
    {
        return 1.0 / nm1; // Eₙ(0) = 1/(n−1), n>1
    }
    if (x > 1.0) // modified Lentz continued fraction
    {
        double b = x + n;
        double c = 1.0 / kEiFpMin;
        double d = 1.0 / b;
        double h = d;
        for (int i = 1; i <= kEiMaxIt; ++i)
        {
            const double a = -1.0 * i * (nm1 + i);
            b += 2.0;
            d = 1.0 / (a * d + b);
            c = b + a / c;
            const double del = c * d;
            h *= del;
            if (crd::math::fabs(del - 1.0) <= kEiEps)
            {
                return h * crd::math::exp(-x);
            }
        }
        return h * crd::math::exp(-x);
    }
    double ans = (nm1 != 0) ? 1.0 / nm1 : (-crd::math::log(x) - kEuler); // series
    double fact = 1.0;
    for (int i = 1; i <= kEiMaxIt; ++i)
    {
        fact *= -x / i;
        double del;
        if (i != nm1)
        {
            del = -fact / (i - nm1);
        }
        else
        {
            double psi = -kEuler;
            for (int ii = 1; ii <= nm1; ++ii)
            {
                psi += 1.0 / ii;
            }
            del = fact * (-crd::math::log(x) + psi);
        }
        ans += del;
        if (crd::math::fabs(del) < crd::math::fabs(ans) * kEiEps)
        {
            return ans;
        }
    }
    return ans;
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
    return expint_en_impl(1, x); // x > 40 (rare, not perf-critical): the modified-Lentz CF
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
    double sum = 0.0; // x > 40: asymptotic
    double term = 1.0;
    for (int k = 1; k <= kEiMaxIt; ++k)
    {
        const double prev = term;
        term *= static_cast<double>(k) / x;
        if (term < kEiEps)
        {
            break;
        }
        if (term < prev)
        {
            sum += term;
        }
        else
        {
            sum -= prev;
            break;
        }
    }
    return crd::math::exp(x) * (1.0 + sum) / x;
}

// Si(x), Ci(x) (NR `cisi`); x real (si odd, ci for x>0).
inline void cisi_impl(double x, double& ci, double& si) noexcept
{
    const double t = crd::math::fabs(x);
    if (t == 0.0)
    {
        ci = -std::numeric_limits<double>::infinity();
        si = 0.0;
        return;
    }
    if (t > 2.0) // continued fraction: evaluate via complex Lentz of (1+i·t)
    {
        std::complex<double> b(1.0, t);
        std::complex<double> c(1.0 / kEiFpMin, 0.0);
        std::complex<double> d = 1.0 / b;
        std::complex<double> h = d;
        for (int i = 2; i <= kEiMaxIt; ++i)
        {
            const double a = -(i - 1) * (i - 1);
            b += 2.0;
            d = 1.0 / (a * d + b);
            c = b + a / c;
            const std::complex<double> del = c * d;
            h *= del;
            if (crd::math::fabs(del.real() - 1.0) + crd::math::fabs(del.imag()) <= kEiEps)
            {
                break;
            }
        }
        h *= std::complex<double>(crd::math::cos(t), -crd::math::sin(t));
        ci = -h.real();
        si = 1.5707963267948966 + h.imag();
    }
    else // power series
    {
        double sumc = 0.0;
        double sums = 0.0;
        if (t < crd::math::sqrt(kEiFpMin))
        {
            sumc = 0.0;
            sums = t;
        }
        else
        {
            double sum = 0.0;
            double sign = 1.0;
            double fact = 1.0;
            bool odd = true;
            int k = 1;
            for (; k <= kEiMaxIt; ++k)
            {
                fact *= t / k;
                const double term = fact / k;
                sum += sign * term;
                const double err = term / crd::math::fabs(sum);
                if (odd)
                {
                    sign = -sign;
                    sums = sum;
                    sum = sumc;
                }
                else
                {
                    sumc = sum;
                    sum = sums;
                }
                if (err < kEiEps)
                {
                    break;
                }
                odd = !odd;
            }
        }
        si = sums;
        ci = sumc + crd::math::log(t) + kEuler;
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
