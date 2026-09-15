#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/checked_math.hpp>
#include <crd/memory/log_channel.hpp>

namespace crd::memory
{
PoolAllocator::PoolAllocator(usize slot_size, usize slot_count, usize slot_alignment, IAllocator* parent,
                             const char* name)
    : m_parent(parent ? parent : default_allocator()), m_slot_alignment(slot_alignment), m_slot_count(slot_count)
{
    CRD_ASSERT(is_pow2(slot_alignment));
    CRD_ASSERT(slot_count > 0);
    CRD_ASSERT(slot_size >= sizeof(FreeNode));

    m_name = name;
    // Checked stride/size arithmetic. slot_size padded to alignment, then
    // stride*count -- both overflow-checked so a hostile/buggy size can never wrap to a
    // too-small buffer. CRD_FATAL (not a debug assert) so the guard survives release.
    usize total = 0U;
    if (!checked_align_up(slot_size, slot_alignment, &m_slot_size) ||
        !checked_mul(m_slot_size, m_slot_count, &total))
    {
        CRD_FATAL("PoolAllocator: slot_size padded * slot_count overflows usize");
    }
    m_buffer = static_cast<u8*>(m_parent->allocate(total, slot_alignment));
    build_free_list();
}

PoolAllocator::PoolAllocator(void* buffer, usize slot_size, usize slot_count, usize slot_alignment,
                             const char* name) noexcept
    : m_buffer(static_cast<u8*>(buffer)), m_slot_alignment(slot_alignment), m_slot_count(slot_count)
{
    CRD_ASSERT(is_pow2(slot_alignment));
    CRD_ASSERT(slot_count > 0);
    CRD_ASSERT(slot_size >= sizeof(FreeNode));
    CRD_ASSERT(buffer != nullptr);

    m_name = name;
    m_slot_size = align_up(slot_size, slot_alignment);
    build_free_list();
}

PoolAllocator::~PoolAllocator()
{
    if (m_parent && m_buffer)
    {
        m_parent->deallocate(m_buffer);
    }
    m_buffer = nullptr;
    m_free_head = nullptr;
    m_in_use = 0;
}

void PoolAllocator::build_free_list() noexcept
{
    // Walk the buffer and link every slot into a singly-linked free list.
    FreeNode* prev = nullptr;
    for (usize i = 0; i < m_slot_count; ++i)
    {
        FreeNode* node = reinterpret_cast<FreeNode*>(m_buffer + i * m_slot_size);
        node->next = prev;
        prev = node;
    }
    m_free_head = prev; // points to the LAST node we walked = end of list
    m_in_use = 0;
}

void* PoolAllocator::allocate(usize size, usize alignment)
{
    CRD_ASSERT(size > 0);
    CRD_ASSERT(size <= m_slot_size);
    CRD_ASSERT(is_pow2(alignment));
    CRD_ASSERT(alignment <= m_slot_alignment);
    (void)size;
    (void)alignment;

    if (!m_free_head)
    {
        CRD_LOG_WARN(g_log_memory, "{} exhausted ({} of {} slots in use)", m_name, m_in_use, m_slot_count);
        return nullptr;
    }

    FreeNode* node = m_free_head;
    m_free_head = node->next;
    ++m_in_use;
    m_stats.on_allocate(m_slot_size);
    return node;
}

void PoolAllocator::deallocate(void* p) noexcept
{
    if (!p)
    {
        return;
    }
    CRD_ASSERT(owns(p));

    FreeNode* node = static_cast<FreeNode*>(p);
    node->next = m_free_head;
    m_free_head = node;
    --m_in_use;
    m_stats.on_deallocate(m_slot_size);
}

bool PoolAllocator::owns(const void* p) const noexcept
{
    const u8* bytes = static_cast<const u8*>(p);
    if (bytes < m_buffer || bytes >= (m_buffer + m_slot_size * m_slot_count))
    {
        return false;
    }
    // Must lie exactly on a slot boundary, otherwise it's an interior pointer
    // and definitely not something we handed out.
    const usize offset = static_cast<usize>(bytes - m_buffer);
    return (offset % m_slot_size) == 0;
}

usize PoolAllocator::allocation_size(const void* p) const noexcept
{
    return owns(p) ? m_slot_size : 0;
}

bool PoolAllocator::is_slot_aligned(const void* p) const noexcept
{
    const u8* bytes = static_cast<const u8*>(p);
    if (bytes < m_buffer || bytes >= (m_buffer + m_slot_size * m_slot_count))
    {
        return false;
    }
    return (static_cast<usize>(bytes - m_buffer) % m_slot_size) == 0;
}

bool PoolAllocator::validate_structure() const noexcept
{
    const u8* const base = m_buffer;
    const u8* const end  = m_buffer + m_slot_size * m_slot_count;

    usize seen = 0;
    for (const FreeNode* n = m_free_head; n != nullptr; n = n->next)
    {
        const u8* const b = reinterpret_cast<const u8*>(n);
        if (b < base || b >= end) // link points outside the buffer
        {
            return false;
        }
        if ((static_cast<usize>(b - base) % m_slot_size) != 0) // link not on a slot boundary
        {
            return false;
        }
        if (++seen > m_slot_count) // cycle or over-length -- the shape a double-free produces
        {
            return false;
        }
    }
    // A consistent free list has exactly slots_free() entries; a mismatch means a leaked or
    // double-freed slot.
    return seen == (m_slot_count - m_in_use);
}
} // namespace crd::memory
