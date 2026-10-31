#pragma once

// The exact 128-bit product of two u64, for general code (clock scaling, XXH3's mixing). Kernel units keep their own
// copies in their level namespace (crd/core/kernel_level.hpp), as crd/math/libm.hpp's does.

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

} // namespace crd
