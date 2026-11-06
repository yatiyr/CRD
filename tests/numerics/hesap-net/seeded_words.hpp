// The seeded word source of the hesap-net tests: SplitMix64 from crd-core, so the tests stay in the module's jobs-free
// closure and reach the ARM64 lanes (hesap-stats' engines link crd-jobs through their module).

#pragma once

#include <crd/core/seed_generators.hpp>
#include <crd/core/types.hpp>

class SeededWords
{
public:
    SeededWords(crd::u64 seed, crd::u64 stream) noexcept : m_state(crd::splitmix64_mix(seed) ^ stream) {}

    [[nodiscard]] crd::u32 next_u32() noexcept
    {
        return static_cast<crd::u32>(crd::splitmix64_next(m_state) >> 32U);
    }

private:
    crd::u64 m_state;
};
