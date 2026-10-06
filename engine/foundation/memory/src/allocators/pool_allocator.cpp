#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/asan_poison.hpp>
#include <crd/memory/checked_math.hpp>
#include <crd/memory/log_channel.hpp>

#include <cstring>

namespace crd::memory
{
// DIAG.3b AddressSanitizer model. A free slot is poisoned whole, its FreeNode header included, so any read or write
// through a freed pointer faults, and so does a one-byte over- or underrun into a free neighbour. The allocator reads
// a free slot's link just in time (read_free_link). A live slot is unpoisoned whole: the pool's logical allocation is
// its slot (allocation_size() reports the slot size), so an access past the requested size but inside the slot is in
// bounds. An overrun into a live neighbour and immediate reuse of the same slot are raw-pointer limits (DIAG.3e
// generations). Everything is unpoisoned again before the buffer goes back to its parent or caller. No-ops without
// ASan.
namespace
{
constexpr usize kPoolHeaderBytes = sizeof(void*); // PoolAllocator::FreeNode is one pointer

// The link stored in a free slot. The header is unpoisoned only for the read and poisoned again afterwards; a slot
// that was not poisoned (a corrupt link into a live slot) stays as it was.
[[nodiscard]] void* read_free_link(const void* slot) noexcept
{
    const bool was_poisoned = asan_is_poisoned(slot);
    asan_unpoison(slot, kPoolHeaderBytes);
    void* next = nullptr;
    std::memcpy(&next, slot, sizeof(next));
    if (was_poisoned)
    {
        asan_poison(slot, kPoolHeaderBytes);
    }
    return next;
}
} // namespace

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
    if (m_buffer == nullptr)
    {
        // DIAG.3a: a parent that refuses (an exhausted arena) must not leave a pool that links slots through null.
        CRD_FATAL("PoolAllocator: parent refused the backing buffer");
    }
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
    if (m_buffer != nullptr)
    {
        // Exactly this pool's slots: never the parent's surrounding range, which may be poisoned on purpose.
        asan_unpoison(m_buffer, m_slot_size * m_slot_count);
    }
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
    static_assert(sizeof(FreeNode) == kPoolHeaderBytes, "read_free_link reads exactly one FreeNode link");
    // Walk the buffer and link every slot into a singly-linked free list.
    FreeNode* prev = nullptr;
    for (usize i = 0; i < m_slot_count; ++i)
    {
        u8* const slot = m_buffer + i * m_slot_size;
        asan_unpoison(slot, kPoolHeaderBytes); // an adopted buffer may arrive poisoned
        FreeNode* node = reinterpret_cast<FreeNode*>(slot);
        node->next = prev;
        prev = node;
        asan_poison(slot, m_slot_size); // free: header and payload both fault
    }
    m_free_head = prev; // points to the LAST node we walked = end of list
    m_in_use = 0;
}

void* PoolAllocator::try_allocate(usize size, usize alignment)
{
    if (size == 0U || size > m_slot_size || !is_pow2(alignment) || alignment > m_slot_alignment)
    {
        return nullptr; // the request does not fit a slot: refuse, never assert
    }
    return allocate(size, alignment);
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
    m_free_head = static_cast<FreeNode*>(read_free_link(node));
    ++m_in_use;
    m_stats.on_allocate(m_slot_size);
    asan_unpoison(node, m_slot_size); // the whole slot is live
    return node;
}

void PoolAllocator::deallocate(void* p) noexcept
{
    if (!p)
    {
        return;
    }
    // DIAG.3a: a pointer this pool did not hand out (another allocator's block, or an interior pointer) is refused in
    // every build. Linking it into the free list would let the next allocate return foreign memory.
    if (!owns(p))
    {
        CRD_ASSERT_MSG(false, "PoolAllocator: deallocate of a pointer this pool does not own");
        return;
    }

    // A live slot is addressable; a double free finds it poisoned. Writing the link anyway keeps the corruption visible
    // to the structural checks (validate_structure()) instead of turning it into an allocator-internal ASan report.
    asan_unpoison(p, kPoolHeaderBytes);
    FreeNode* node = static_cast<FreeNode*>(p);
    node->next = m_free_head;
    m_free_head = node;
    --m_in_use;
    m_stats.on_deallocate(m_slot_size);
    asan_poison(p, m_slot_size); // the freed slot faults, header included
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
    for (const FreeNode* n = m_free_head; n != nullptr; n = static_cast<const FreeNode*>(read_free_link(n)))
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
