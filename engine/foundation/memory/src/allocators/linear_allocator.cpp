#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/asan_poison.hpp>
#include <crd/memory/checked_math.hpp>
#include <crd/memory/log_channel.hpp>

namespace crd::memory
{
LinearAllocator::LinearAllocator(usize capacity, IAllocator* parent, const char* name)
    : m_parent(parent ? parent : default_allocator()), m_capacity(capacity)
{
    CRD_ASSERT(capacity > 0);
    m_name = name;
    m_buffer = static_cast<u8*>(m_parent->allocate(capacity, kDefaultAlignment));
    asan_poison(m_buffer, m_capacity); // nothing handed out yet
}

LinearAllocator::LinearAllocator(void* buffer, usize capacity, const char* name) noexcept
    : m_buffer(static_cast<u8*>(buffer)), m_capacity(capacity)
{
    CRD_ASSERT(buffer != nullptr);
    CRD_ASSERT(capacity > 0);
    m_name = name;
    asan_poison(m_buffer, m_capacity);
}

LinearAllocator::~LinearAllocator()
{
    // Return the memory unpoisoned: an external buffer belongs to the caller, and a parent
    // free of a poisoned region would confuse a subsequent reuse.
    if (m_buffer)
    {
        asan_unpoison(m_buffer, m_capacity);
    }
    if (m_parent && m_buffer)
    {
        m_parent->deallocate(m_buffer);
    }
    m_buffer = nullptr;
    m_capacity = 0;
    m_offset = 0;
}

void* LinearAllocator::allocate(usize size, usize alignment)
{
    CRD_ASSERT(size > 0);
    CRD_ASSERT(is_pow2(alignment));

    // Compute aligned start within our buffer. Checked so a near-SIZE_MAX size
    // cannot wrap new_offset small and slip past the capacity test below. On overflow this
    // fails like exhaustion -- non-fatal, and m_offset (the previous allocation) is preserved.
    const usize current = reinterpret_cast<usize>(m_buffer) + m_offset;
    usize       aligned = 0U;
    usize       new_offset = 0U;
    if (!checked_align_up(current, alignment, &aligned) ||
        !checked_add(m_offset, (aligned - current), &new_offset) ||
        !checked_add(new_offset, size, &new_offset))
    {
        CRD_LOG_ERROR(g_log_memory, "{} size arithmetic overflow (requested {})", m_name, size);
        return nullptr;
    }
    const usize padding = aligned - current;

    if (new_offset > m_capacity)
    {
        CRD_LOG_ERROR(g_log_memory, "{} exhausted (requested {} + {} pad, have {} of {})", m_name, size, padding,
                      m_capacity - m_offset, m_capacity);
        return nullptr; // exhaustion is non-fatal; caller decides
    }

    m_offset = new_offset;
    m_stats.on_allocate(size);
    u8* const result = m_buffer + (new_offset - size);
    asan_unpoison(result, size); // this logical allocation is now live (padding stays poisoned)
    return result;
}

void LinearAllocator::deallocate(void* /*p*/) noexcept
{
    // Linear allocators do not free per-allocation. Use reset().
    // We don't even bump dealloc_count, because nothing changed.
}

bool LinearAllocator::owns(const void* p) const noexcept
{
    const u8* bytes = static_cast<const u8*>(p);
    return bytes >= m_buffer && bytes < (m_buffer + m_capacity);
}

void LinearAllocator::reset() noexcept
{
    if (m_offset > 0)
    {
        // Stats: count one synthetic dealloc for the whole region.
        m_stats.on_deallocate(m_offset);
    }
    m_offset = 0;
    asan_poison(m_buffer, m_capacity); // use-after-reset of any prior pointer now faults
}

void LinearAllocator::reset_to(usize saved_offset) noexcept
{
    CRD_ASSERT(saved_offset <= m_offset);
    if (m_offset > saved_offset)
    {
        m_stats.on_deallocate(m_offset - saved_offset);
    }
    m_offset = saved_offset;
    // Poison the rewound tail so nested-arena reuse cannot read a scope's freed slices.
    asan_poison(m_buffer + saved_offset, m_capacity - saved_offset);
}

LinearScope::~LinearScope() noexcept
{
    // Roll back to where we were when the scope was created. Allocations
    // made before the scope are preserved.
    m_alloc.reset_to(m_saved_offset);
}
} // namespace crd::memory
