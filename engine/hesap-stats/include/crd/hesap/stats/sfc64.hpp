#pragma once

// crd-hesap-stats v12-e — SFC64 (Chris Doty-Humphrey, PractRand): small-fast-counting, 256-bit state (a,b,c + a
// 64-bit counter), no multiply, very fast, passes PractRand to 32 TB. The NumPy SFC64 BitGenerator. Gated vs NumPy
// random_raw via a known internal state (the algorithm is bit-exact to NumPy's sfc64_next).

#include <crd/core/types.hpp>

namespace crd::hesap::stats
{
class Sfc64
{
public:
    // Reference seeding (Doty-Humphrey): a=b=c=seed, counter=1, then 18 warm-up rounds.
    explicit Sfc64(crd::u64 seed) noexcept : m_a(seed), m_b(seed), m_c(seed), m_w(1U)
    {
        for (int i = 0; i < 18; ++i)
        {
            (void)next_u64();
        }
    }

    // Direct-state construction (matches NumPy's {a,b,c,counter} state — for bit-exact cross-checks).
    [[nodiscard]] static Sfc64 from_state(crd::u64 a, crd::u64 b, crd::u64 c, crd::u64 counter) noexcept
    {
        Sfc64 g;
        g.m_a = a;
        g.m_b = b;
        g.m_c = c;
        g.m_w = counter;
        return g;
    }

    [[nodiscard]] crd::u64 next_u64() noexcept
    {
        const crd::u64 tmp = m_a + m_b + m_w++;
        m_a = m_b ^ (m_b >> 11);
        m_b = m_c + (m_c << 3);
        m_c = ((m_c << 24) | (m_c >> 40)) + tmp; // rotl(c,24) + tmp
        return tmp;
    }

private:
    Sfc64() noexcept = default;
    crd::u64 m_a = 0;
    crd::u64 m_b = 0;
    crd::u64 m_c = 0;
    crd::u64 m_w = 0;
};

} // namespace crd::hesap::stats
