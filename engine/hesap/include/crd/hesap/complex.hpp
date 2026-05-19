#pragma once

#include <crd/core/types.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::hesap
{
// -----------------------------------------------------------------------
// Complex<T> — Cerid-native complex value type.
//
// Per ADR-0065 §13 D2 (2026-05-19 elite amendment) — complex from v0,
// not "real-valued first" as the original 2026-05-10 plan had it.
//
// Per the tak-çıkar (plug-out) principle in PRINCIPLES.md, Cerid does
// NOT wrap std::complex. The Cerid type is its own header so:
//   - Division uses Smith 1962 robust algorithm (avoids spurious overflow
//     and underflow when |re| or |im| is near the type's range bounds).
//   - abs uses std::hypot (no spurious overflow on x*x + y*y).
//   - arg uses std::atan2 (handles quadrant correctly).
//   - Every op is scalar in v0a; SIMD bit-exact parity arrives v0b
//     (the algorithms here vectorise without changing summation order).
//
// Trivially copyable; sizeof(Complex<f32>) == 8, sizeof(Complex<f64>) == 16.
// -----------------------------------------------------------------------
template <typename T>
struct Complex
{
    static_assert(std::is_floating_point_v<T>, "Complex<T> requires a floating-point T");

    T re;
    T im;

    // ---- Construction ---------------------------------------------

    constexpr Complex() noexcept : re(T(0)), im(T(0)) {}

    constexpr Complex(T real_part, T imag_part) noexcept : re(real_part), im(imag_part) {}

    explicit constexpr Complex(T real_part) noexcept : re(real_part), im(T(0)) {}

    // ---- Predicates -----------------------------------------------

    [[nodiscard]] constexpr bool is_real() const noexcept { return im == T(0); }

    [[nodiscard]] constexpr bool is_zero() const noexcept { return re == T(0) && im == T(0); }

    [[nodiscard]] constexpr bool operator==(const Complex&) const noexcept = default;

    // ---- Field operations -----------------------------------------

    [[nodiscard]] constexpr Complex operator+(const Complex& b) const noexcept
    {
        return Complex{re + b.re, im + b.im};
    }

    [[nodiscard]] constexpr Complex operator-(const Complex& b) const noexcept
    {
        return Complex{re - b.re, im - b.im};
    }

    [[nodiscard]] constexpr Complex operator-() const noexcept { return Complex{-re, -im}; }

    [[nodiscard]] constexpr Complex operator*(const Complex& b) const noexcept
    {
        return Complex{re * b.re - im * b.im, re * b.im + im * b.re};
    }

    [[nodiscard]] constexpr Complex operator*(T s) const noexcept { return Complex{re * s, im * s}; }

    // Smith 1962 robust division. Avoids overflow when |b.re| or |b.im| is
    // near the range bounds. Branches on which component dominates and
    // factors that out so intermediate products stay in range. Returns
    // {NaN,NaN} on division by zero (no exceptions per ADR-0001 contract).
    [[nodiscard]] Complex operator/(const Complex& b) const noexcept
    {
        const T abs_re = b.re < T(0) ? -b.re : b.re;
        const T abs_im = b.im < T(0) ? -b.im : b.im;

        if (abs_re >= abs_im)
        {
            if (abs_re == T(0))
            {
                const T nan = std::numeric_limits<T>::quiet_NaN();
                return Complex{nan, nan};
            }
            const T r = b.im / b.re;
            const T den = b.re + r * b.im;
            return Complex{(re + im * r) / den, (im - re * r) / den};
        }
        const T r = b.re / b.im;
        const T den = b.im + r * b.re;
        return Complex{(re * r + im) / den, (im * r - re) / den};
    }

    [[nodiscard]] constexpr Complex operator/(T s) const noexcept { return Complex{re / s, im / s}; }

    constexpr Complex& operator+=(const Complex& b) noexcept
    {
        re += b.re;
        im += b.im;
        return *this;
    }

    constexpr Complex& operator-=(const Complex& b) noexcept
    {
        re -= b.re;
        im -= b.im;
        return *this;
    }

    constexpr Complex& operator*=(const Complex& b) noexcept
    {
        *this = *this * b;
        return *this;
    }

    constexpr Complex& operator*=(T s) noexcept
    {
        re *= s;
        im *= s;
        return *this;
    }

    Complex& operator/=(const Complex& b) noexcept
    {
        *this = *this / b;
        return *this;
    }

    constexpr Complex& operator/=(T s) noexcept
    {
        re /= s;
        im /= s;
        return *this;
    }
};

// ---- Symmetric scalar-on-the-left overloads -------------------------

template <typename T>
[[nodiscard]] constexpr Complex<T> operator*(T s, const Complex<T>& z) noexcept
{
    return Complex<T>{s * z.re, s * z.im};
}

// ---- Free functions ------------------------------------------------

// Conjugate. Hermitian-adjoint of a 1x1 matrix.
template <typename T>
[[nodiscard]] constexpr Complex<T> conj(const Complex<T>& z) noexcept
{
    return Complex<T>{z.re, -z.im};
}

// Real / imaginary projection. Free-function form for symmetry with std::complex.
template <typename T>
[[nodiscard]] constexpr T real(const Complex<T>& z) noexcept
{
    return z.re;
}

template <typename T>
[[nodiscard]] constexpr T imag(const Complex<T>& z) noexcept
{
    return z.im;
}

// Squared magnitude. Exact (no sqrt) — useful for ordering tests.
template <typename T>
[[nodiscard]] constexpr T norm_sq(const Complex<T>& z) noexcept
{
    return z.re * z.re + z.im * z.im;
}

// Magnitude. Uses std::hypot — robust against overflow when one component
// is near the type's range bound.
template <typename T>
[[nodiscard]] inline T abs(const Complex<T>& z) noexcept
{
    return std::hypot(z.re, z.im);
}

// Argument (angle). Uses std::atan2 for correct quadrant handling.
template <typename T>
[[nodiscard]] inline T arg(const Complex<T>& z) noexcept
{
    return std::atan2(z.im, z.re);
}

// ---- Type aliases ---------------------------------------------------

using Complex32 = Complex<f32>;
using Complex64 = Complex<f64>;

static_assert(sizeof(Complex32) == 8, "Complex<f32> must pack to 8 bytes");
static_assert(sizeof(Complex64) == 16, "Complex<f64> must pack to 16 bytes");
static_assert(std::is_trivially_copyable_v<Complex32>, "Complex<f32> must be trivially copyable");
static_assert(std::is_trivially_copyable_v<Complex64>, "Complex<f64> must be trivially copyable");

} // namespace crd::hesap
