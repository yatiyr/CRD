#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/allocators/growable_linear_allocator.hpp>
#include <crd/memory/checked_math.hpp>
#include <crd/memory/log_channel.hpp>

#include <cstring> // std::memcpy (reallocate)
#include <new>     // placement new

namespace crd::memory
{
GrowableLinearAllocator::GrowableLinearAllocator(usize chunk_bytes, IAllocator* parent, const char* name)
    : m_parent(parent != nullptr ? parent : default_allocator()), m_chunk_bytes(chunk_bytes)
{
    CRD_ASSERT(chunk_bytes > 0);
    m_name        = name;
    m_header_size = align_up(sizeof(Chunk), kDefaultAlignment);
    grow(0); // reserve the first chunk so a modest working set never re-hits the parent
}

GrowableLinearAllocator::~GrowableLinearAllocator()
{
    Chunk* c = m_first;
    while (c != nullptr)
    {
        Chunk* const next = c->next;
        m_parent->deallocate(c->base);
        c = next;
    }
    m_first = m_last = m_current = nullptr;
}

bool GrowableLinearAllocator::grow(usize need)
{
    usize cap      = m_chunk_bytes;
    usize with_hdr = 0;
    if (!checked_add(need, m_header_size, &with_hdr))
    {
        return false; // no chunk can hold it (DIAG.3a: never a wrapped, small chunk)
    }
    if (with_hdr > cap) // an oversized alloc gets its own right-sized chunk
    {
        cap = with_hdr;
    }
    // try_allocate: a parent whose allocate is fatal on exhaustion still yields nullptr here.
    void* const base = m_parent->try_allocate(cap, kDefaultAlignment);
    if (base == nullptr)
    {
        CRD_LOG_ERROR(g_log_memory, "{} out of memory (requested chunk {} bytes)", m_name, cap);
        return false; // exhaustion is non-fatal; allocate() returns nullptr
    }
    Chunk* const c = new (base) Chunk{};
    c->base        = base;
    c->cap         = cap;
    c->off         = m_header_size;
    c->next        = nullptr;
    if (m_last != nullptr)
    {
        m_last->next = c;
    }
    else
    {
        m_first = c;
    }
    m_last    = c;
    m_current = c;
    return true;
}

void* GrowableLinearAllocator::allocate(usize size, usize alignment)
{
    CRD_ASSERT(size > 0);
    CRD_ASSERT(is_pow2(alignment));
    for (;;)
    {
        Chunk* const c = m_current;
        if (c != nullptr)
        {
            auto* const cbase   = static_cast<u8*>(c->base);
            const usize current = reinterpret_cast<usize>(cbase) + c->off; // ptr-to-int for the align computation
            usize       aligned = 0;
            usize       new_off = 0;
            // Checked: a huge size or alignment refuses instead of wrapping back inside the chunk (DIAG.3a).
            const bool fits = checked_align_up(current, alignment, &aligned) &&
                              checked_add(c->off + (aligned - current), size, &new_off) && new_off <= c->cap;
            if (fits)
            {
                c->off = new_off;
                m_stats.on_allocate(size);
                return cbase + (new_off - size); // pointer arithmetic — no int-to-ptr cast
            }
            // current chunk is full — reuse the next existing chunk (rewound by a prior reset()), else grow.
            if (c->next != nullptr)
            {
                m_current = c->next;
                continue;
            }
        }
        usize need = 0;
        if (!checked_add(size, alignment, &need) || !grow(need))
        {
            return nullptr;
        }
    }
}

void GrowableLinearAllocator::deallocate(void* /*p*/) noexcept
{
    // Arena: individual frees are a no-op. Use reset() to reclaim the whole arena for reuse.
}

bool GrowableLinearAllocator::owns(const void* p) const noexcept
{
    const auto* bytes = static_cast<const u8*>(p);
    for (const Chunk* c = m_first; c != nullptr; c = c->next)
    {
        const auto* base = static_cast<const u8*>(c->base);
        if (bytes >= base && bytes < base + c->cap)
        {
            return true;
        }
    }
    return false;
}

void* GrowableLinearAllocator::reallocate(void* p, usize old_size, usize new_size, usize alignment)
{
    if (new_size == 0U)
    {
        return nullptr;
    }
    void* const np = allocate(new_size, alignment);
    if (np != nullptr && p != nullptr && old_size > 0U)
    {
        std::memcpy(np, p, old_size < new_size ? old_size : new_size);
    }
    return np; // the old slice leaks into the arena by design (deallocate is a no-op)
}

void GrowableLinearAllocator::reset() noexcept
{
    usize freed = 0;
    for (Chunk* c = m_first; c != nullptr; c = c->next)
    {
        freed += c->off - m_header_size;
        c->off = m_header_size;
    }
    if (freed > 0U)
    {
        m_stats.on_deallocate(freed);
    }
    m_current = m_first;
}

usize GrowableLinearAllocator::num_chunks() const noexcept
{
    usize n = 0;
    for (const Chunk* c = m_first; c != nullptr; c = c->next)
    {
        ++n;
    }
    return n;
}

usize GrowableLinearAllocator::bytes_reserved() const noexcept
{
    usize s = 0;
    for (const Chunk* c = m_first; c != nullptr; c = c->next)
    {
        s += c->cap;
    }
    return s;
}

usize GrowableLinearAllocator::bytes_used() const noexcept
{
    usize s = 0;
    for (const Chunk* c = m_first; c != nullptr; c = c->next)
    {
        s += c->off - m_header_size;
    }
    return s;
}
} // namespace crd::memory
