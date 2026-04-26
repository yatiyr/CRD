#pragma once

#include <crd/memory/allocator.hpp>

namespace crd::memory
{
// A linear allocator that supports nested LIFO rollback via Markers.
//
//   StackAllocator s(64 * 1024);
//   auto m1 = s.mark();
//   void* a = s.allocate(100);
//   void* b = s.allocate(200);
//   auto m2 = s.mark();
//   void* c = s.allocate(50);
//   s.reset_to(m2); // c is gone
//   s.reset_to(m1); // a, b are also gone
//
// Use cases:
//  - Recursive parsing / build steps where each level needs scratch.
//  - Frame-graph compilation, scene-graph traversal scratch buffers.
//
// Rules:
//  - Markers MUST be released in reverse order (LIFO). Releasing an
//    older marker silently discards everything between, including any
//    later markers. Wrong-order release is asserted in debug builds.
//  - `deallocate()` is a no-op (use markers instead).
//  - Not thread-safe.
class StackAllocator : public IAllocator
{
public:
    // A marker is just an offset; we wrap it in a struct so callers can't
    // accidentally mix markers from different StackAllocators (in debug).
    struct Marker
    {
        usize offset = 0;
#if defined(CRD_DEBUG)
        const StackAllocator* owner = nullptr;
#endif
    };

    // Owning ctor: allocates `capacity` bytes from `parent`.
    explicit StackAllocator(usize capacity, IAllocator* parent = nullptr, const char* name = "StackAllocator");

    // Non-owning ctor: takes a pre-allocated buffer.
    StackAllocator(void* buffer, usize capacity, const char* name = "StackAllocator") noexcept;

    ~StackAllocator() override;

    StackAllocator(const StackAllocator&) = delete;
    StackAllocator& operator=(const StackAllocator&) = delete;

    // ---- IAllocator -----------------------------------------------
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    void deallocate(void* p) noexcept override; // no-op
    bool owns(const void* p) const noexcept override;

    // ---- StackAllocator extras ------------------------------------
    Marker mark() const noexcept;
    void reset_to(Marker m) noexcept;
    void reset() noexcept;

    usize used() const noexcept { return m_offset; }
    usize capacity() const noexcept { return m_capacity; }
    usize remaining() const noexcept { return m_capacity - m_offset; }

private:
    IAllocator* m_parent = nullptr;
    u8* m_buffer = nullptr;
    usize m_capacity = 0;
    usize m_offset = 0;
};

// RAII helper that marks on construction and reset_to on destruction.
class StackScope
{
public:
    explicit StackScope(StackAllocator& alloc) noexcept : m_alloc(alloc), m_marker(alloc.mark()) {}
    ~StackScope() noexcept { m_alloc.reset_to(m_marker); }

    StackScope(const StackScope&) = delete;
    StackScope& operator=(const StackScope&) = delete;

private:
    StackAllocator& m_alloc;
    StackAllocator::Marker m_marker;
};
} // namespace crd::memory
