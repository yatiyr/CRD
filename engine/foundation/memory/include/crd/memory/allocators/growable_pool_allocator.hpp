#pragma once

#include <crd/memory/allocator.hpp>

namespace crd::memory
{
// GrowablePoolAllocator — auto-growing pool of fixed-size aligned blocks.
//
// O(1) allocate (free-list pop) and deallocate (free-list push). When the
// free list is empty, allocates a new "page" from `parent` containing
// `slots_per_page` contiguous slots, then links every slot onto the free
// list. Pages are kept allocated for the life of the allocator (no
// auto-shrink) — repeated alloc/free cycles do not thrash the parent.
//
// Properties:
//   - allocate(size, alignment) requires size ≤ slot_size AND
//     alignment ≤ slot_alignment. Otherwise CRD_FATAL. A page the parent
//     refuses is out of memory: allocate() is fatal, try_allocate() returns
//     nullptr and the pool stays usable (DIAG.3a).
//   - deallocate(p) is O(log pages). p must be a slot returned by allocate();
//     any other pointer (another allocator's block, an interior pointer) is
//     refused in every build, after an assert where asserts are on (DIAG.3a).
//   - owns(p) is O(log pages): the page table is kept sorted by address.
//   - allocation_size(p) returns slot_size when p is owned.
//   - Move-constructible / move-assignable (transfers pages + free list).
//   - Not copy-able. Not thread-safe (per project IAllocator convention).
//
// Use cases:
//   - ECS archetype chunks (16 KB blocks, 64-byte aligned). See
//     `crd::scene::ChunkAllocator` which wraps this.
//   - Particle records, voice instances, fixed-size object pools that
//     grow without bound.
//   - Any "many of the same struct, lifetime managed by caller" workload.
//
// Memory shape per page:
//   page_total_bytes = slot_size_aligned_up_to(slot_alignment) * slots_per_page
//   pages are allocated at slot_alignment from `parent` so every slot is
//   slot_alignment-aligned.
//
// Free-list layout: when a slot is free, its first sizeof(void*) bytes
// hold a `FreeNode*` to the next free slot. slot_size MUST be ≥ sizeof(void*)
// (CRD_ASSERT in ctor).
class GrowablePoolAllocator : public IAllocator
{
public:
    // Owning ctor. `parent` allocates pages on demand. nullptr ⇒ default_allocator().
    GrowablePoolAllocator(usize slot_size, usize slot_alignment, usize slots_per_page, IAllocator* parent = nullptr,
                          const char* name = "GrowablePool");

    ~GrowablePoolAllocator() override;

    GrowablePoolAllocator(const GrowablePoolAllocator&) = delete;
    GrowablePoolAllocator& operator=(const GrowablePoolAllocator&) = delete;
    GrowablePoolAllocator(GrowablePoolAllocator&&) noexcept;
    GrowablePoolAllocator& operator=(GrowablePoolAllocator&&) noexcept;

    // ---- IAllocator ---------------------------------------------------
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    // DIAG.3a: a size or alignment no slot can hold returns nullptr instead of asserting; otherwise allocate().
    [[nodiscard]] void* try_allocate(usize size, usize alignment = kDefaultAlignment) override;
    void deallocate(void* p) noexcept override;
    [[nodiscard]] bool owns(const void* p) const noexcept override;
    [[nodiscard]] usize allocation_size(const void* p) const noexcept override;

    // ---- Pool extras --------------------------------------------------
    [[nodiscard]] usize slot_size() const noexcept { return m_slot_size; }
    [[nodiscard]] usize slot_alignment() const noexcept { return m_slot_alignment; }
    [[nodiscard]] usize slots_per_page() const noexcept { return m_slots_per_page; }
    [[nodiscard]] usize page_count() const noexcept;
    [[nodiscard]] usize slots_in_use() const noexcept { return m_in_use; }
    [[nodiscard]] usize slots_free() const noexcept;

private:
    struct FreeNode
    {
        FreeNode* next;
    };

    // Adds one page. False when the parent refuses; then nothing changed except, possibly, a larger page table.
    [[nodiscard]] bool grow();
    void free_all_pages() noexcept;
    // The page holding p, or null. Binary search over the address-sorted page table.
    [[nodiscard]] const u8* page_of(const void* p) const noexcept;

    IAllocator* m_parent;
    usize m_slot_size;
    usize m_slot_alignment;
    usize m_slots_per_page;
    usize m_page_bytes; // = align_up(slot_size, slot_alignment) * slots_per_page

    // Pages are tracked in a manually-grown void*[] (we cannot use
    // crd::containers::Array — that would invert the crd-memory →
    // crd-containers dependency). Pages grow doubling from kInitialPagesCapacity.
    // Kept sorted by address, so ownership is a binary search.
    void** m_pages;
    usize m_pages_size;
    usize m_pages_capacity;
    FreeNode* m_free_head;
    usize m_in_use;
};

} // namespace crd::memory
