#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/asan_poison.hpp>
#include <crd/memory/checked_math.hpp>
#include <crd/memory/log_channel.hpp>

namespace crd::memory
{
StackAllocator::StackAllocator(usize capacity, IAllocator* parent, const char* name)
    : m_parent(parent ? parent : default_allocator()), m_capacity(capacity)
{
    CRD_ASSERT(capacity > 0);
    m_name = name;
    m_buffer = static_cast<u8*>(m_parent->allocate(capacity, kDefaultAlignment));
    asan_poison(m_buffer, m_capacity); // DIAG.3b
}

StackAllocator::StackAllocator(void* buffer, usize capacity, const char* name) noexcept
    : m_buffer(static_cast<u8*>(buffer)), m_capacity(capacity)
{
    CRD_ASSERT(buffer != nullptr);
    CRD_ASSERT(capacity > 0);
    m_name = name;
    asan_poison(m_buffer, m_capacity);
}

StackAllocator::~StackAllocator()
{
    if (m_buffer)
    {
        asan_unpoison(m_buffer, m_capacity); // return memory clean (external buffers, parent reuse)
    }
    if (m_parent && m_buffer)
    {
        m_parent->deallocate(m_buffer);
    }
    m_buffer = nullptr;
    m_capacity = 0;
    m_offset = 0;
}

void* StackAllocator::allocate(usize size, usize alignment)
{
    CRD_ASSERT(size > 0);
    CRD_ASSERT(is_pow2(alignment));

    // DIAG.3a: checked so a near-SIZE_MAX size cannot wrap past the capacity test; on overflow
    // this fails like exhaustion, preserving the prior allocation.
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
        return nullptr;
    }

    m_offset = new_offset;
    m_stats.on_allocate(size);
    u8* const result = m_buffer + (new_offset - size);
    asan_unpoison(result, size); // DIAG.3b: this logical allocation is now live
    return result;
}

void StackAllocator::deallocate(void* /*p*/) noexcept
{
    // No-op. Use mark()/reset_to() to free.
}

bool StackAllocator::owns(const void* p) const noexcept
{
    const u8* bytes = static_cast<const u8*>(p);
    return bytes >= m_buffer && bytes < (m_buffer + m_capacity);
}

StackAllocator::Marker StackAllocator::mark() const noexcept
{
    Marker m{};
    m.offset = m_offset;
#if defined(CRD_DEBUG)
    m.owner = this;
#endif
    return m;
}

void StackAllocator::reset_to(Marker m) noexcept
{
#if defined(CRD_DEBUG)
    CRD_ASSERT(m.owner == this);
#endif
    CRD_ASSERT(m.offset <= m_offset);
    if (m_offset > m.offset)
    {
        m_stats.on_deallocate(m_offset - m.offset);
    }
    m_offset = m.offset;
    asan_poison(m_buffer + m.offset, m_capacity - m.offset); // DIAG.3b: popped frames go stale
}

void StackAllocator::reset() noexcept
{
    if (m_offset > 0)
    {
        m_stats.on_deallocate(m_offset);
    }
    m_offset = 0;
    asan_poison(m_buffer, m_capacity); // DIAG.3b
}
} // namespace crd::memory
