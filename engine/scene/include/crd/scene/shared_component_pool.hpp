#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::scene
{
// Phase 3.0 v1m4b — deduplicated, refcounted byte pool for InheritPolicy::Inherit
// components (ADR-0058 pillar 5 CoW backend).
//
// One SharedComponentPool per component-pool that has one or more shared
// instances. Each pool entry is `entry_size` bytes of component payload.
// Multiple öbek instances can SHARE an entry (refcount += 1 on each share);
// breaking the share (runtime mutation through `World::get_component_mut`)
// decrements the refcount and copies the entry's bytes into the instance's
// owned slot.
//
// Memory model:
//   - Pool storage grows monotonically; freed entries are reused via freelist.
//   - Capacity grows exponentially × 2 starting at 8 slots.
//   - Refcount overflow guarded by CRD_ASSERT. 32-bit refcount handles 4B+
//     instances — adequate for any conceivable Cerid workload.
//
// Move-only — destructor releases the byte buffer through `m_alloc`.
//
// Lifetime: pool outlives the longest-lived sharing instance. Owned by
// SparseSetStorage::Pool (one shared-pool per Inherit-policy component);
// destructor releases storage when the storage backend tears down.
class SharedComponentPool
{
public:
    static constexpr crd::u32 kInvalidIdx = 0xFFFFFFFFU;

    SharedComponentPool(crd::memory::IAllocator* alloc,
                        crd::usize               entry_size,
                        crd::usize               entry_alignment) noexcept;
    ~SharedComponentPool();

    SharedComponentPool(const SharedComponentPool&)            = delete;
    SharedComponentPool& operator=(const SharedComponentPool&) = delete;
    SharedComponentPool(SharedComponentPool&&) noexcept;
    SharedComponentPool& operator=(SharedComponentPool&&) noexcept;

    // Acquire a new pool entry, copy `entry_size` bytes from `src` into it,
    // set refcount to 1. Returns the entry's index. Caller owns no allocation.
    [[nodiscard]] crd::u32 acquire(const void* src);

    // v1m4b3 — content-hash deduplicating acquire. If `content_hash` is
    // already present in the dedup table, retain that entry (refcount += 1)
    // and return its idx. Otherwise acquire a new entry as `acquire`. Caller
    // supplies the hash (typically FNV-1a 64 of the source bytes) so the
    // pool itself doesn't take a hash policy.
    //
    // This is the path that makes Inherit's CoW pay off: many instances
    // calling acquire_or_retain with byte-identical source data share ONE
    // pool entry (refcount = N) rather than N separate entries.
    [[nodiscard]] crd::u32 acquire_or_retain(const void* src, crd::u64 content_hash);

    // Bump refcount on an existing entry. CRD_ASSERTs if idx is invalid or
    // refcount is 0 (i.e. trying to retain a freed entry).
    void retain(crd::u32 idx);

    // Decrement refcount. If 0, push idx to the freelist. The bytes remain
    // physically present until the slot is reused; callers must not read
    // entry_bytes after release.
    void release(crd::u32 idx);

    // Read pointer to bytes at idx. CRD_ASSERTs on invalid idx or 0 refcount.
    [[nodiscard]] const crd::u8* entry_bytes(crd::u32 idx) const;
    [[nodiscard]] crd::u8*       entry_bytes(crd::u32 idx);

    // Refcount of entry at idx. Returns 0 if idx is past the high-water mark
    // (never acquired) or has been released.
    [[nodiscard]] crd::u32 refcount(crd::u32 idx) const noexcept;

    // Number of currently live entries (refcount > 0).
    [[nodiscard]] crd::u32 live_count() const noexcept { return m_live_count; }

    // Number of allocated entry slots (live + freed).
    [[nodiscard]] crd::u32 capacity() const noexcept { return m_capacity; }

    // Bytes per entry.
    [[nodiscard]] crd::usize entry_size() const noexcept { return m_entry_size; }

private:
    void grow(crd::u32 min_capacity);

    crd::memory::IAllocator*              m_alloc;
    crd::usize                            m_entry_size;
    crd::usize                            m_entry_alignment;
    crd::u8*                              m_bytes      = nullptr;
    crd::containers::Array<crd::u32>      m_refcounts;
    crd::containers::Array<crd::u64>      m_entry_hashes;        // parallel to refcounts; 0 when slot is free
    crd::containers::Array<crd::u32>      m_freelist;
    crd::containers::HashMap<crd::u64, crd::u32> m_hash_to_idx;  // content_hash -> idx (live entries only)
    crd::u32                              m_capacity   = 0;
    crd::u32                              m_live_count = 0;
    crd::u32                              m_high_water = 0; // smallest idx never assigned
};

} // namespace crd::scene
