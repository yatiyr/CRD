#pragma once

#include <crd/memory/allocator.hpp>

namespace crd::memory
{
// Bump allocator. Holds one fixed-size memory block. `allocate()` advances
// an offset; `deallocate()` is a NO-OP. Use `reset()` (or `MemoryScope`)
// to wipe everything in O(1).
//
// Use cases:
//  - Per-frame scratch memory: reset at the top of each frame.
//  - Build-time data baking: allocate a lot, throw it all away at end.
//  - Temporary working sets that have a clear "all-or-nothing" lifetime.
//
// Two flavors:
//  - Construct with (capacity, parent_alloc=default): we own and free
//    the underlying block via parent_alloc.
//  - Construct with (existing_buffer, capacity): we use buffer as-is and
//    don't free it. Useful for stack-backed or arena-backed allocators.
//
// Not thread-safe. One LinearAllocator per thread, or wrap externally.
class LinearAllocator : public IAllocator
{
public:
    // Owning ctor: allocates `capacity` bytes from `parent`.
    explicit LinearAllocator(usize capacity, IAllocator* parent = nullptr, const char* name = "LinearAllocator");

    // Non-owning ctor: takes a pre-allocated buffer; we never free it.
    LinearAllocator(void* buffer, usize capacity, const char* name = "LinearAllocator") noexcept;

    ~LinearAllocator() override;

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    // ---- IAllocator -----------------------------------------------
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    void deallocate(void* p) noexcept override; // no-op
    bool owns(const void* p) const noexcept override;

    // ---- LinearAllocator extras -----------------------------------
    // Mark all memory as free again. O(1). Pointers handed out before
    // reset are invalidated immediately.
    void reset() noexcept;

    // Roll the offset back to a previously-saved value. Used by LinearScope
    // for partial rollback. Pointers allocated after `saved_offset` are
    // invalidated.
    void reset_to(usize saved_offset) noexcept;

    // How many bytes have been handed out so far (excludes alignment padding
    // if measured at the offset level).
    usize used() const noexcept { return m_offset; }
    usize capacity() const noexcept { return m_capacity; }
    usize remaining() const noexcept { return m_capacity - m_offset; }
    const void* base() const noexcept { return m_buffer; }

private:
    IAllocator* m_parent = nullptr; // non-null iff we own the buffer
    u8* m_buffer = nullptr;
    usize m_capacity = 0;
    usize m_offset = 0;
};

// RAII helper. On construction it captures the allocator's current offset;
// on destruction it resets the allocator. Useful for scoped temporary
// allocations:
//
//   {
//       MemoryScope scope(temp_alloc);
//       // ... allocate, allocate, allocate ...
//   } // everything freed
//
// Note: this calls reset(), which wipes the allocator entirely. If you
// need nested LIFO scopes, use StackAllocator instead.
class LinearScope
{
public:
    explicit LinearScope(LinearAllocator& alloc) noexcept : m_alloc(alloc), m_saved_offset(alloc.used()) {}
    ~LinearScope() noexcept;

    LinearScope(const LinearScope&) = delete;
    LinearScope& operator=(const LinearScope&) = delete;

private:
    LinearAllocator& m_alloc;
    usize m_saved_offset;
};
} // namespace crd::memory
