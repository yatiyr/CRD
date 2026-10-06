#include <crd/core/assert.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/memory/asan_poison.hpp>
#include <crd/memory/checked_math.hpp>

#include <cstring>
#include <utility>

namespace crd::memory
{

namespace
{
constexpr usize kInitialPagesCapacity = 4;

// DIAG.3b AddressSanitizer model, as in PoolAllocator: a free slot is poisoned whole, header included, and its link
// is read just in time; a live slot is unpoisoned whole (the logical allocation is the slot); a page is unpoisoned
// before it goes back to the parent. No-ops without ASan.
constexpr usize kPoolHeaderBytes = sizeof(void*); // GrowablePoolAllocator::FreeNode is one pointer

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

    // Checked (DIAG.3a): a stride times a slot count that wraps would build pages too small for their slots.
    if (!checked_mul(aligned_slot_stride(slot_size, slot_alignment), slots_per_page, &m_page_bytes))
    {
        CRD_FATAL("GrowablePoolAllocator: slot stride * slots_per_page overflows usize");
    }
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
            asan_unpoison(m_pages[i], m_page_bytes); // exactly the page, never the parent's surrounding range
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

bool GrowablePoolAllocator::grow()
{
    // Ensure space in the pages array. try_allocate: a parent that refuses (an exhausted arena, or the non-fatal path
    // of a fatal-on-OOM parent) leaves the pool as it was instead of writing through null (DIAG.3a).
    if (m_pages_size == m_pages_capacity)
    {
        const usize new_cap = (m_pages_capacity == 0) ? kInitialPagesCapacity : m_pages_capacity * 2U;

        void** new_buf = static_cast<void**>(m_parent->try_allocate(new_cap * sizeof(void*), alignof(void*)));
        if (new_buf == nullptr)
        {
            return false;
        }
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
    void* page = m_parent->try_allocate(m_page_bytes, m_slot_alignment);
    if (page == nullptr)
    {
        return false;
    }
    // Insert in address order, so page_of() can binary-search.
    usize at = m_pages_size;
    while (at > 0 && reinterpret_cast<usize>(m_pages[at - 1]) > reinterpret_cast<usize>(page))
    {
        m_pages[at] = m_pages[at - 1];
        --at;
    }
    m_pages[at] = page;
    ++m_pages_size;

    // Push every slot onto the free list. Walk back-to-front so the head ends
    // up pointing at the lowest-address slot — gives slightly more
    // cache-friendly allocation order on the first sweep through a fresh page.
    static_assert(sizeof(FreeNode) == kPoolHeaderBytes, "read_free_link reads exactly one FreeNode link");
    const usize stride = aligned_slot_stride(m_slot_size, m_slot_alignment);
    for (usize i = m_slots_per_page; i > 0; --i)
    {
        u8* slot_bytes = static_cast<u8*>(page) + (i - 1) * stride;
        FreeNode* node = reinterpret_cast<FreeNode*>(slot_bytes);
        node->next = m_free_head;
        m_free_head = node;
        asan_poison(slot_bytes, stride); // free: header and payload both fault
    }
    return true;
}

const u8* GrowablePoolAllocator::page_of(const void* p) const noexcept
{
    const usize addr = reinterpret_cast<usize>(p);
    usize lo = 0;
    usize hi = m_pages_size;
    while (lo < hi) // find the first page whose base is above p
    {
        const usize mid = lo + ((hi - lo) / 2U);
        if (reinterpret_cast<usize>(m_pages[mid]) <= addr)
        {
            lo = mid + 1U;
        }
        else
        {
            hi = mid;
        }
    }
    if (lo == 0)
    {
        return nullptr; // below every page
    }
    const u8* const base = static_cast<const u8*>(m_pages[lo - 1U]);
    return (addr - reinterpret_cast<usize>(base)) < m_page_bytes ? base : nullptr;
}

// ---- IAllocator ----------------------------------------------------------

void* GrowablePoolAllocator::try_allocate(usize size, usize alignment)
{
    if (size == 0U || size > m_slot_size || !is_pow2(alignment) || alignment > m_slot_alignment)
    {
        return nullptr; // the request does not fit a slot: refuse, never assert
    }
    if (m_free_head == nullptr && !grow())
    {
        return nullptr; // the parent refused a page: non-fatal here
    }
    return allocate(size, alignment);
}

void* GrowablePoolAllocator::allocate(usize size, usize alignment)
{
    CRD_ASSERT(size <= m_slot_size);
    CRD_ASSERT(is_pow2(alignment));
    CRD_ASSERT(alignment <= m_slot_alignment);
    (void)size;
    (void)alignment;

    if (m_free_head == nullptr && !grow())
    {
        CRD_FATAL("GrowablePoolAllocator: out of memory (the parent refused a page)");
        return nullptr;
    }

    FreeNode* node = m_free_head;
    m_free_head = static_cast<FreeNode*>(read_free_link(node));
    ++m_in_use;
    m_stats.on_allocate(static_cast<u64>(m_slot_size));
    asan_unpoison(node, aligned_slot_stride(m_slot_size, m_slot_alignment)); // the whole slot is live
    return node;
}

void GrowablePoolAllocator::deallocate(void* p) noexcept
{
    if (p == nullptr)
    {
        return;
    }
    // DIAG.3a: a pointer that is not one of this pool's slots (another allocator's block, or an interior pointer) is
    // refused in every build. Linking it into the free list would let the next allocate return foreign memory.
    if (!owns(p))
    {
        CRD_ASSERT_MSG(false, "GrowablePoolAllocator: deallocate of a pointer this pool does not own");
        return;
    }

    // A live slot is addressable; a double free finds it poisoned. Writing the link anyway keeps the corruption visible
    // to the structural checks (validate_structure()) instead of turning it into an allocator-internal ASan report.
    asan_unpoison(p, kPoolHeaderBytes);
    FreeNode* node = static_cast<FreeNode*>(p);
    node->next = m_free_head;
    m_free_head = node;
    --m_in_use;
    m_stats.on_deallocate(static_cast<u64>(m_slot_size));
    asan_poison(p, aligned_slot_stride(m_slot_size, m_slot_alignment)); // the freed slot faults, header included
}

bool GrowablePoolAllocator::owns(const void* p) const noexcept
{
    if (p == nullptr)
    {
        return false;
    }
    const u8* const page = page_of(p);
    if (page == nullptr)
    {
        return false;
    }
    // Must lie exactly on a slot boundary: an interior pointer was never handed out.
    const usize stride = aligned_slot_stride(m_slot_size, m_slot_alignment);
    return ((reinterpret_cast<usize>(p) - reinterpret_cast<usize>(page)) % stride) == 0;
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
