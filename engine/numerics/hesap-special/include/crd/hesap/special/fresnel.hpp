#pragma once

// crd-hesap-special v12-d — Fresnel integrals S(x)=∫₀ˣ sin(πt²/2)dt, C(x)=∫₀ˣ cos(πt²/2)dt (NR `frenel`):
// power series for |x| ≤ 1.5, complex continued fraction for larger |x|. Gated vs scipy.special.fresnel to <1e-13.

#include <crd/hesap/special/gamma.hpp> // Real concept

#include <crd/math/cmath.hpp>
#include <complex>

namespace crd::hesap::special
{
namespace detail
{
inline void fresnel_impl(double x, double& s, double& c) noexcept
{
    constexpr double eps = 1.0e-16;
    constexpr int maxit = 100;
    constexpr double fpmin = 1.0e-300;
    constexpr double xmin = 1.5;
    constexpr double piby2 = 1.5707963267948966192313216916397514;
    const double ax = crd::math::fabs(x);
    if (ax < crd::math::sqrt(fpmin))
    {
        s = 0.0;
        c = ax;
    }
    else if (ax <= xmin) // power series
    {
        double sum = 0.0;
        double sums = 0.0;
        double sumc = ax;
        double sign = 1.0;
        double fact = piby2 * ax * ax;
        bool odd = true;
        double term = ax;
        int n = 3;
        int k = 1;
        for (; k <= maxit; ++k)
        {
            term *= fact / k;
            sum += sign * term / n;
            const double test = crd::math::fabs(sum) * eps;
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
            if (term < test)
            {
                break;
            }
            odd = !odd;
            n += 2;
        }
        s = sums;
        c = sumc;
    }
    else // continued fraction (complex)
    {
        const double pix2 = 3.14159265358979323846264338327950288 * ax * ax;
        std::complex<double> b(1.0, -pix2);
        std::complex<double> cc(1.0 / fpmin, 0.0);
        std::complex<double> d = 1.0 / b;
        std::complex<double> h = d;
        int n = -1;
        for (int k = 2; k <= maxit; ++k)
        {
            n += 2;
            const double a = -static_cast<double>(n) * (n + 1);
            b += 4.0;
            d = 1.0 / (a * d + b);
            cc = b + a / cc;
            const std::complex<double> del = cc * d;
            h *= del;
            if (crd::math::fabs(del.real() - 1.0) + crd::math::fabs(del.imag()) < eps)
            {
                break;
            }
        }
        h *= std::complex<double>(ax, -ax);
        const std::complex<double> cs =
            std::complex<double>(0.5, 0.5) *
            (1.0 - std::complex<double>(crd::math::cos(0.5 * pix2), crd::math::sin(0.5 * pix2)) * h);
        c = cs.real();
        s = cs.imag();
    }
    if (x < 0.0)
    {
        c = -c;
        s = -s;
    }
}
} // namespace detail

template <Real T>
[[nodiscard]] T fresnel_s(T x) noexcept
{
    double s = 0.0;
    double c = 0.0;
    detail::fresnel_impl(static_cast<double>(x), s, c);
    return static_cast<T>(s);
}
template <Real T>
[[nodiscard]] T fresnel_c(T x) noexcept
{
    double s = 0.0;
    double c = 0.0;
    detail::fresnel_impl(static_cast<double>(x), s, c);
    return static_cast<T>(c);
}

} // namespace crd::hesap::special
