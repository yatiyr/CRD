// crd-net -- admission stage S2's connection ID table (4.2a; docs/design/network-foundation.md#ingress).

#include <crd/memory/allocator.hpp>
#include <crd/net/connection_table.hpp>

#include <new>

namespace crd::net
{
namespace
{
bool same_id(const u8* stored, u8 length, cont::ConstSpan<u8> id) noexcept
{
    if (length != id.size())
    {
        return false;
    }
    for (usize index = 0U; index < id.size(); ++index)
    {
        if (stored[index] != id[index])
        {
            return false;
        }
    }
    return true;
}
} // namespace

ConnectionIdTable::ConnectionIdTable(memory::IAllocator& allocator, u32 capacity, cont::ConstSpan<u8> key) noexcept
    : m_allocator(allocator)
{
    if (capacity == 0U || capacity > kMaxRoutes)
    {
        return;
    }
    const crypto::CryptoError keyed = key.empty() ? m_key.generate(crypto::AlgorithmId::Siphash24)
                                                  : m_key.init(crypto::AlgorithmId::Siphash24, key);
    if (keyed != crypto::CryptoError::None)
    {
        return;
    }
    u32 slots = 1U;
    while (slots < 2U * capacity)
    {
        slots <<= 1U;
    }
    void* const storage = allocator.try_allocate(sizeof(Slot) * slots, alignof(Slot));
    if (storage == nullptr)
    {
        m_key.clear();
        return;
    }
    m_slots = static_cast<Slot*>(storage);
    for (u32 index = 0U; index < slots; ++index)
    {
        new (&m_slots[index]) Slot{};
    }
    m_mask = slots - 1U;
    m_capacity = capacity;
}

ConnectionIdTable::~ConnectionIdTable()
{
    if (m_slots != nullptr)
    {
        m_allocator.deallocate(m_slots);
    }
}

bool ConnectionIdTable::hash_of(cont::ConstSpan<u8> id, u64& out) const noexcept
{
    return m_key.hash(id, out) == crypto::CryptoError::None;
}

u32 ConnectionIdTable::locate(cont::ConstSpan<u8> id, u64 hash) const noexcept
{
    u32 index = static_cast<u32>(hash) & m_mask;
    u32 walked = 1U;
    while (m_slots[index].connection != kNoConnection)
    {
        const Slot& slot = m_slots[index];
        if (slot.hash == hash && same_id(slot.id, slot.length, id))
        {
            break;
        }
        index = (index + 1U) & m_mask;
        ++walked;
    }
    if (walked > m_longest_probe)
    {
        m_longest_probe = walked;
    }
    return index;
}

RouteError ConnectionIdTable::insert(cont::ConstSpan<u8> id, u32 connection) noexcept
{
    if (m_slots == nullptr)
    {
        return RouteError::NotReady;
    }
    if (id.size() > crypto::kQuicMaxConnectionIdBytes)
    {
        return RouteError::BadLength;
    }
    if (connection == kNoConnection)
    {
        return RouteError::BadValue;
    }
    u64 hash = 0U;
    if (!hash_of(id, hash))
    {
        return RouteError::NotReady;
    }
    const u32 index = locate(id, hash);
    Slot& slot = m_slots[index];
    if (slot.connection != kNoConnection)
    {
        return RouteError::Exists;
    }
    if (m_size == m_capacity)
    {
        return RouteError::Full;
    }
    slot.hash = hash;
    slot.connection = connection;
    slot.length = static_cast<u8>(id.size());
    for (usize byte = 0U; byte < id.size(); ++byte)
    {
        slot.id[byte] = id[byte];
    }
    ++m_size;
    return RouteError::None;
}

u32 ConnectionIdTable::find(cont::ConstSpan<u8> id) const noexcept
{
    u64 hash = 0U;
    if (m_slots == nullptr || id.size() > crypto::kQuicMaxConnectionIdBytes || !hash_of(id, hash))
    {
        return kNoConnection;
    }
    return m_slots[locate(id, hash)].connection;
}

bool ConnectionIdTable::erase(cont::ConstSpan<u8> id) noexcept
{
    u64 hash = 0U;
    if (m_slots == nullptr || id.size() > crypto::kQuicMaxConnectionIdBytes || !hash_of(id, hash))
    {
        return false;
    }
    u32 hole = locate(id, hash);
    if (m_slots[hole].connection == kNoConnection)
    {
        return false;
    }
    // Backward-shift deletion (Knuth, TAOCP vol. 3, section 6.4, Algorithm R): each later slot of the run whose home
    // is not cyclically within (hole, slot] moves into the hole, so every lookup still finds its run unbroken.
    u32 next = hole;
    for (;;)
    {
        next = (next + 1U) & m_mask;
        const Slot& candidate = m_slots[next];
        if (candidate.connection == kNoConnection)
        {
            break;
        }
        const u32 home = static_cast<u32>(candidate.hash) & m_mask;
        const u32 from_home = (next - home) & m_mask;
        const u32 from_hole = (next - hole) & m_mask;
        if (from_home >= from_hole)
        {
            m_slots[hole] = candidate;
            hole = next;
        }
    }
    m_slots[hole] = Slot{};
    --m_size;
    return true;
}

} // namespace crd::net
