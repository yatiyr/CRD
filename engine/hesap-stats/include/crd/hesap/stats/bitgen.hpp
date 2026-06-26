#pragma once

// crd-hesap-stats v12-e — the BitGenerator concept + uniform conversions, the common surface over every RNG engine
// (SplitMix64 · Xoshiro256**/++ · SFC64 · PCG64-DXSM · Threefry4x64 · Philox4x32 · MT19937). A BitGenerator is any
// type exposing `crd::u64 next_u64()`. Conversions follow the NumPy/standard conventions (top 53 bits → double).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <concepts>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace crd::hesap::stats
{
#if defined(__SIZEOF_INT128__)
__extension__ using crd_u128_t = unsigned __int128; // __extension__ keeps it under -Werror=pedantic
#endif

// Portable 64×64 → 128: returns the low 64 bits, writes the high 64 to `hi`. (MSVC has no __int128.)
[[nodiscard]] inline crd::u64 mul128(crd::u64 a, crd::u64 b, crd::u64& hi) noexcept
{
#if defined(__SIZEOF_INT128__)
    const crd_u128_t p = static_cast<crd_u128_t>(a) * static_cast<crd_u128_t>(b);
    hi = static_cast<crd::u64>(p >> 64);
    return static_cast<crd::u64>(p);
#elif defined(_MSC_VER)
    crd::u64 lo;
    lo = _umul128(a, b, &hi);
    return lo;
#else
    const crd::u64 al = a & 0xFFFFFFFFULL, ah = a >> 32, bl = b & 0xFFFFFFFFULL, bh = b >> 32;
    const crd::u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const crd::u64 mid = (ll >> 32) + (lh & 0xFFFFFFFFULL) + (hl & 0xFFFFFFFFULL);
    hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return (mid << 32) | (ll & 0xFFFFFFFFULL);
#endif
}

template <typename G>
concept BitGenerator = requires(G g) {
    { g.next_u64() } -> std::same_as<crd::u64>;
};

// Uniform double in [0,1) from the top 53 bits (the canonical NumPy/xoshiro recipe; exactly representable).
template <BitGenerator G>
[[nodiscard]] inline double next_double(G& g) noexcept
{
    return static_cast<double>(g.next_u64() >> 11) * (1.0 / 9007199254740992.0); // / 2^53
}

// Uniform u32 from the high 32 bits (the better-mixed half for most engines).
template <BitGenerator G>
[[nodiscard]] inline crd::u32 next_u32(G& g) noexcept
{
    return static_cast<crd::u32>(g.next_u64() >> 32);
}

// Lemire's unbiased bounded integer [0, bound): one multiply, rejection only on the rare biased low window.
template <BitGenerator G>
[[nodiscard]] inline crd::u64 bounded(G& g, crd::u64 bound) noexcept
{
    if (bound == 0U)
    {
        return g.next_u64();
    }
    crd::u64 hi = 0;
    crd::u64 low = mul128(g.next_u64(), bound, hi);
    if (low < bound)
    {
        const crd::u64 thresh = (0U - bound) % bound;
        while (low < thresh)
        {
            low = mul128(g.next_u64(), bound, hi);
        }
    }
    return hi;
}

template <BitGenerator G>
inline void fill_u64(G& g, crd::containers::Span<crd::u64> out) noexcept
{
    for (crd::u64& x : out)
    {
        x = g.next_u64();
    }
}

// 64-bit bit-rotate left — shared by the engines.
[[nodiscard]] inline constexpr crd::u64 rotl64(crd::u64 x, int k) noexcept
{
    return (x << k) | (x >> (64 - k));
}

} // namespace crd::hesap::stats
