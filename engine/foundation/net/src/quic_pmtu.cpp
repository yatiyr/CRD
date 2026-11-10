// crd-net -- QUIC's datagram packetization layer PMTU discovery (NET-PATH; docs/design/network-foundation.md#pmtu).
#include <crd/net/quic_pmtu.hpp>

namespace crd::net
{
namespace
{
[[nodiscard]] constexpr u32 clamp_floor(u32 bytes) noexcept
{
    return (bytes < kQuicPmtuFloor) ? kQuicPmtuFloor : bytes;
}

[[nodiscard]] constexpr u32 min_u32(u32 a, u32 b) noexcept
{
    return (a < b) ? a : b;
}

[[nodiscard]] constexpr u64 add_saturating(u64 a, u64 b) noexcept
{
    return (a > kNeverNs - b) ? kNeverNs : a + b;
}
} // namespace

void QuicPathMtu::start(u32 base, u32 ceiling, bool discovery) noexcept
{
    *this = QuicPathMtu{};
    m_ceiling = clamp_floor(ceiling);
    m_base = min_u32(clamp_floor(base), m_ceiling);
    m_high = m_ceiling;
    if (!discovery || m_ceiling == kQuicPmtuFloor)
    {
        m_size = m_base;
        m_low = m_base;
        return;
    }
    m_state = (m_base > kQuicPmtuFloor) ? QuicPmtuState::Base : QuicPmtuState::Searching;
    m_optimistic = true;
}

void QuicPathMtu::limit(u32 ceiling) noexcept
{
    const u32 bound = clamp_floor(ceiling);
    if (bound >= m_ceiling)
    {
        return;
    }
    m_ceiling = bound;
    m_base = min_u32(m_base, bound);
    m_size = min_u32(m_size, bound);
    m_low = min_u32(m_low, bound);
    m_high = min_u32(m_high, bound);
    if (m_state == QuicPmtuState::Disabled)
    {
        return;
    }
    if (bound == kQuicPmtuFloor)
    {
        m_state = QuicPmtuState::Disabled; // nothing above the floor is left to find
        return;
    }
    if (m_state == QuicPmtuState::Base && m_low >= m_base)
    {
        m_state = QuicPmtuState::Searching; // the base fell to a size already proved (the floor, at least)
    }
    if (m_trying > bound)
    {
        m_trying = 0U; // a probe in flight above the ceiling settles as a stale one
        m_failures = 0U;
    }
    if (m_state == QuicPmtuState::Searching && m_low >= m_high)
    {
        m_state = QuicPmtuState::SearchComplete;
    }
}

u32 QuicPathMtu::probe(u64 now_ns, bool busy) noexcept
{
    const bool waiting =
        m_state == QuicPmtuState::Error || (m_state == QuicPmtuState::SearchComplete && m_size < m_ceiling);
    if (waiting && busy && now_ns >= m_raise_ns)
    {
        // Section 5.2: the raise timer probes again, from the ceiling.
        m_state = (m_state == QuicPmtuState::Error) ? QuicPmtuState::Base : QuicPmtuState::Searching;
        m_high = m_ceiling;
        m_optimistic = true;
        m_failures = 0U;
    }
    if (m_flight != 0U)
    {
        return 0U;
    }
    if (m_state == QuicPmtuState::Base)
    {
        m_trying = m_base;
        return m_trying;
    }
    if (m_state != QuicPmtuState::Searching)
    {
        return 0U;
    }
    if (m_failures == 0U || m_trying <= m_low || m_trying > m_high)
    {
        // A size an acknowledgement or a ceiling settled meanwhile is probed no more.
        m_failures = 0U;
        // Upper middle: always above the largest acknowledged, so every probe narrows the gap.
        m_trying = m_optimistic ? m_high : m_low + ((m_high - m_low + 1U) / 2U);
    }
    return m_trying;
}

void QuicPathMtu::on_probe_sent(u32 bytes) noexcept
{
    m_trying = bytes;
    m_flight = bytes;
    ++m_counters.probes_sent;
}

void QuicPathMtu::on_probe_acked(u32 bytes, u64 now_ns) noexcept
{
    ++m_counters.probes_acked;
    m_flight = (bytes == m_flight) ? 0U : m_flight;
    m_failures = (bytes == m_trying) ? 0U : m_failures;
    if (m_state == QuicPmtuState::Disabled || bytes > m_ceiling)
    {
        return; // a probe sent before the ceiling fell proves nothing the connection may use
    }
    if (bytes > m_low)
    {
        m_low = bytes;
        m_size = bytes;
    }
    if (bytes >= m_high)
    {
        m_high = bytes;
        m_optimistic = false;
    }
    if (m_state == QuicPmtuState::Base || m_state == QuicPmtuState::Error)
    {
        if (m_low < m_base)
        {
            return; // a stale probe below the base
        }
        m_state = QuicPmtuState::Searching; // up to the bound the start, the raise or a black hole set
    }
    if (m_state == QuicPmtuState::Searching)
    {
        search(now_ns);
    }
}

void QuicPathMtu::on_probe_lost(u32 bytes, u64 now_ns) noexcept
{
    ++m_counters.probes_lost;
    m_flight = (bytes == m_flight) ? 0U : m_flight;
    if (bytes != m_trying)
    {
        return; // a stale probe: its size was settled already
    }
    if (++m_failures < kQuicPmtuMaxProbes)
    {
        return; // probed again
    }
    m_failures = 0U;
    m_optimistic = false;
    if (m_state == QuicPmtuState::Base)
    {
        m_state = QuicPmtuState::Error;
        m_raise_ns = add_saturating(now_ns, kQuicPmtuRaiseNs);
        return;
    }
    if (m_state != QuicPmtuState::Searching)
    {
        return;
    }
    m_high = (bytes > m_low) ? bytes - 1U : m_low;
    search(now_ns);
}

void QuicPathMtu::on_acked(u64 number, u32 bytes) noexcept
{
    if (bytes <= kQuicPmtuFloor || number + 1U <= m_large_acked)
    {
        return;
    }
    m_large_acked = number + 1U;
    // A larger packet acknowledged after a suspect detection's losses: they were congestion, not the path.
    u32 kept = 0U;
    for (u32 index = 0U; index < m_suspect_count; ++index)
    {
        if (m_suspects[index] >= m_large_acked)
        {
            m_suspects[kept++] = m_suspects[index];
        }
    }
    m_suspect_count = kept;
}

void QuicPathMtu::on_lost(u64 number, u32 bytes) noexcept
{
    if (bytes <= kQuicPmtuFloor)
    {
        m_burst_small = true;
        return;
    }
    m_burst_oldest = (number < m_burst_oldest) ? number : m_burst_oldest;
}

bool QuicPathMtu::end_detection(u64 now_ns) noexcept
{
    const u64 oldest = m_burst_oldest;
    const bool small = m_burst_small;
    m_burst_oldest = kNeverNs;
    m_burst_small = false;
    const bool raised = m_state == QuicPmtuState::Searching || m_state == QuicPmtuState::SearchComplete;
    if (oldest == kNeverNs || small || !raised || m_size == kQuicPmtuFloor || oldest < m_large_acked)
    {
        return false;
    }
    m_suspects[m_suspect_count++] = oldest;
    if (m_suspect_count < kQuicPmtuBlackHoleDetections)
    {
        return false;
    }
    // RFC 8899 section 4.3: back to the floor, then the base and a search below the size that stopped.
    ++m_counters.black_holes;
    const u32 failed = m_size;
    m_suspect_count = 0U;
    m_size = kQuicPmtuFloor;
    m_low = kQuicPmtuFloor;
    m_high = failed - 1U;
    m_trying = 0U; // a probe in flight settles as a stale one
    m_failures = 0U;
    m_optimistic = false;
    m_state = (m_base > kQuicPmtuFloor) ? QuicPmtuState::Base : QuicPmtuState::Searching;
    if (m_state == QuicPmtuState::Searching)
    {
        search(now_ns);
    }
    return true;
}

bool QuicPathMtu::on_too_big(u32 quoted, u32 limit, u64 now_ns) noexcept
{
    // RFC 9000 section 14.2.1 ignores a limit under QUIC's floor; RFC 8899 section 4.6.2 one under the base, and one
    // at or above the size of the datagram it quotes.
    if (m_state == QuicPmtuState::Disabled || limit < m_base || limit >= quoted)
    {
        ++m_counters.hints_ignored;
        return false;
    }
    ++m_counters.hints;
    m_flight = (m_flight > limit) ? 0U : m_flight; // gone: the next probe need not wait for its loss
    if (m_trying > limit)
    {
        m_trying = 0U;
        m_failures = 0U;
    }
    if (limit < m_size)
    {
        // The path shrank under the size in use: back to the floor, then the base and a search up to the limit.
        m_size = kQuicPmtuFloor;
        m_low = kQuicPmtuFloor;
        m_suspect_count = 0U;
        m_state = (m_base > kQuicPmtuFloor) ? QuicPmtuState::Base : QuicPmtuState::Searching;
    }
    if (limit < m_high)
    {
        m_high = limit;
        m_optimistic = true; // the next probe tries the limit itself
    }
    if (m_state == QuicPmtuState::Searching)
    {
        search(now_ns);
    }
    return true;
}

void QuicPathMtu::search(u64 now_ns) noexcept
{
    m_state = QuicPmtuState::Searching;
    if (m_low >= m_high)
    {
        complete(now_ns);
    }
}

void QuicPathMtu::complete(u64 now_ns) noexcept
{
    m_state = QuicPmtuState::SearchComplete;
    m_high = m_low;
    m_raise_ns = add_saturating(now_ns, kQuicPmtuRaiseNs);
}

} // namespace crd::net
