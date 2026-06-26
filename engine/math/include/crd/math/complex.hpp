#pragma once

// crd-math complex transcendentals — deterministic complex exp/log/sqrt/pow/trig/hyperbolic + abs/arg/polar/norm/
// conj, built on the REAL crd::math cores. The moat extends to complex math: every result is bit-identical on every
// platform (the real cores are), where std::complex<T> exp/log/... call the platform libm (varying) + carry
// overhead. std::complex<T> in/out so engine code (Bessel/Airy, FFT twiddles, complex LA) drops in unchanged.

#include <crd/math/hyperbolic.hpp>     // real sinh/cosh
#include <crd/math/power.hpp>          // real pow/hypot
#include <crd/math/select.hpp>         // real sqrt/copysign
#include <crd/math/transcendental.hpp> // real exp/log/sin/cos/atan2 (+ trig.hpp)

#include <complex>

namespace crd::math
{
template <class T>
[[nodiscard]] inline T abs(const std::complex<T>& z) noexcept
{
    return hypot(z.real(), z.imag());
}
template <class T>
[[nodiscard]] inline T arg(const std::complex<T>& z) noexcept
{
    return atan2(z.imag(), z.real());
}
template <class T>
[[nodiscard]] inline T norm(const std::complex<T>& z) noexcept
{
    return z.real() * z.real() + z.imag() * z.imag(); // exact (no transcendental)
}
template <class T>
[[nodiscard]] inline std::complex<T> conj(const std::complex<T>& z) noexcept
{
    return {z.real(), -z.imag()};
}
template <class T>
[[nodiscard]] inline std::complex<T> polar(T r, T theta) noexcept
{
    return {r * cos(theta), r * sin(theta)};
}

// exp(a+bi) = e^a·(cos b + i·sin b)
template <class T>
[[nodiscard]] inline std::complex<T> exp(const std::complex<T>& z) noexcept
{
    const T e = exp(z.real());
    return {e * cos(z.imag()), e * sin(z.imag())};
}
// log(z) = ln|z| + i·arg(z)  (real hypot/atan2 directly — avoids ADL ambiguity with std::abs/arg on complex)
template <class T>
[[nodiscard]] inline std::complex<T> log(const std::complex<T>& z) noexcept
{
    return {log(hypot(z.real(), z.imag())), atan2(z.imag(), z.real())};
}
// sqrt(z) — Kahan's cancellation-free form
template <class T>
[[nodiscard]] inline std::complex<T> sqrt(const std::complex<T>& z) noexcept
{
    const T a = z.real();
    const T b = z.imag();
    if (a == T(0) && b == T(0))
    {
        return {T(0), b}; // sqrt(±0±0i) = +0 ± 0i
    }
    const T m = hypot(a, b);
    if (a >= T(0))
    {
        const T re = sqrt(T(0.5) * (m + a));
        return {re, b / (re + re)};
    }
    const T im = copysign(sqrt(T(0.5) * (m - a)), b);
    return {b / (im + im), im};
}
// pow(z,w) = exp(w·log z)
template <class T>
[[nodiscard]] inline std::complex<T> pow(const std::complex<T>& z, const std::complex<T>& w) noexcept
{
    return exp(w * log(z));
}
template <class T>
[[nodiscard]] inline std::complex<T> pow(const std::complex<T>& z, T p) noexcept
{
    return polar(pow(hypot(z.real(), z.imag()), p), p * atan2(z.imag(), z.real())); // |z|^p·(cos+i·sin)(p·arg)
}

// trig / hyperbolic via the real addition formulas
template <class T>
[[nodiscard]] inline std::complex<T> sin(const std::complex<T>& z) noexcept
{
    return {sin(z.real()) * cosh(z.imag()), cos(z.real()) * sinh(z.imag())};
}
template <class T>
[[nodiscard]] inline std::complex<T> cos(const std::complex<T>& z) noexcept
{
    return {cos(z.real()) * cosh(z.imag()), -sin(z.real()) * sinh(z.imag())};
}
template <class T>
[[nodiscard]] inline std::complex<T> sinh(const std::complex<T>& z) noexcept
{
    return {sinh(z.real()) * cos(z.imag()), cosh(z.real()) * sin(z.imag())};
}
template <class T>
[[nodiscard]] inline std::complex<T> cosh(const std::complex<T>& z) noexcept
{
    return {cosh(z.real()) * cos(z.imag()), sinh(z.real()) * sin(z.imag())};
}
template <class T>
[[nodiscard]] inline std::complex<T> tan(const std::complex<T>& z) noexcept
{
    return sin(z) / cos(z);
}
template <class T>
[[nodiscard]] inline std::complex<T> tanh(const std::complex<T>& z) noexcept
{
    return sinh(z) / cosh(z);
}

} // namespace crd::math
