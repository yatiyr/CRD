// Bit equality with ADR-0134 §7's carve-out, "NaN payloads aside": a NaN agrees with any NaN. IEEE 754 leaves the
// payload and sign of an operation on two NaNs open, and the operand order a compiler picks for a commutative add or a
// negated product decides them, so two code paths over the same values (a level's kernel and portable's, a parallel
// body and the serial one) may give different NaN bits. Every other value, -0 and the subnormals included, matches
// bit for bit. Complex values ({re, im}) compare part by part.
//
// A case whose output starts as NaN guard words still checks that every guard was overwritten: here an unwritten
// guard agrees with a NaN result.

#pragma once

#include <crd/core/types.hpp>

#include <bit>
#include <catch2/catch_message.hpp>
#include <cmath>
#include <ios>
#include <type_traits>

namespace crd::test
{

// A complex value: compared by its re and im parts.
template <typename T>
concept NanAsideParts = requires(const T& v) {
    v.re;
    v.im;
};

template <typename R> [[nodiscard]] bool same_bits_nan_aside(R x, R y) noexcept
{
    static_assert(std::is_floating_point_v<R> && (sizeof(R) == 4U || sizeof(R) == 8U));
    using Bits = std::conditional_t<sizeof(R) == 4U, crd::u32, crd::u64>;
    return std::bit_cast<Bits>(x) == std::bit_cast<Bits>(y) || (std::isnan(x) && std::isnan(y));
}

// Whether x[0, n) and y[0, n) agree. On the first disagreement it attaches the index and both bit patterns to the
// next assertion (UNSCOPED_INFO), so a failing CHECK shows the values.
template <typename T> [[nodiscard]] bool same_values_nan_aside(const T* x, const T* y, crd::usize n)
{
    for (crd::usize i = 0; i < n; ++i)
    {
        if constexpr (NanAsideParts<T>)
        {
            const bool re = same_bits_nan_aside(x[i].re, y[i].re);
            if (!re || !same_bits_nan_aside(x[i].im, y[i].im))
            {
                using R = std::remove_cvref_t<decltype(x[i].re)>;
                using Bits = std::conditional_t<sizeof(R) == 4U, crd::u32, crd::u64>;
                const R xp = re ? x[i].im : x[i].re;
                const R yp = re ? y[i].im : y[i].re;
                UNSCOPED_INFO("value " << i << (re ? " im" : " re") << " differs: 0x" << std::hex
                                       << std::bit_cast<Bits>(xp) << " against 0x" << std::bit_cast<Bits>(yp)
                                       << std::dec);
                return false;
            }
        }
        else
        {
            if (!same_bits_nan_aside(x[i], y[i]))
            {
                using Bits = std::conditional_t<sizeof(T) == 4U, crd::u32, crd::u64>;
                UNSCOPED_INFO("value " << i << " differs: 0x" << std::hex << std::bit_cast<Bits>(x[i]) << " against 0x"
                                       << std::bit_cast<Bits>(y[i]) << std::dec);
                return false;
            }
        }
    }
    return true;
}

} // namespace crd::test
