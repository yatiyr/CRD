// Integer roots and logarithms the network estimators share (HESAP-NET, capability numerics.network-estimators, owner
// crd-hesap-net): exact on every platform, so NET-SIM's replays compute the same controls. Header-only.

#pragma once

#include <crd/core/types.hpp>
#include <crd/core/wide_multiply.hpp>

#include <bit>

namespace crd::hesap::net
{

// floor(sqrt(x)), digit by digit.
[[nodiscard]] constexpr crd::u64 square_root(crd::u64 x) noexcept
{
    crd::u64 root = 0U;
    crd::u64 bit = crd::u64{1} << 62U;
    while (bit > x)
    {
        bit >>= 2U;
    }
    while (bit != 0U)
    {
        if (x >= root + bit)
        {
            x -= root + bit;
            root = (root >> 1U) + bit;
        }
        else
        {
            root >>= 1U;
        }
        bit >>= 2U;
    }
    return root;
}

// log2(x) in 2^-16 units, rounded down; 0 for x of 0 or 1. Each squaring of the mantissa yields a fraction bit.
[[nodiscard]] inline crd::i64 log2_q16(crd::u64 x) noexcept
{
    if (x <= 1U)
    {
        return 0;
    }
    const auto integer = static_cast<crd::u32>(63 - std::countl_zero(x));
    crd::u64 mantissa = (integer <= 62U) ? (x << (62U - integer)) : (x >> 1U); // [1, 2) with 62 fraction bits
    crd::i64 result = static_cast<crd::i64>(integer) << 16U;
    for (crd::u32 bit = 16U; bit-- > 0U;)
    {
        const WideProduct square = multiply_wide(mantissa, mantissa);
        mantissa = (square.hi << 2U) | (square.lo >> 62U);
        if (mantissa >= (crd::u64{1} << 63U))
        {
            mantissa >>= 1U;
            result |= crd::i64{1} << bit;
        }
    }
    return result;
}

} // namespace crd::hesap::net
