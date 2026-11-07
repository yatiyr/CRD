// crd-net -- the state-hash exchange (NET.3; docs/design/network-foundation.md#state-hashes).

#include <crd/containers/checksum.hpp>
#include <crd/net/state_hash.hpp>

namespace crd::net
{

u64 state_hash_chain(u64 previous, u64 step, u64 hash) noexcept
{
    u8 bytes[16];
    for (u32 index = 0U; index < 8U; ++index)
    {
        bytes[index] = static_cast<u8>(step >> (index * 8U));
        bytes[index + 8U] = static_cast<u8>(hash >> (index * 8U));
    }
    return cont::xxh3_64(cont::ConstSpan<u8>(bytes, sizeof(bytes)), previous);
}

StateHashExchange::StateHashExchange(cont::Span<StateHashReport> local, cont::Span<StateHashReport> ahead) noexcept
    : m_local(local), m_ahead(ahead)
{
}

void StateHashExchange::restart() noexcept
{
    m_local_count = 0U;
    m_local_next = 0U;
    m_ahead_count = 0U;
    m_ahead_first = 0U;
    m_chain = 0U;
    m_any_report = false;
    m_last_report = 0U;
    m_diverged = false;
    m_divergence = StateHashDivergence{};
    m_dropped = 0U;
}

u64 StateHashExchange::newest_local() const noexcept
{
    return m_local[(m_local_next + m_local.size() - 1U) % m_local.size()].step;
}

const StateHashReport* StateHashExchange::find_local(u64 step) const noexcept
{
    // The records rise from the oldest, which m_local_next points at once the ring has wrapped.
    const usize oldest = (m_local_next + m_local.size() - m_local_count) % m_local.size();
    usize low = 0U;
    usize high = m_local_count;
    while (low < high)
    {
        const usize middle = low + ((high - low) / 2U);
        const StateHashReport& entry = m_local[(oldest + middle) % m_local.size()];
        if (entry.step == step)
        {
            return &entry;
        }
        if (entry.step < step)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    return nullptr;
}

StateHashVerdict StateHashExchange::compare(const StateHashReport& report) noexcept
{
    const StateHashReport* local = find_local(report.step);
    if (local == nullptr)
    {
        return StateHashVerdict::NotHeld;
    }
    if (local->hash == report.hash && local->chain == report.chain)
    {
        m_divergence.any_agreed = true;
        m_divergence.agreed_through = report.step;
        return StateHashVerdict::Agreed;
    }
    m_diverged = true;
    m_divergence.step = report.step;
    m_divergence.local_hash = local->hash;
    m_divergence.peer_hash = report.hash;
    m_divergence.at_this_step = local->hash != report.hash;
    return StateHashVerdict::Diverged;
}

StateHashVerdict StateHashExchange::record(u64 step, u64 hash, StateHashReport& report) noexcept
{
    if (m_local_count != 0U && step <= newest_local())
    {
        return StateHashVerdict::Stale;
    }
    m_chain = state_hash_chain(m_chain, step, hash);
    report = StateHashReport{step, hash, m_chain};
    m_local[m_local_next] = report;
    m_local_next = (m_local_next + 1U) % m_local.size();
    m_local_count += (m_local_count < m_local.size()) ? 1U : 0U;

    StateHashVerdict verdict = m_diverged ? StateHashVerdict::AlreadyDiverged : StateHashVerdict::Waiting;
    while (!m_diverged && m_ahead_count != 0U && m_ahead[m_ahead_first].step <= step)
    {
        // Only the last can be held: a report of a step already passed was compared when it arrived.
        verdict = compare(m_ahead[m_ahead_first]);
        m_ahead_first = (m_ahead_first + 1U) % m_ahead.size();
        --m_ahead_count;
    }
    return verdict;
}

StateHashVerdict StateHashExchange::on_report(const StateHashReport& report) noexcept
{
    if (m_diverged)
    {
        return StateHashVerdict::AlreadyDiverged;
    }
    if (m_any_report && report.step <= m_last_report)
    {
        return StateHashVerdict::Stale;
    }
    m_any_report = true;
    m_last_report = report.step;

    if (m_local_count != 0U && report.step <= newest_local())
    {
        return compare(report);
    }
    if (m_ahead_count == m_ahead.size())
    {
        m_ahead_first = (m_ahead_first + 1U) % m_ahead.size();
        --m_ahead_count;
        ++m_dropped;
    }
    m_ahead[(m_ahead_first + m_ahead_count) % m_ahead.size()] = report;
    ++m_ahead_count;
    return StateHashVerdict::Waiting;
}

} // namespace crd::net
