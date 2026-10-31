#pragma once

// crd-hesap-stats v12-e — SplitMix64 (Vigna): the fast mixing PRNG used to seed the other engines (xoshiro/sfc), the
// bit-generator form of crd-core's step (crd/core/seed_generators.hpp). KAT-anchored: SplitMix64(0) →
// 0xE220A8397B1DCDAF, 0x6E789E6AA1B965F4, 0x06C45D188009454F, … (the documented seed-0 stream).

#include <crd/core/seed_generators.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::stats
{
class SplitMix64
{
public:
    explicit SplitMix64(crd::u64 seed) noexcept : m_state(seed) {}

    [[nodiscard]] crd::u64 next_u64() noexcept { return crd::splitmix64_next(m_state); }

private:
    crd::u64 m_state;
};

} // namespace crd::hesap::stats
