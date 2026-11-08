// crd-net -- a client's bounded store of TLS session tickets (4.2a; docs/design/network-foundation.md#ticket-cache).

#include <crd/crypto/provider.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/ticket_cache.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
bool config_valid(const TicketCacheConfig& config) noexcept
{
    return config.entries <= kTicketCacheMaxEntries && config.per_binding != 0U &&
           config.per_binding <= config.entries && config.ticket_bytes != 0U &&
           config.ticket_bytes <= kTicketCacheMaxBytes && config.max_age_ns != 0U &&
           config.max_age_ns <= kTicketMaxAgeNs;
}
} // namespace

TicketCache::TicketCache(memory::IAllocator& allocator, const TicketCacheConfig& config) noexcept
    : m_allocator(allocator), m_config(config)
{
    if (config.entries == 0U)
    {
        m_ready = true; // keeps nothing
        return;
    }
    if (!config_valid(config))
    {
        return;
    }
    Entry* const entries = memory::try_construct_array<Entry>(allocator, config.entries);
    u8* const bytes = memory::try_construct_array<u8>(allocator, usize{config.entries} * config.ticket_bytes);
    if (entries == nullptr || bytes == nullptr)
    {
        memory::destroy_array(allocator, entries, config.entries);
        memory::destroy_array(allocator, bytes, usize{config.entries} * config.ticket_bytes);
        return;
    }
    m_entries = entries;
    m_bytes = bytes;
    m_ready = true;
}

TicketCache::~TicketCache()
{
    if (m_entries == nullptr)
    {
        return;
    }
    const usize bytes = usize{m_config.entries} * m_config.ticket_bytes;
    crypto::secure_zero(cont::Span<u8>(m_bytes, bytes));
    memory::destroy_array(m_allocator, m_bytes, bytes);
    memory::destroy_array(m_allocator, m_entries, m_config.entries);
}

u8* TicketCache::storage(u32 index) const noexcept
{
    return m_bytes + (usize{index} * m_config.ticket_bytes);
}

void TicketCache::wipe(u32 index) noexcept
{
    Entry& entry = m_entries[index];
    crypto::secure_zero(cont::Span<u8>(storage(index), entry.bytes));
    entry = Entry{};
}

void TicketCache::scrub() noexcept
{
    if (m_handed != ~u32{0})
    {
        wipe(m_handed);
        m_handed = ~u32{0};
    }
}

void TicketCache::advance(u64 now_ns) noexcept
{
    scrub();
    m_now_ns = (now_ns > m_now_ns) ? now_ns : m_now_ns;
    if (m_size == 0U || m_now_ns - m_oldest_ns < m_config.max_age_ns)
    {
        return;
    }
    u64 oldest = m_now_ns;
    for (u32 index = 0U; index < m_config.entries; ++index)
    {
        const Entry& entry = m_entries[index];
        if (entry.order == 0U)
        {
            continue;
        }
        if (m_now_ns - entry.kept_ns >= m_config.max_age_ns)
        {
            wipe(index);
            --m_size;
            ++m_counters.expired;
            continue;
        }
        oldest = (entry.kept_ns < oldest) ? entry.kept_ns : oldest;
    }
    m_oldest_ns = oldest;
}

void TicketCache::keep(const crypto::TlsBinding& binding, cont::ConstSpan<u8> ticket) noexcept
{
    scrub();
    if (m_entries == nullptr)
    {
        return;
    }
    if (ticket.empty() || ticket.size() > m_config.ticket_bytes)
    {
        ++m_counters.too_large;
        return;
    }
    const u32 none = m_config.entries;
    u32 free_entry = none;
    u32 oldest = none;
    u32 own_oldest = none;
    u32 own = 0U;
    for (u32 index = 0U; index < m_config.entries; ++index)
    {
        const Entry& entry = m_entries[index];
        if (entry.order == 0U)
        {
            free_entry = index;
            continue;
        }
        oldest = (oldest == none || entry.order < m_entries[oldest].order) ? index : oldest;
        if (entry.binding == binding)
        {
            ++own;
            own_oldest = (own_oldest == none || entry.order < m_entries[own_oldest].order) ? index : own_oldest;
        }
    }
    u32 victim = free_entry;
    if (own >= m_config.per_binding)
    {
        victim = own_oldest;
    }
    else if (free_entry == none)
    {
        victim = oldest;
    }
    if (m_entries[victim].order != 0U)
    {
        wipe(victim);
        --m_size;
        ++m_counters.displaced;
    }
    if (m_size == 0U)
    {
        m_oldest_ns = m_now_ns;
    }
    Entry& entry = m_entries[victim];
    entry.binding = binding;
    entry.order = ++m_order;
    entry.kept_ns = m_now_ns;
    entry.bytes = ticket.size();
    std::memcpy(storage(victim), ticket.data(), ticket.size());
    ++m_size;
    ++m_counters.kept;
}

cont::ConstSpan<u8> TicketCache::take(const crypto::TlsBinding& binding) noexcept
{
    scrub();
    const u32 none = m_config.entries;
    u32 newest = none;
    for (u32 index = 0U; index < m_config.entries; ++index)
    {
        const Entry& entry = m_entries[index];
        if (entry.order != 0U && entry.binding == binding && (newest == none || entry.order > m_entries[newest].order))
        {
            newest = index;
        }
    }
    if (newest == none)
    {
        return {};
    }
    m_entries[newest].order = 0U; // free from now on: its bytes stay valid until the next call, which zeroes them
    m_handed = newest;
    --m_size;
    ++m_counters.taken;
    return cont::ConstSpan<u8>(storage(newest), m_entries[newest].bytes);
}

} // namespace crd::net
