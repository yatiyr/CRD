#pragma once

// crd-hesap-stats v12-e — SplitMix64 (Vigna): the fast mixing PRNG used to seed the other engines (xoshiro/sfc).
// state += golden-ratio; finalize with the two MurmurHash3-style multiply-xorshift stages. KAT-anchored:
// SplitMix64(0) → 0xE220A8397B1DCDAF, 0x6E789E6AA1B965F4, 0x06C45D188009454F, … (the documented seed-0 stream).

#include <crd/core/types.hpp>

namespace crd::hesap::stats
{
class SplitMix64
{
public:
    explicit SplitMix64(crd::u64 seed) noexcept : m_state(seed) {}

    [[nodiscard]] crd::u64 next_u64() noexcept
    {
        crd::u64 z = (m_state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

private:
    crd::u64 m_state;
};

} // namespace crd::hesap::stats
