// crd-net -- which connection of a fixed pool is due next (4.2a; docs/design/network-foundation.md#quic-server).

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/deadline_heap.hpp>

namespace crd::net
{

bool DeadlineHeap::init(u32 capacity) noexcept
{
    if (m_entries != nullptr || capacity == 0U)
    {
        return false;
    }
    Entry* const entries = memory::try_construct_array<Entry>(m_allocator, capacity);
    u32* const heap = memory::try_construct_array<u32>(m_allocator, capacity);
    if (entries == nullptr || heap == nullptr)
    {
        memory::destroy_array(m_allocator, entries, capacity);
        memory::destroy_array(m_allocator, heap, capacity);
        return false;
    }
    m_entries = entries;
    m_heap = heap;
    m_capacity = capacity;
    return true;
}

DeadlineHeap::~DeadlineHeap()
{
    memory::destroy_array(m_allocator, m_entries, m_capacity);
    memory::destroy_array(m_allocator, m_heap, m_capacity);
}

void DeadlineHeap::schedule(u32 index, u64 due_ns) noexcept
{
    Entry& entry = m_entries[index];
    entry.due_ns = due_ns;
    if (entry.place == kAbsent)
    {
        put(m_size, index);
        ++m_size;
    }
    sift_up(entry.place);
    sift_down(entry.place);
}

void DeadlineHeap::unschedule(u32 index) noexcept
{
    Entry& entry = m_entries[index];
    const u32 place = entry.place;
    if (place == kAbsent)
    {
        return;
    }
    entry.place = kAbsent;
    --m_size;
    if (place == m_size)
    {
        return;
    }
    const u32 moved = m_heap[m_size];
    put(place, moved);
    sift_up(place);
    sift_down(m_entries[moved].place);
}

u64 DeadlineHeap::next_due() const noexcept
{
    return (m_size != 0U) ? m_entries[m_heap[0]].due_ns : kNeverNs;
}

u32 DeadlineHeap::take_due(u64 now_ns, cont::Span<u32> out) noexcept
{
    u32 due = 0U;
    while (m_size != 0U && m_entries[m_heap[0]].due_ns <= now_ns && due < out.size())
    {
        const u32 index = m_heap[0];
        unschedule(index);
        out[due++] = index;
    }
    return due;
}

void DeadlineHeap::sift_up(u32 place) noexcept
{
    const u32 index = m_heap[place];
    const u64 due = m_entries[index].due_ns;
    while (place != 0U)
    {
        const u32 parent = (place - 1U) / 2U;
        if (m_entries[m_heap[parent]].due_ns <= due)
        {
            break;
        }
        put(place, m_heap[parent]);
        place = parent;
    }
    put(place, index);
}

void DeadlineHeap::sift_down(u32 place) noexcept
{
    const u32 index = m_heap[place];
    const u64 due = m_entries[index].due_ns;
    for (;;)
    {
        const u32 left = (2U * place) + 1U;
        if (left >= m_size)
        {
            break;
        }
        const u32 right = left + 1U;
        const u32 child =
            (right < m_size && m_entries[m_heap[right]].due_ns < m_entries[m_heap[left]].due_ns) ? right : left;
        if (due <= m_entries[m_heap[child]].due_ns)
        {
            break;
        }
        put(place, m_heap[child]);
        place = child;
    }
    put(place, index);
}

void DeadlineHeap::put(u32 place, u32 index) noexcept
{
    m_heap[place] = index;
    m_entries[index].place = place;
}

} // namespace crd::net
