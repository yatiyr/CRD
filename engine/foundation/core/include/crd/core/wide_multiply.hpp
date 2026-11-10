#pragma once

// The exact 128-bit product of two u64, for general code (clock scaling, XXH3's mixing), and the exact quotient of such
// a product (rates and bandwidth-delay products). Kernel units keep their own copies in their level namespace
// (crd/core/kernel_level.hpp), as crd/math/libm.hpp's does.

#include <crd/core/types.hpp>

#if defined(_MSC_VER) && !defined(__SIZEOF_INT128__)
#include <intrin.h>
#endif

namespace crd
{

struct WideProduct
{
    crd::u64 hi = 0U;
    crd::u64 lo = 0U;
};

[[nodiscard]] inline WideProduct multiply_wide(crd::u64 a, crd::u64 b) noexcept
{
#if defined(__SIZEOF_INT128__)
    __extension__ using u128 = unsigned __int128; // __extension__ keeps it under -Werror=pedantic
    const u128 p = static_cast<u128>(a) * b;
    return {static_cast<crd::u64>(p >> 64U), static_cast<crd::u64>(p)};
#elif defined(_MSC_VER) && defined(_M_X64)
    WideProduct p{};
    p.lo = _umul128(a, b, &p.hi);
    return p;
#elif defined(_MSC_VER) && defined(_M_ARM64)
    return {__umulh(a, b), a * b};
#else
    // Four 32 x 32 products; the middle sum carries at most one bit into the high word.
    const crd::u64 a_lo = a & 0xFFFFFFFFU;
    const crd::u64 a_hi = a >> 32U;
    const crd::u64 b_lo = b & 0xFFFFFFFFU;
    const crd::u64 b_hi = b >> 32U;
    const crd::u64 lo_lo = a_lo * b_lo;
    const crd::u64 hi_lo = a_hi * b_lo;
    const crd::u64 lo_hi = a_lo * b_hi;
    const crd::u64 cross = (lo_lo >> 32U) + (hi_lo & 0xFFFFFFFFU) + lo_hi;
    return {(a_hi * b_hi) + (hi_lo >> 32U) + (cross >> 32U), (cross << 32U) | (lo_lo & 0xFFFFFFFFU)};
#endif
}

namespace detail
{
// The quotient of `p` by `d` for p.hi < d, by restoring division, one quotient bit a step: the running remainder stays
// below 2d, and its top bit is carried apart so doubling it never overflows. Where no 128-bit division is native
// (clang-cl links no helper for one, ARM64 MSVC has no _udiv128); defined everywhere so every host tests it.
[[nodiscard]] inline crd::u64 divide_wide_restoring(WideProduct p, crd::u64 d) noexcept
{
    crd::u64 remainder = p.hi;
    crd::u64 quotient = 0U;
    for (int bit = 63; bit >= 0; --bit)
    {
        const bool carry = (remainder >> 63U) != 0U;
        remainder = (remainder << 1U) | ((p.lo >> static_cast<unsigned>(bit)) & 1U);
        quotient <<= 1U;
        if (carry || remainder >= d)
        {
            remainder -= d;
            quotient |= 1U;
        }
    }
    return quotient;
}
} // namespace detail

// floor(a * b / d) of the exact product; ~0 when the quotient does not fit in 64 bits or `d` is 0.
[[nodiscard]] inline crd::u64 multiply_divide(crd::u64 a, crd::u64 b, crd::u64 d) noexcept
{
    const WideProduct p = multiply_wide(a, b);
    if (d == 0U || p.hi >= d)
    {
        return ~crd::u64{0};
    }
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
    __extension__ using u128 = unsigned __int128;
    return static_cast<crd::u64>(((static_cast<u128>(p.hi) << 64U) | p.lo) / d);
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    crd::u64 remainder = 0U;
    return _udiv128(p.hi, p.lo, d, &remainder);
#else
    return detail::divide_wide_restoring(p, d);
#endif
}

} // namespace crd
