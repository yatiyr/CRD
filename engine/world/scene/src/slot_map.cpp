#include <crd/core/assert.hpp>
#include <crd/scene/slot_map.hpp>

namespace crd::scene
{

SlotMap::SlotMap(crd::memory::IAllocator* alloc) : m_slots(alloc)
{
    // Slot 0 is the null sentinel. It is permanently dead and never allocated.
    Slot sentinel{};
    sentinel.generation = 0;
    sentinel.next_free = kInvalidSlotIndex;
    sentinel.alive = false;
    m_slots.push_back(sentinel);
}

EntityId SlotMap::allocate()
{
    if (m_free_head != kInvalidSlotIndex)
    {
        const crd::u32 idx = m_free_head;
        Slot& s = m_slots[idx];
        CRD_ASSERT(!s.alive);
        m_free_head = s.next_free;

        s.alive = true;
        s.next_free = kInvalidSlotIndex;
        ++m_alive_count;
        return EntityId::make(idx, s.generation);
    }

    // No free slot — append a new one. Initial generation is 1 so that a default
    // EntityId{} (generation 0, index 0) never matches a freshly minted handle.
    Slot s{};
    s.generation = 1;
    s.next_free = kInvalidSlotIndex;
    s.alive = true;
    m_slots.push_back(s);

    const crd::u32 idx = static_cast<crd::u32>(m_slots.size() - 1);
    ++m_alive_count;
    return EntityId::make(idx, 1);
}

void SlotMap::free(EntityId e)
{
    CRD_ASSERT(is_alive(e));

    Slot& s = m_slots[e.index()];

    // Bump generation. Wrap-around at 2^32 is documented as extreme abuse;
    // skip the value 0 to keep "generation 0" reserved as the sentinel/dead value.
    ++s.generation;
    if (s.generation == 0)
    {
        s.generation = 1;
    }

    s.alive = false;
    s.next_free = m_free_head;
    m_free_head = e.index();

    --m_alive_count;
}

bool SlotMap::is_alive(EntityId e) const noexcept
{
    const crd::u32 idx = e.index();
    if (idx == 0 || idx >= m_slots.size())
    {
        return false;
    }
    const Slot& s = m_slots[idx];
    return s.alive && s.generation == e.generation();
}

// ---- Iterator ------------------------------------------------------------

SlotMap::Iterator::Iterator(const SlotMap* map, crd::u32 index) noexcept : m_map(map), m_index(index)
{
    advance_to_alive();
}

void SlotMap::Iterator::advance_to_alive() noexcept
{
    const crd::u32 n = static_cast<crd::u32>(m_map->m_slots.size());
    while (m_index < n && !m_map->m_slots[m_index].alive)
    {
        ++m_index;
    }
}

EntityId SlotMap::Iterator::operator*() const noexcept
{
    const Slot& s = m_map->m_slots[m_index];
    return EntityId::make(m_index, s.generation);
}

SlotMap::Iterator& SlotMap::Iterator::operator++() noexcept
{
    ++m_index;
    advance_to_alive();
    return *this;
}

SlotMap::Iterator SlotMap::begin() const noexcept
{
    // Start at slot 1 — slot 0 is the null sentinel and is never alive.
    return Iterator{this, 1};
}

SlotMap::Iterator SlotMap::end() const noexcept
{
    return Iterator{this, static_cast<crd::u32>(m_slots.size())};
}

} // namespace crd::scene
