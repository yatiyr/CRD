#pragma once

#include <crd/core/types.hpp>

namespace crd::memory
{
class IAllocator;

// OffsetAllocator — O(1) hard-real-time offset allocator with EXTERNAL metadata.
//
// Manages a virtual span [0, capacity) of byte OFFSETS — it never touches the
// memory it describes. All bookkeeping (free-list nodes, bin bitmaps) lives in a
// side array, NOT inside the managed span. That is the property that makes it the
// right kernel for GPU device-memory suballocation (you cannot cheaply write a
// free-list header into device-local VRAM the way TLSF does for CPU heaps), and it
// is equally usable for any CPU offset-partitioning problem.
//
// Algorithm: Sebastian Aaltonen's OffsetAllocator (2023, MIT — sebbbi/OffsetAllocator),
// reimplemented in Cerid idiom. 256 size-bins via an 8-bit float distribution
// (3-bit mantissa + 5-bit exponent), a two-level bitmap found with two count-
// trailing-zero ops -> O(1) allocate + free, with neighbour coalescing on free and
// bounded internal fragmentation (<= 12.5%, ~6.25% average) vs power-of-two's +100%.
//
// 32-bit offsets/sizes: the managed span is at most 4 GiB (matches the reference;
// GPU memory blocks are far smaller). `max_allocations` caps live allocations.
//
// NOT thread-safe (IAllocator convention) — the caller serializes (e.g. the GPU
// allocator guards it with the same mutex that protects its block pools).
class OffsetAllocator final
{
public:
    // Returned by allocate(); `offset` == kAllocationFailed on out-of-space.
    static constexpr u32 kAllocationFailed = 0xFFFFFFFFU;

    struct Allocation
    {
        u32 offset   = kAllocationFailed; // aligned byte offset into the span (for the caller)
        u32 metadata = kAllocationFailed; // opaque node index — pass back to free()
        [[nodiscard]] bool valid() const noexcept { return offset != kAllocationFailed; }
    };

    struct StorageReport
    {
        u32 total_free_space     = 0; // sum of all free regions
        u32 largest_free_region  = 0; // biggest single contiguous free region
    };

    // capacity: span size in bytes (1..4 GiB). max_allocations: max simultaneous
    // live allocations (also bounds free-region fragmentation). alloc: source for
    // the two side arrays (nullptr -> default_allocator()).
    explicit OffsetAllocator(u32 capacity, u32 max_allocations = 128U * 1024U, IAllocator* alloc = nullptr,
                             const char* name = "OffsetAllocator");
    ~OffsetAllocator();

    OffsetAllocator(const OffsetAllocator&)            = delete;
    OffsetAllocator& operator=(const OffsetAllocator&) = delete;

    // Allocate `size` bytes at >= `alignment` (power of two). Returns an invalid
    // Allocation when no free region fits or the node pool is exhausted. O(1).
    [[nodiscard]] Allocation allocate(u32 size, u32 alignment = 1) noexcept;

    // Free a prior allocation (coalesces with free neighbours). O(1).
    void free(Allocation allocation) noexcept;

    // Drop everything back to one free region spanning [0, capacity). O(bins).
    void reset() noexcept;

    [[nodiscard]] StorageReport storage_report() const noexcept;
    [[nodiscard]] u32           capacity() const noexcept { return m_capacity; }
    [[nodiscard]] u32           free_storage() const noexcept { return m_free_storage; }
    [[nodiscard]] const char*   name() const noexcept { return m_name; }

private:
    struct Node;
    static constexpr u32 kUnused = 0xFFFFFFFFU;

    [[nodiscard]] u32 insert_node_into_bin(u32 size, u32 data_offset) noexcept;
    void              remove_node_from_bin(u32 node_index) noexcept;

    IAllocator* m_alloc;
    const char* m_name;
    u32         m_capacity;
    u32         m_max_allocs;
    u32         m_free_storage = 0;

    u32 m_used_bins_top = 0;     // bit i set iff top-bin i has a non-empty leaf
    u8  m_used_bins[32] = {};    // m_used_bins[t] bit l set iff leaf-bin (t*8+l) non-empty
    u32 m_bin_indices[256] = {}; // head node index per leaf bin (kUnused = empty)

    Node* m_nodes      = nullptr; // [m_max_allocs] side metadata
    u32*  m_free_nodes = nullptr; // [m_max_allocs] stack of recycled node indices
    u32   m_free_offset = 0;      // top of the free-node stack
};
} // namespace crd::memory
