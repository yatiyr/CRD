#include <crd/core/assert.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>

#include <cstring>
#include <utility>

namespace crd::memory
{

namespace
{
constexpr usize kInitialPagesCapacity = 4;

inline usize aligned_slot_stride(usize slot_size, usize slot_alignment) noexcept
{
    // Slots are placed back-to-back. Each slot must start at slot_alignment;
    // the slot itself spans slot_size bytes. Stride = align_up(slot_size, slot_alignment)
    // ensures the next slot also lands aligned.
    return align_up(slot_size, slot_alignment);
}
} // namespace

// ---- Construction --------------------------------------------------------

GrowablePoolAllocator::GrowablePoolAllocator(usize slot_size, usize slot_alignment, usize slots_per_page,
                                             IAllocator* parent, const char* name)
    : m_parent(parent != nullptr ? parent : default_allocator()), m_slot_size(slot_size),
      m_slot_alignment(slot_alignment), m_slots_per_page(slots_per_page), m_page_bytes(0), m_pages(nullptr),
      m_pages_size(0), m_pages_capacity(0), m_free_head(nullptr), m_in_use(0)
{
    m_name = name;
    CRD_ASSERT(slot_size >= sizeof(FreeNode));
    CRD_ASSERT(is_pow2(slot_alignment));
    CRD_ASSERT(slot_alignment >= alignof(FreeNode));
    CRD_ASSERT(slots_per_page > 0);

    m_page_bytes = aligned_slot_stride(slot_size, slot_alignment) * slots_per_page;
}

GrowablePoolAllocator::GrowablePoolAllocator(GrowablePoolAllocator&& other) noexcept
    : m_parent(other.m_parent), m_slot_size(other.m_slot_size), m_slot_alignment(other.m_slot_alignment),
      m_slots_per_page(other.m_slots_per_page), m_page_bytes(other.m_page_bytes), m_pages(other.m_pages),
      m_pages_size(other.m_pages_size), m_pages_capacity(other.m_pages_capacity), m_free_head(other.m_free_head),
      m_in_use(other.m_in_use)
{
    m_name = other.m_name;
    other.m_parent = nullptr;
    other.m_pages = nullptr;
    other.m_pages_size = 0;
    other.m_pages_capacity = 0;
    other.m_free_head = nullptr;
    other.m_in_use = 0;
}

GrowablePoolAllocator& GrowablePoolAllocator::operator=(GrowablePoolAllocator&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    free_all_pages();

    m_parent = other.m_parent;
    m_slot_size = other.m_slot_size;
    m_slot_alignment = other.m_slot_alignment;
    m_slots_per_page = other.m_slots_per_page;
    m_page_bytes = other.m_page_bytes;
    m_pages = other.m_pages;
    m_pages_size = other.m_pages_size;
    m_pages_capacity = other.m_pages_capacity;
    m_free_head = other.m_free_head;
    m_in_use = other.m_in_use;
    m_name = other.m_name;

    other.m_parent = nullptr;
    other.m_pages = nullptr;
    other.m_pages_size = 0;
    other.m_pages_capacity = 0;
    other.m_free_head = nullptr;
    other.m_in_use = 0;
    return *this;
}

GrowablePoolAllocator::~GrowablePoolAllocator()
{
    free_all_pages();
}

void GrowablePoolAllocator::free_all_pages() noexcept
{
    if (m_parent == nullptr)
    {
        return;
    }
    for (usize i = 0; i < m_pages_size; ++i)
    {
        if (m_pages[i] != nullptr)
        {
            m_parent->deallocate(m_pages[i]);
        }
    }
    if (m_pages != nullptr)
    {
        m_parent->deallocate(m_pages);
    }
    m_pages = nullptr;
    m_pages_size = 0;
    m_pages_capacity = 0;
    m_free_head = nullptr;
    m_in_use = 0;
}

// ---- grow() — allocate a new page and link its slots into the free list ----

void GrowablePoolAllocator::grow()
{
    // Ensure space in the pages array.
    if (m_pages_size == m_pages_capacity)
    {
        const usize new_cap = (m_pages_capacity == 0) ? kInitialPagesCapacity : m_pages_capacity * 2U;

        void** new_buf = static_cast<void**>(m_parent->allocate(new_cap * sizeof(void*), alignof(void*)));
        if (m_pages != nullptr)
        {
            // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast) — copying a void*[] is intended.
            std::memcpy(new_buf, m_pages, m_pages_size * sizeof(void*));
            m_parent->deallocate(m_pages);
        }
        m_pages = new_buf;
        m_pages_capacity = new_cap;
    }

    // Allocate a new page from the parent at slot_alignment so every slot lands aligned.
    void* page = m_parent->allocate(m_page_bytes, m_slot_alignment);
    CRD_ASSERT(page != nullptr);
    m_pages[m_pages_size++] = page;

    // Push every slot onto the free list. Walk back-to-front so the head ends
    // up pointing at the lowest-address slot — gives slightly more
    // cache-friendly allocation order on the first sweep through a fresh page.
    const usize stride = aligned_slot_stride(m_slot_size, m_slot_alignment);
    for (usize i = m_slots_per_page; i > 0; --i)
    {
        u8* slot_bytes = static_cast<u8*>(page) + (i - 1) * stride;
        FreeNode* node = reinterpret_cast<FreeNode*>(slot_bytes);
        node->next = m_free_head;
        m_free_head = node;
    }
}

// ---- IAllocator ----------------------------------------------------------

void* GrowablePoolAllocator::allocate(usize size, usize alignment)
{
    CRD_ASSERT(size <= m_slot_size);
    CRD_ASSERT(is_pow2(alignment));
    CRD_ASSERT(alignment <= m_slot_alignment);
    (void)size;
    (void)alignment;

    if (m_free_head == nullptr)
    {
        grow();
    }
    CRD_ASSERT(m_free_head != nullptr);

    FreeNode* node = m_free_head;
    m_free_head = node->next;
    ++m_in_use;
    m_stats.on_allocate(static_cast<u64>(m_slot_size));
    return node;
}

void GrowablePoolAllocator::deallocate(void* p) noexcept
{
    if (p == nullptr)
    {
        return;
    }
    CRD_ASSERT(owns(p));

    FreeNode* node = static_cast<FreeNode*>(p);
    node->next = m_free_head;
    m_free_head = node;
    --m_in_use;
    m_stats.on_deallocate(static_cast<u64>(m_slot_size));
}

bool GrowablePoolAllocator::owns(const void* p) const noexcept
{
    if (p == nullptr)
    {
        return false;
    }
    const u8* pb = static_cast<const u8*>(p);
    for (usize i = 0; i < m_pages_size; ++i)
    {
        const u8* base = static_cast<const u8*>(m_pages[i]);
        if (pb >= base && pb < base + m_page_bytes)
        {
            return true;
        }
    }
    return false;
}

usize GrowablePoolAllocator::allocation_size(const void* p) const noexcept
{
    return owns(p) ? m_slot_size : 0;
}

usize GrowablePoolAllocator::page_count() const noexcept
{
    return m_pages_size;
}

usize GrowablePoolAllocator::slots_free() const noexcept
{
    return (m_pages_size * m_slots_per_page) - m_in_use;
}

} // namespace crd::memory
