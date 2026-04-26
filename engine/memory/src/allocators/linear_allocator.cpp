#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/log_channel.hpp>

namespace crd::memory
{
LinearAllocator::LinearAllocator(usize capacity, IAllocator* parent, const char* name)
    : m_parent(parent ? parent : default_allocator()), m_capacity(capacity)
{
    CRD_ASSERT(capacity > 0);
    m_name = name;
    m_buffer = static_cast<u8*>(m_parent->allocate(capacity, kDefaultAlignment));
}

LinearAllocator::LinearAllocator(void* buffer, usize capacity, const char* name) noexcept
    : m_buffer(static_cast<u8*>(buffer)), m_capacity(capacity)
{
    CRD_ASSERT(buffer != nullptr);
    CRD_ASSERT(capacity > 0);
    m_name = name;
}

LinearAllocator::~LinearAllocator()
{
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

    // Compute aligned start within our buffer.
    const usize current = reinterpret_cast<usize>(m_buffer) + m_offset;
    const usize aligned = align_up(current, alignment);
    const usize padding = aligned - current;
    const usize new_offset = m_offset + padding + size;

    if (new_offset > m_capacity)
    {
        CRD_LOG_ERROR(g_log_memory, "{} exhausted (requested {} + {} pad, have {} of {})", m_name, size, padding,
                      m_capacity - m_offset, m_capacity);
        return nullptr; // exhaustion is non-fatal; caller decides
    }

    m_offset = new_offset;
    m_stats.on_allocate(size);
    return reinterpret_cast<void*>(aligned);
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
}

void LinearAllocator::reset_to(usize saved_offset) noexcept
{
    CRD_ASSERT(saved_offset <= m_offset);
    if (m_offset > saved_offset)
    {
        m_stats.on_deallocate(m_offset - saved_offset);
    }
    m_offset = saved_offset;
}

LinearScope::~LinearScope() noexcept
{
    // Roll back to where we were when the scope was created. Allocations
    // made before the scope are preserved.
    m_alloc.reset_to(m_saved_offset);
}
} // namespace crd::memory
