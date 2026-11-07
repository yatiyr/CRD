// A simulated request-response path between two free-running clocks, for HESAP-NET's clock filters: per-direction
// delays with exponential jitter and queue spikes, a fixed asymmetry, loss, and a peer clock with a frequency, wander,
// steps, a leap smear, a frequency jump and a route change. Every exchange carries the true offset at T4.

#pragma once

#include "seeded_words.hpp"

#include <crd/core/types.hpp>

#include <cmath>

namespace clock_paths
{

struct Profile
{
    const char* name;
    crd::i64 poll_us;
    crd::u32 one_way_us;
    crd::u32 jitter_us;
    crd::u32 asymmetry_us; // Added to the forward direction only: no exchange can see it.
    double frequency_ppm;
    double wander_ppb_per_poll; // A random walk of the frequency.
    crd::u32 loss_per_1024;
    crd::u32 spike_per_1024;
    crd::u32 spike_us;
    crd::u32 event_poll; // When the event below happens; 0 for none.
    crd::i64 step_us;
    double frequency_jump_ppm;
    crd::u32 smear_polls; // A leap smear: smear_ppm added for this many polls from event_poll.
    double smear_ppm;
    crd::u32 route_change_us; // Added to both directions from event_poll on.
};

inline constexpr Profile kProfiles[] = {
    {"lan", 1'000'000, 100U, 40U, 0U, 30.0, 0.0, 0U, 0U, 0U, 0U, 0, 0.0, 0U, 0.0, 0U},
    {"wan", 1'000'000, 20'000U, 2'500U, 0U, -80.0, 0.0, 0U, 10U, 40'000U, 0U, 0, 0.0, 0U, 0.0, 0U},
    {"wifi-bursts", 200'000, 3'000U, 6'000U, 0U, 12.0, 0.0, 10U, 200U, 90'000U, 0U, 0, 0.0, 0U, 0.0, 0U},
    {"asymmetric", 1'000'000, 10'000U, 800U, 30'000U, 55.0, 0.0, 0U, 0U, 0U, 0U, 0, 0.0, 0U, 0.0, 0U},
    {"lossy", 500'000, 15'000U, 3'000U, 0U, -20.0, 0.0, 300U, 40U, 60'000U, 0U, 0, 0.0, 0U, 0.0, 0U},
    {"wander", 1'000'000, 2'000U, 300U, 0U, 10.0, 20.0, 0U, 0U, 0U, 0U, 0, 0.0, 0U, 0.0, 0U},
    {"step-forward", 1'000'000, 5'000U, 500U, 0U, 25.0, 0.0, 0U, 0U, 0U, 1500U, 250'000, 0.0, 0U, 0.0, 0U},
    {"step-back", 1'000'000, 5'000U, 500U, 0U, -25.0, 0.0, 0U, 0U, 0U, 1500U, -2'000'000, 0.0, 0U, 0.0, 0U},
    {"leap-smear", 1'000'000, 5'000U, 500U, 0U, 5.0, 0.0, 0U, 0U, 0U, 1500U, 0, 0.0, 1000U, 11.574, 0U},
    {"frequency-jump", 1'000'000, 5'000U, 500U, 0U, 40.0, 0.0, 0U, 0U, 0U, 1500U, 0, -340.0, 0U, 0.0, 0U},
    {"route-change", 1'000'000, 8'000U, 600U, 0U, 15.0, 0.0, 0U, 0U, 0U, 1500U, 0, 0.0, 0U, 0.0, 25'000U},
};

struct Exchange
{
    crd::i64 t1 = 0;
    crd::i64 t2 = 0;
    crd::i64 t3 = 0;
    crd::i64 t4 = 0;
    crd::i64 true_offset_ns = 0; // At T4.
    double true_frequency = 0.0;
    bool lost = false;
};

class Path
{
public:
    Path(const Profile& profile, crd::u64 seed, crd::i64 start_offset_ns = 0) noexcept
        : m_profile(profile), m_words(seed, 0x4E545044U), m_offset_ns(static_cast<double>(start_offset_ns)),
          m_frequency(profile.frequency_ppm * 1e-6)
    {
    }

    [[nodiscard]] Exchange next() noexcept
    {
        ++m_poll;
        if (m_poll == m_profile.event_poll)
        {
            m_offset_ns += static_cast<double>(m_profile.step_us) * 1e3;
            m_frequency += m_profile.frequency_jump_ppm * 1e-6;
            m_route_us = m_profile.route_change_us;
            m_frequency += m_profile.smear_ppm * 1e-6;
        }
        if (m_profile.smear_polls != 0U && m_poll == m_profile.event_poll + m_profile.smear_polls)
        {
            m_frequency -= m_profile.smear_ppm * 1e-6;
        }
        if (m_profile.wander_ppb_per_poll != 0.0)
        {
            const double unit = (static_cast<double>(m_words.next_u32()) / 2147483648.0) - 1.0;
            m_frequency += unit * m_profile.wander_ppb_per_poll * 1e-9;
        }

        Exchange exchange;
        const double forward = one_way() + static_cast<double>(m_profile.asymmetry_us) * 1e3;
        const double back = one_way();
        const double hold = 10'000.0 + static_cast<double>(m_words.next_u32() % 50'000U);
        exchange.lost = (m_words.next_u32() % 1024U) < m_profile.loss_per_1024;
        exchange.t1 = m_now_ns;
        exchange.t2 = remote(exchange.t1 + static_cast<crd::i64>(forward));
        exchange.t3 = remote(exchange.t1 + static_cast<crd::i64>(forward + hold));
        exchange.t4 = exchange.t1 + static_cast<crd::i64>(forward + hold + back);
        exchange.true_offset_ns = remote(exchange.t4) - exchange.t4;
        exchange.true_frequency = m_frequency;

        const double poll_ns = static_cast<double>(m_profile.poll_us) * 1e3;
        m_offset_ns += m_frequency * poll_ns;
        m_now_ns += static_cast<crd::i64>(poll_ns);
        return exchange;
    }

    [[nodiscard]] crd::i64 poll_ns() const noexcept { return m_profile.poll_us * 1000; }

    [[nodiscard]] crd::u32 poll() const noexcept { return m_poll; }

private:
    [[nodiscard]] double one_way() noexcept
    {
        // Exponential jitter from one uniform word, and a queue spike now and then.
        const double uniform = (static_cast<double>(m_words.next_u32()) + 1.0) / 4294967297.0;
        double us = static_cast<double>(m_profile.one_way_us + m_route_us) -
                    (static_cast<double>(m_profile.jitter_us) * std::log(uniform));
        if ((m_words.next_u32() % 1024U) < m_profile.spike_per_1024)
        {
            us += static_cast<double>(m_words.next_u32() % (m_profile.spike_us + 1U));
        }
        return us * 1e3;
    }

    [[nodiscard]] crd::i64 remote(crd::i64 local_ns) const noexcept
    {
        const double elapsed = static_cast<double>(local_ns - m_now_ns);
        return local_ns + static_cast<crd::i64>(std::llround(m_offset_ns + (m_frequency * elapsed)));
    }

    Profile m_profile;
    SeededWords m_words;
    double m_offset_ns;
    double m_frequency;
    crd::i64 m_now_ns = 1'000'000'000'000;
    crd::u32 m_poll = 0U;
    crd::u32 m_route_us = 0U;
};

} // namespace clock_paths
