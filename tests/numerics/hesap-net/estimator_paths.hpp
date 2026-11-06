// The simulated network paths the libwebrtc oracle and board of hesap-net's estimators run on: a bottleneck queue with
// jitter, reordering, aggregation, far-clock jumps, capacity steps and frame-shaped traffic, deterministic per seed.

#pragma once

#include "seeded_words.hpp"

#include <crd/core/types.hpp>

namespace estimator_paths
{

struct Packet
{
    crd::i64 send_us = 0;
    crd::i64 arrival_us = 0;
    crd::i64 system_us = 0;
    crd::u32 bytes = 0U;
};

struct Profile
{
    const char* name;
    crd::u32 spacing_us;       // Mean send spacing.
    crd::u32 bytes;            // Packet size.
    crd::u32 bytes_per_ms;     // Bottleneck capacity.
    crd::u32 jitter_us;        // Added to each arrival, uniform.
    crd::u32 reorder_per_1024; // Packets delivered out of order.
    crd::u32 burst_per_1024;   // Packets sent back to back with the one before.
    crd::u32 aggregate_us;     // Arrivals released at this tick (Wi-Fi aggregation); 0 for none.
    crd::u32 frame_period;     // Every this many packets, a key frame's worth; 0 for none.
    crd::u32 jump_period;      // Every this many packets the far clock jumps 4 s; 0 for none.
    crd::u32 step_period;      // Every this many packets capacity halves or doubles; 0 for none.
};

inline constexpr Profile kProfiles[] = {
    // name, spacing, bytes, capacity, jitter, reorder, burst, aggregate, frame, jump, step
    {"steady", 2000U, 1200U, 2000U, 200U, 0U, 0U, 0U, 0U, 0U, 0U},
    {"overuse", 1000U, 1400U, 1000U, 300U, 0U, 20U, 0U, 0U, 0U, 0U},
    {"capacity-steps", 2000U, 1200U, 1000U, 300U, 0U, 40U, 0U, 0U, 0U, 3000U},
    {"wifi-aggregation", 2000U, 1200U, 3000U, 500U, 0U, 100U, 8000U, 0U, 0U, 0U},
    {"heavy-jitter", 5000U, 800U, 5000U, 30000U, 0U, 0U, 0U, 0U, 0U, 0U},
    {"reordering", 3000U, 1000U, 2000U, 2000U, 150U, 0U, 0U, 0U, 0U, 0U},
    {"clock-jumps", 3000U, 1000U, 2000U, 500U, 0U, 0U, 0U, 0U, 700U, 0U},
    {"key-frames", 8000U, 600U, 800U, 500U, 0U, 300U, 0U, 300U, 0U, 0U},
    {"tiny-packets", 500U, 60U, 10000U, 100U, 10U, 200U, 0U, 0U, 0U, 0U},
};

class Path
{
public:
    Path(const Profile& profile, crd::u64 seed) noexcept
        : m_profile(profile), m_words(seed, 0x57525443U), m_capacity(profile.bytes_per_ms)
    {
    }

    [[nodiscard]] Packet next() noexcept
    {
        ++m_index;
        if (m_profile.step_period != 0U && m_index % m_profile.step_period == 0U)
        {
            m_capacity = (m_capacity == m_profile.bytes_per_ms) ? m_capacity / 2U : m_profile.bytes_per_ms;
        }
        if (m_profile.jump_period != 0U && m_index % m_profile.jump_period == 0U)
        {
            m_far_offset_us += 4'000'000;
        }
        Packet packet;
        const bool burst = (m_words.next_u32() % 1024U) < m_profile.burst_per_1024;
        m_send_us += burst ? 0 : crd::i64{1} + crd::i64{m_words.next_u32() % (2U * m_profile.spacing_us)};
        packet.send_us = m_send_us;
        packet.bytes = m_profile.bytes / 2U + (m_words.next_u32() % m_profile.bytes);
        if (m_profile.frame_period != 0U && m_index % m_profile.frame_period < 8U)
        {
            packet.bytes = 1200U; // The packets of a key frame go out at full size.
        }
        const crd::i64 service_us = (crd::i64{packet.bytes} * 1000) / crd::i64{m_capacity};
        const crd::i64 start_us = (m_send_us + 20'000 > m_queue_free_us) ? m_send_us + 20'000 : m_queue_free_us;
        m_queue_free_us = start_us + service_us;
        crd::i64 local_arrival_us = m_queue_free_us + crd::i64{m_words.next_u32() % (m_profile.jitter_us + 1U)};
        if (m_profile.aggregate_us != 0U)
        {
            const crd::i64 tick = m_profile.aggregate_us;
            local_arrival_us = ((local_arrival_us / tick) + 1) * tick;
        }
        if ((m_words.next_u32() % 1024U) < m_profile.reorder_per_1024)
        {
            // Hold this packet for later and hand out the one held before, at their own arrival times.
            const Packet held = m_held;
            m_held = packet;
            m_held.arrival_us = local_arrival_us + m_far_offset_us;
            m_held.system_us = local_arrival_us + 500;
            if (held.bytes != 0U)
            {
                return held;
            }
            return next();
        }
        packet.arrival_us = local_arrival_us + m_far_offset_us;
        packet.system_us = local_arrival_us + 500;
        return packet;
    }

private:
    Profile m_profile;
    SeededWords m_words;
    Packet m_held;
    crd::i64 m_send_us = 1'000'000;
    crd::i64 m_queue_free_us = 0;
    crd::i64 m_far_offset_us = 0;
    crd::u32 m_capacity;
    crd::u32 m_index = 0U;
};

} // namespace estimator_paths
