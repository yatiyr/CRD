// GF(2^8) of RFC 5510 §8.1 (HESAP-NET; capability numerics.erasure-codes): bytes as binary polynomials of degree
// below 8, the leftmost bit the highest power, added by XOR and multiplied modulo the primitive polynomial
// x^8 + x^4 + x^3 + x^2 + 1 (0x11D), whose root alpha is the byte 2. Every function is constexpr over the
// exponent and logarithm tables built at compile time.

#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/net/erasure_kernels.hpp>

namespace crd::hesap::net
{

inline constexpr crd::u32 kGf256Polynomial = 0x11DU;
inline constexpr crd::u32 kGf256Order = 255U; // the multiplicative group's order: alpha^255 = 1

struct Gf256Tables
{
    crd::u8 exp[2U * kGf256Order]; // alpha^e for e < 510, so a product's two logarithms index it without a reduction
    crd::u8 log[256];              // log[0] is unused
};

[[nodiscard]] constexpr Gf256Tables make_gf256_tables() noexcept
{
    Gf256Tables t{};
    crd::u32 x = 1U;
    for (crd::u32 e = 0U; e < kGf256Order; ++e)
    {
        t.exp[e] = static_cast<crd::u8>(x);
        t.exp[e + kGf256Order] = static_cast<crd::u8>(x);
        t.log[x] = static_cast<crd::u8>(e);
        x <<= 1U;
        if ((x & 0x100U) != 0U)
        {
            x ^= kGf256Polynomial;
        }
    }
    return t;
}

inline constexpr Gf256Tables kGf256 = make_gf256_tables();

[[nodiscard]] constexpr crd::u8 gf256_mul(crd::u8 a, crd::u8 b) noexcept
{
    if (a == 0U || b == 0U)
    {
        return 0U;
    }
    return kGf256.exp[static_cast<crd::u32>(kGf256.log[a]) + kGf256.log[b]];
}

// The inverse of a nonzero element.
[[nodiscard]] constexpr crd::u8 gf256_inv(crd::u8 a) noexcept
{
    CRD_ASSERT(a != 0U);
    return kGf256.exp[kGf256Order - kGf256.log[a]];
}

// a / b for a nonzero b.
[[nodiscard]] constexpr crd::u8 gf256_div(crd::u8 a, crd::u8 b) noexcept
{
    CRD_ASSERT(b != 0U);
    if (a == 0U)
    {
        return 0U;
    }
    return kGf256.exp[static_cast<crd::u32>(kGf256.log[a]) + kGf256Order - kGf256.log[b]];
}

// alpha^e.
[[nodiscard]] constexpr crd::u8 gf256_alpha_pow(crd::u64 e) noexcept
{
    return kGf256.exp[e % kGf256Order];
}

// c expanded for the dot kernel (crd/hesap/net/erasure_kernels.hpp): the products with every low and high nibble and
// the bit matrix of the product.
[[nodiscard]] constexpr GfCoefficient gf256_expand(crd::u8 c) noexcept
{
    GfCoefficient e{};
    for (crd::u32 x = 0U; x < 16U; ++x)
    {
        e.lo[x] = gf256_mul(c, static_cast<crd::u8>(x));
        e.hi[x] = gf256_mul(c, static_cast<crd::u8>(x << 4U));
    }
    for (crd::u32 b = 0U; b < 8U; ++b)
    {
        const crd::u32 product = gf256_mul(c, static_cast<crd::u8>(1U << b));
        for (crd::u32 i = 0U; i < 8U; ++i)
        {
            e.affine |= crd::u64{(product >> i) & 1U} << (((7U - i) * 8U) + b);
        }
    }
    return e;
}

} // namespace crd::hesap::net
