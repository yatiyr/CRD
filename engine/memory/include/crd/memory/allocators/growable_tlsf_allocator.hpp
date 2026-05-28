#pragma once

#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

namespace crd::memory
{
// GrowableTlsfAllocator — general-purpose, UNBOUNDED pooled allocator.
//
// Wraps a chain of TlsfAllocator chunks (each ≤ ~4 GB, the single-pool TLSF cap)
// and adds a new chunk from the parent allocator on demand when no existing
// chunk can serve a request. O(1) amortized allocate within a chunk; deallocate
// dispatches to the owning chunk via TlsfAllocator::owns.
//
// This is the pooled, NON-MallocAllocator option for consumers whose working set
// exceeds a single 4 GB TLSF pool (large sparse benches, cookers, batch tools) —
// the same role `default_allocator()` (malloc) used to fill, but as a real pooled
// allocator. The parent supplies each chunk's backing memory exactly as a bare
// TlsfAllocator does (default: the process system allocator); the consumer never
// holds a MallocAllocator itself.
//
// NOT thread-safe (per the IAllocator convention) — one instance per thread/arena.
//
// VM-backed (malloc-free) heap (ADR-0085 S3): pass a `VirtualMemoryAllocator*` as
// `parent` and every chunk (both the Chunk node and its TLSF pool) is carved from
// that VM reservation instead of malloc — the open-world malloc-free general heap
// with stable addresses. Notes when the parent is a VM arena:
//   - LIFETIME: the VM arena MUST outlive this allocator. Declare the VM arena
//     first and the GrowableTlsfAllocator second so destruction is reverse order.
//   - COMMIT GRANULARITY: each chunk-grow commits `chunk_bytes` of physical memory
//     up front (a VM `allocate` commits the whole request). Lower `chunk_bytes` for
//     finer-grained commit; raise it for fewer syscalls. (A malloc parent instead
//     lazy-faults pages via the OS.)
//   - GROW-MOSTLY: a VM bump-arena's `deallocate` is a no-op, so freed chunks do
//     not return their address range to the arena — acceptable for a long-lived
//     heap; the arena reclaims everything when it is reset/released.
//   - GRACEFUL grow: a chunk-grow asks the parent via `parent->try_allocate`, so
//     `try_allocate` returns nullptr (not a fatal) end-to-end when the VM
//     reservation is exhausted — the composition honors the non-throwing contract.
//
// Single-allocation bound: one allocation must fit in one chunk, so the largest
// servable single allocation is `max_chunk_bytes()` (just under 4 GB). Total live
// memory across chunks is unbounded.
class GrowableTlsfAllocator final : public IAllocator
{
public:
    // chunk_bytes: nominal size of each grown chunk (clamped to
    //   [TlsfAllocator::min_pool_size(), max_chunk_bytes()]). A request larger
    //   than the nominal chunk grows a bespoke chunk sized to fit it.
    explicit GrowableTlsfAllocator(usize chunk_bytes = usize{256} << 20, IAllocator* parent = nullptr,
                                   const char* name = "GrowableTlsfAllocator") noexcept;
    ~GrowableTlsfAllocator() override;

    GrowableTlsfAllocator(const GrowableTlsfAllocator&)            = delete;
    GrowableTlsfAllocator& operator=(const GrowableTlsfAllocator&) = delete;

    // ---- IAllocator -----------------------------------------------------
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    void deallocate(void* p) noexcept override;
    [[nodiscard]] bool owns(const void* p) const noexcept override;
    void* reallocate(void* p, usize old_size, usize new_size, usize alignment = kDefaultAlignment) override;
    [[nodiscard]] usize allocation_size(const void* p) const noexcept override;

    // ---- Non-throwing path ---------------------------------------------
    // Returns nullptr on size==0, when a single allocation exceeds the per-chunk
    // cap, or when the parent cannot satisfy a chunk-grow. Grows a chunk if
    // needed — growth uses parent->try_allocate, so a fatal-on-OOM parent like
    // VirtualMemoryAllocator yields nullptr here (not a fatal) on exhaustion.
    [[nodiscard]] void* try_allocate(usize size, usize alignment = kDefaultAlignment) override;

    // ---- Diagnostics ----------------------------------------------------
    [[nodiscard]] usize num_chunks() const noexcept { return m_num_chunks; }
    [[nodiscard]] static usize max_chunk_bytes() noexcept;

private:
    struct Chunk;
    Chunk* grow(usize min_bytes);

    IAllocator* m_parent;
    usize       m_chunk_bytes;
    Chunk*      m_head       = nullptr; // newest first (most likely to have space)
    usize       m_num_chunks = 0;
};
} // namespace crd::memory
