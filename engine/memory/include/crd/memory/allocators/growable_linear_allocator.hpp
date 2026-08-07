#pragma once

#include <crd/memory/allocator.hpp>

namespace crd::memory
{
// A GROWABLE bump / arena allocator — the growable sibling of LinearAllocator (as GrowablePoolAllocator is to
// PoolAllocator). `allocate()` bumps an offset within the current chunk and, when a chunk is full, slabs another
// chunk from the parent; an allocation larger than the chunk size gets its own right-sized chunk. `deallocate()`
// is a NO-OP. `reset()` rewinds ALL chunks in O(chunks) for reuse (the memory is kept, not returned). The
// destructor frees every chunk back to the parent.
//
// Use cases: unbounded build-time / IR / graph / bake construction where a fixed-capacity LinearAllocator would
// overflow and a fixed-block PoolAllocator cannot fit variable sizes. Constructing a large graph hits the parent
// only once per chunk (~thousands of small nodes), not per allocation — a first chunk is reserved at construction
// so a modest working set never touches the parent after that.
//
// Not thread-safe. One per thread, or wrap externally (ThreadSafeAllocator).
class GrowableLinearAllocator : public IAllocator
{
public:
    // Owning ctor: each chunk is `chunk_bytes` slabbed from `parent` (default allocator if null). A first chunk is
    // reserved immediately so the common small working set never re-hits the parent.
    explicit GrowableLinearAllocator(usize chunk_bytes = 64U * 1024U, IAllocator* parent = nullptr,
                                     const char* name = "GrowableLinearAllocator");

    ~GrowableLinearAllocator() override;

    GrowableLinearAllocator(const GrowableLinearAllocator&)            = delete;
    GrowableLinearAllocator& operator=(const GrowableLinearAllocator&) = delete;

    // ---- IAllocator -----------------------------------------------
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    void  deallocate(void* p) noexcept override; // no-op
    bool  owns(const void* p) const noexcept override;
    // Arena reallocate: allocate fresh + copy; the old slice leaks into the arena by design (deallocate is a no-op).
    void* reallocate(void* p, usize old_size, usize new_size, usize alignment = kDefaultAlignment) override;

    // ---- GrowableLinearAllocator extras ---------------------------
    // Mark all chunks free again for reuse (memory kept, not freed). O(chunks). Pointers handed out before reset
    // are invalidated immediately.
    void reset() noexcept;

    [[nodiscard]] usize num_chunks() const noexcept;
    [[nodiscard]] usize bytes_reserved() const noexcept; // total chunk capacity (incl. headers)
    [[nodiscard]] usize bytes_used() const noexcept;      // total bump used across chunks (excl. headers)
    [[nodiscard]] IAllocator* parent() const noexcept { return m_parent; }

private:
    struct Chunk
    {
        void*  base = nullptr;
        usize  cap  = 0;
        usize  off  = 0;
        Chunk* next = nullptr;
    };

    bool grow(usize need); // append a chunk sized to fit `need` (+ header); false on OOM

    IAllocator* m_parent      = nullptr;
    Chunk*      m_first        = nullptr; // append order (for reset / owns / stats)
    Chunk*      m_last         = nullptr;
    Chunk*      m_current      = nullptr; // the bump cursor
    usize       m_chunk_bytes  = 0;
    usize       m_header_size  = 0;
};
} // namespace crd::memory
