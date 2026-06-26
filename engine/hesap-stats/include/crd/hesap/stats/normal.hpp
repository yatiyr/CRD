#pragma once

// normal.hpp — Phase 3.1.6 v7-q (the v12 stats cluster grows): DETERMINISTIC standard-normal sampling over
// the Philox counter stream — the Box-Muller transform with the pair cache (each draw of two uniforms yields
// two normals; the spare is part of the sampler's state, so the consumption sequence — and therefore every
// sample — is a pure function of (seed, stream, draw index)). u ∈ (0, 1] via 1 − next_f64() so log() never
// sees zero. The CMA-ES / global-search consumers inherit bit-identical runs by construction. ADR-0090.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/stats/philox.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::stats
{

// Standard-normal sampler over a PhiloxRng the caller owns. Stateful (the Box-Muller spare) — one sampler
// per consumption sequence; copying mid-stream forks the sequence deterministically.
class NormalSampler
{
public:
    explicit NormalSampler(PhiloxRng& rng) noexcept : m_rng(&rng) {}

    [[nodiscard]] crd::f64 next() noexcept
    {
        if (m_has_spare)
        {
            m_has_spare = false;
            return m_spare;
        }
        const crd::f64 u1 = 1.0 - m_rng->next_f64(); // (0, 1]
        const crd::f64 u2 = m_rng->next_f64();       // [0, 1)
        const crd::f64 r = crd::math::sqrt(-2.0 * crd::math::log(u1));
        const crd::f64 theta = 6.283185307179586476925286766559 * u2; // 2π
        m_spare = r * crd::math::sin(theta);
        m_has_spare = true;
        return r * crd::math::cos(theta);
    }

    void fill(crd::containers::Span<crd::f64> out) noexcept
    {
        for (crd::usize i = 0; i < out.size(); ++i)
        {
            out[i] = next();
        }
    }

private:
    PhiloxRng* m_rng;
    crd::f64 m_spare = 0.0;
    bool m_has_spare = false;
};

} // namespace crd::hesap::stats
