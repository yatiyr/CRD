#pragma once

#include <crd/memory/allocator.hpp>

namespace crd::memory
{
// TlsfAllocator — Two-Level Segregated Fit allocator (Masmano et al. 2008).
//
// O(1) allocate, deallocate, and coalesce. Bounded internal fragmentation.
// The canonical real-time allocator and the standard choice when sub-frame
// budget guarantees matter.
//
// Pool layout (per instance):
//   [sentinel "last" block | free block #0 | sentinel "first" block]
//                            ↑
//                            user pool grows from here as splits happen.
//
// Free blocks are classified by (FL, SL):
//   FL = floor(log2(size))   — first-level size class (power-of-two range)
//   SL = linear sub-class within the FL range (kSlIndexCount sub-classes)
// Two bitmaps + a 2-D free-list array let us find any suitable block in
// O(1) via std::countr_zero on the bitmaps.
//
// Allocation strategy:
//   1. Round size up to align of 16 + alignment-padding allowance.
//   2. mapping_search → (fl, sl). search_suitable_block via bitmaps.
//   3. Remove from free list, split if remainder ≥ kBlockMinSize.
//   4. Mark in-use; clear `prev_free` on the next physical block.
//   5. Compute aligned payload (split off leading gap as a free block when
//      alignment > kAlignSize).
//
// Deallocate strategy:
//   1. Mark free.
//   2. Coalesce with previous physical block if it's free.
//   3. Coalesce with next physical block if it's free.
//   4. Insert merged block into the appropriate free list.
//   5. Set `prev_free` on the next physical block.
//
// Tuning constants (canonical Conte parameters for 64-bit + 16-byte align):
//   kAlignSizeLog2     = 4    → kAlignSize = 16 (matches kDefaultAlignment)
//   kSlIndexLog2       = 5    → kSlIndexCount = 32 sub-classes per FL
//   kFlIndexShift      = 9    → kSmallBlockSize = 512 (small-block boundary)
//   kFlIndexMax        = 32   → max pool size = 2^32 = 4 GB
//   kFlIndexCount      = 24   → 24 first-level classes
//   Metadata footprint ≈ 7 KB per allocator instance.
//
// Block header (16 bytes per allocated block; 32-byte minimum free block):
//   [prev_phys_block | size_and_flags | (free: next_free | prev_free)]
//   ── 8 bytes ─────────8 bytes ──────────┴── overlaid with user payload ──┘
//
// Capabilities (production-grade as of 2026-05-07 D-001-a fix):
//   - Pool sizes up to 4 GB.
//   - Arbitrary power-of-two alignment, tested up to 256 bytes under ASan
//     stress (1000-iteration mixed-alignment random alloc/free).
//   - try_allocate() non-throwing path returning nullptr on OOM.
//   - reallocate() with in-place grow + alloc-copy fallback.
//
// Deferred enhancements (see docs/debt.md):
//   - Single-threaded by project convention (see IAllocator base class).
//   - 64-bit only — Cerid CI is 64-bit; no 32-bit consumer.
//   - block_header_overhead = 16 (Conte's 8-byte overlap trick saves
//     marginal memory at engine scale; deferred to debt).
class TlsfAllocator : public IAllocator
{
public:
    // Owning ctor. Allocates `capacity` bytes from `parent` and manages it
    // as one TLSF heap. `capacity` must be ≥ kMinPoolSize (a few KB header +
    // a usable free block); CRD_FATAL otherwise.
    explicit TlsfAllocator(usize capacity, IAllocator* parent = nullptr, const char* name = "TlsfAllocator") noexcept;

    // Non-owning ctor. `buffer` must be ≥ kMinPoolSize bytes and satisfy
    // alignof(void*). The buffer's contents are clobbered (overwritten with
    // the TLSF block header chain).
    TlsfAllocator(void* buffer, usize capacity, const char* name = "TlsfAllocator") noexcept;

    ~TlsfAllocator() override;

    TlsfAllocator(const TlsfAllocator&) = delete;
    TlsfAllocator& operator=(const TlsfAllocator&) = delete;

    // ---- IAllocator -----------------------------------------------------
    // allocate is fatal-on-OOM (per IAllocator contract). Use try_allocate
    // for the non-throwing path that returns nullptr instead.
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    void deallocate(void* p) noexcept override;
    [[nodiscard]] bool owns(const void* p) const noexcept override;
    void* reallocate(void* p, usize old_size, usize new_size, usize alignment = kDefaultAlignment) override;
    [[nodiscard]] usize allocation_size(const void* p) const noexcept override;

    // ---- Non-throwing allocator path -----------------------------------
    // Returns nullptr on out-of-memory or size==0. Use this when the caller
    // wants to handle pool exhaustion gracefully (sub-budget allocators,
    // try-fallback patterns).
    [[nodiscard]] void* try_allocate(usize size, usize alignment = kDefaultAlignment) override;

    // ---- TlsfAllocator extras -------------------------------------------
    [[nodiscard]] usize pool_capacity() const noexcept { return m_pool_capacity; }
    [[nodiscard]] const void* pool_base() const noexcept { return m_pool; }

    // The smallest pool size the allocator can be constructed with. A pool
    // smaller than this would have no usable free block.
    [[nodiscard]] static usize min_pool_size() noexcept;

private:
    void init_pool(void* buffer, usize capacity) noexcept;
    void destroy_pool() noexcept;

    IAllocator* m_parent;  // non-null iff we own the buffer (free in dtor)
    void* m_pool;          // start of the user pool (after header sentinel)
    usize m_pool_capacity; // total bytes managed (including sentinel headers)

    // FL / SL bitmap + free-list state. Sized to the canonical constants —
    // see the impl file for the exact values + their derivation.
    u32 m_fl_bitmap;
    u32 m_sl_bitmap[24];        // [kFlIndexCount]
    void* m_free_lists[24][32]; // [kFlIndexCount][kSlIndexCount] — BlockHeader*
};

} // namespace crd::memory
