#pragma once

#include <crd/memory/allocator.hpp>

namespace crd::memory
{
// Fixed-size object pool with O(1) allocate/deallocate. Holds an array of
// N "slots", each large enough for one object of size `slot_size` and
// `slot_alignment`. Free slots form an intrusive singly-linked list.
//
// Use cases:
//  - ECS components of one type.
//  - Sound voice instances, particle records, etc.
//  - Any "I need many of the same struct" workload.
//
// Properties:
//  - allocate() returns one slot or nullptr if the pool is exhausted.
//    (Pool exhaustion is NOT fatal — caller decides whether to grow,
//    fall back to default, or simply refuse the allocation.)
//  - allocate(size, alignment) requires size <= slot_size and
//    alignment <= slot_alignment; otherwise CRD_FATAL.
//  - deallocate() in O(1) by pushing the slot onto the free list.
//  - owns() is range-based (cheap) — true iff `p` lies inside our buffer.
//  - Not thread-safe.
//
// Constructor variants mirror the other allocators (owning + adopting).
class PoolAllocator : public IAllocator
{
public:
    // Owning ctor: allocates the backing buffer from `parent`.
    // slot_size must be >= sizeof(void*), slot_alignment must be a power of two.
    PoolAllocator(usize slot_size, usize slot_count, usize slot_alignment = kDefaultAlignment,
                  IAllocator* parent = nullptr, const char* name = "PoolAllocator");

    // Non-owning ctor: takes a pre-allocated buffer big enough for
    // slot_count slots of (slot_size aligned up to slot_alignment).
    PoolAllocator(void* buffer, usize slot_size, usize slot_count, usize slot_alignment = kDefaultAlignment,
                  const char* name = "PoolAllocator") noexcept;

    ~PoolAllocator() override;

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    // ---- IAllocator -----------------------------------------------
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    void deallocate(void* p) noexcept override;
    bool owns(const void* p) const noexcept override;

    usize allocation_size(const void* p) const noexcept override;

    // ---- PoolAllocator extras -------------------------------------
    usize slot_size() const noexcept { return m_slot_size; }
    usize slot_alignment() const noexcept { return m_slot_alignment; }
    usize slot_count() const noexcept { return m_slot_count; }
    usize slots_in_use() const noexcept { return m_in_use; }
    usize slots_free() const noexcept { return m_slot_count - m_in_use; }

private:
    struct FreeNode
    {
        FreeNode* next;
    };

    void build_free_list() noexcept;

    IAllocator* m_parent = nullptr;
    u8* m_buffer = nullptr;
    usize m_slot_size = 0; // already aligned up
    usize m_slot_alignment = 0;
    usize m_slot_count = 0;
    usize m_in_use = 0;
    FreeNode* m_free_head = nullptr;
};
} // namespace crd::memory
