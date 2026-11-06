// Interarrival jitter (HESAP-NET, capability numerics.network-estimators, owner crd-hesap-net): RFC 3550 §6.4.1's
// J = J + (|D| - J) / 16 in the fixed point of its Appendix A.8, over 32-bit media timestamps that wrap.
//
// Both clocks are in the stream's timestamp units, the arrival clock on any origin. A sender controls its timestamps,
// so a step of |D| at or above the bound is skipped rather than folded in, as libwebrtc's receive statistics do: a
// stream restart must not read as seconds of jitter. kUnbounded is the RFC's own behaviour.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::net
{

class InterarrivalJitter
{
public:
    static constexpr crd::u32 kUnbounded = 0x80000001U; // Above every |D| of a signed 32-bit difference.

    constexpr explicit InterarrivalJitter(crd::u32 step_bound = kUnbounded) noexcept : m_bound(step_bound) {}

    // One packet: its arrival time and its media timestamp, in timestamp units.
    constexpr void update(crd::u32 arrival, crd::u32 timestamp) noexcept
    {
        const crd::u32 transit = arrival - timestamp;
        if (m_started)
        {
            const crd::u32 difference = transit - m_transit;
            const crd::u32 step = (difference >= 0x80000000U) ? 0U - difference : difference;
            if (step < m_bound)
            {
                m_q4 += step - ((m_q4 + 8U) >> 4U);
            }
        }
        m_started = true;
        m_transit = transit;
    }

    constexpr void reset() noexcept
    {
        m_started = false;
        m_q4 = 0U;
    }

    // The value a receiver report carries, in timestamp units.
    [[nodiscard]] constexpr crd::u32 value() const noexcept
    {
        return m_q4 >> 4U;
    }

    [[nodiscard]] constexpr crd::u32 value_q4() const noexcept
    {
        return m_q4;
    }

private:
    crd::u32 m_bound;
    crd::u32 m_q4 = 0U;
    crd::u32 m_transit = 0U;
    bool m_started = false;
};

} // namespace crd::hesap::net
