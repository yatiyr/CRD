#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/log_channel.hpp>

namespace crd::memory
{
StackAllocator::StackAllocator(usize capacity, IAllocator* parent, const char* name)
    : m_parent(parent ? parent : default_allocator()), m_capacity(capacity)
{
    CRD_ASSERT(capacity > 0);
    m_name = name;
    m_buffer = static_cast<u8*>(m_parent->allocate(capacity, kDefaultAlignment));
}

StackAllocator::StackAllocator(void* buffer, usize capacity, const char* name) noexcept
    : m_buffer(static_cast<u8*>(buffer)), m_capacity(capacity)
{
    CRD_ASSERT(buffer != nullptr);
    CRD_ASSERT(capacity > 0);
    m_name = name;
}

StackAllocator::~StackAllocator()
{
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

    const usize current = reinterpret_cast<usize>(m_buffer) + m_offset;
    const usize aligned = align_up(current, alignment);
    const usize padding = aligned - current;
    const usize new_offset = m_offset + padding + size;

    if (new_offset > m_capacity)
    {
        CRD_LOG_ERROR(g_log_memory, "{} exhausted (requested {} + {} pad, have {} of {})", m_name, size, padding,
                      m_capacity - m_offset, m_capacity);
        return nullptr;
    }

    m_offset = new_offset;
    m_stats.on_allocate(size);
    return m_buffer + (new_offset - size);
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
}

void StackAllocator::reset() noexcept
{
    if (m_offset > 0)
    {
        m_stats.on_deallocate(m_offset);
    }
    m_offset = 0;
}
} // namespace crd::memory
