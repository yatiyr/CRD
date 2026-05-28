#include <crd/core/assert.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <cstring>

namespace crd::memory
{
// ---- TLSF tuning constants ------------------------------------------------
// Canonical Matt Conte / Masmano values for 64-bit + 16-byte alignment.
namespace
{
constexpr usize kAlignSizeLog2 = 4;
constexpr usize kAlignSize = usize{1} << kAlignSizeLog2; // 16

constexpr usize kSlIndexLog2 = 5;
constexpr usize kSlIndexCount = usize{1} << kSlIndexLog2; // 32

constexpr usize kFlIndexShift = kSlIndexLog2 + kAlignSizeLog2; // 9
constexpr usize kSmallBlockSize = usize{1} << kFlIndexShift;   // 512

// kFlIndexMax = 32 (max pool size 2^32 = 4 GB) is documented but not directly
// referenced — the FL/SL math implicitly bounds size via the bitmap widths.

// ---- Block header ---------------------------------------------------------
// 16-byte overhead per allocated block. Min free block size is 32 (must hold
// next_free + prev_free pointers in addition to the header).
//
//   [prev_phys_block (8) | size_and_flags (8) | user payload | next_free (8) prev_free (8) when free]
//
// The two low bits of `size_and_flags` are flags:
//   bit 0: this block is free
//   bit 1: previous physical block is free
struct BlockHeader
{
    BlockHeader* prev_phys_block;
    usize size_and_flags;
    BlockHeader* next_free;
    BlockHeader* prev_free;
};

constexpr usize kBlockHeaderOverhead = sizeof(BlockHeader::prev_phys_block) + sizeof(BlockHeader::size_and_flags); // 16
constexpr usize kBlockMinSize = sizeof(BlockHeader);                      // 32 (free min)
constexpr usize kBlockMinUserSize = kBlockMinSize - kBlockHeaderOverhead; // 16

constexpr usize kFreeBit = usize{1};
constexpr usize kPrevFreeBit = usize{2};
constexpr usize kSizeMask = ~(kFreeBit | kPrevFreeBit);

// ---- Block accessors ------------------------------------------------------

inline usize block_size(const BlockHeader* b) noexcept
{
    return b->size_and_flags & kSizeMask;
}

inline bool block_is_free(const BlockHeader* b) noexcept
{
    return (b->size_and_flags & kFreeBit) != 0;
}

inline bool block_prev_is_free(const BlockHeader* b) noexcept
{
    return (b->size_and_flags & kPrevFreeBit) != 0;
}

inline void block_set_size(BlockHeader* b, usize size) noexcept
{
    b->size_and_flags = (b->size_and_flags & ~kSizeMask) | (size & kSizeMask);
}

inline void block_set_free(BlockHeader* b) noexcept
{
    b->size_and_flags |= kFreeBit;
}

inline void block_set_used(BlockHeader* b) noexcept
{
    b->size_and_flags &= ~kFreeBit;
}

inline void block_set_prev_free(BlockHeader* b) noexcept
{
    b->size_and_flags |= kPrevFreeBit;
}

inline void block_set_prev_used(BlockHeader* b) noexcept
{
    b->size_and_flags &= ~kPrevFreeBit;
}

inline u8* block_payload(BlockHeader* b) noexcept
{
    return reinterpret_cast<u8*>(b) + kBlockHeaderOverhead;
}

inline BlockHeader* block_from_payload(void* p) noexcept
{
    return reinterpret_cast<BlockHeader*>(static_cast<u8*>(p) - kBlockHeaderOverhead);
}

inline BlockHeader* block_next(BlockHeader* b) noexcept
{
    return reinterpret_cast<BlockHeader*>(reinterpret_cast<u8*>(b) + kBlockHeaderOverhead + block_size(b));
}

inline BlockHeader* block_prev(BlockHeader* b) noexcept
{
    return b->prev_phys_block;
}

// ---- TLSF mapping (size → fl, sl) ----------------------------------------

inline u32 fls_size(usize size) noexcept
{
    // floor(log2(size)). Undefined for size == 0; caller must ensure size > 0.
    return 63U - static_cast<u32>(std::countl_zero(static_cast<unsigned long long>(size)));
}

inline void mapping_insert(usize size, u32& fl, u32& sl) noexcept
{
    if (size < kSmallBlockSize)
    {
        fl = 0;
        sl = static_cast<u32>(size / (kSmallBlockSize / kSlIndexCount));
    }
    else
    {
        fl = fls_size(size);
        sl = static_cast<u32>((size >> (fl - kSlIndexLog2)) - kSlIndexCount);
        fl -= static_cast<u32>(kFlIndexShift - 1);
    }
}

inline void mapping_search(usize size, u32& fl, u32& sl) noexcept
{
    if (size >= kSmallBlockSize)
    {
        // Round up so the resulting block is at least `size` bytes.
        const usize round = (usize{1} << (fls_size(size) - kSlIndexLog2)) - 1;
        size += round;
    }
    mapping_insert(size, fl, sl);
}

} // namespace

// ---- Construction ---------------------------------------------------------

usize TlsfAllocator::min_pool_size() noexcept
{
    // Three block headers (start sentinel, free block, end sentinel) + minimum
    // usable free payload.
    return 3U * kBlockHeaderOverhead + kBlockMinSize;
}

TlsfAllocator::TlsfAllocator(usize capacity, IAllocator* parent, const char* name) noexcept
    : m_parent(parent != nullptr ? parent : default_allocator()), m_pool(nullptr), m_pool_capacity(0), m_fl_bitmap(0),
      m_sl_bitmap{}, m_free_lists{}
{
    m_name = name;
    CRD_ASSERT(capacity >= min_pool_size());
    // NOLINTNEXTLINE(readability-suspicious-call-argument) — (size, alignment) is correct.
    void* buffer = m_parent->allocate(capacity, kAlignSize);
    init_pool(buffer, capacity);
}

TlsfAllocator::TlsfAllocator(void* buffer, usize capacity, const char* name) noexcept
    : m_parent(nullptr), m_pool(nullptr), m_pool_capacity(0), m_fl_bitmap(0), m_sl_bitmap{}, m_free_lists{}
{
    m_name = name;
    CRD_ASSERT(buffer != nullptr);
    CRD_ASSERT(capacity >= min_pool_size());
    CRD_ASSERT(is_aligned(buffer, kAlignSize));
    init_pool(buffer, capacity);
}

TlsfAllocator::~TlsfAllocator()
{
    if (m_parent != nullptr && m_pool != nullptr)
    {
        // m_pool points kBlockHeaderOverhead bytes into the original buffer,
        // because the start sentinel sits before it. Walk back to free.
        u8* original = static_cast<u8*>(m_pool) - kBlockHeaderOverhead;
        m_parent->deallocate(original);
    }
}

void TlsfAllocator::init_pool(void* buffer, usize capacity) noexcept
{
    // Layout: [start sentinel header (16 B)] [free block (capacity - 32 B)] [end sentinel header (16 B)]
    //
    // The start sentinel is a zero-size USED block. Its purpose is to give
    // the first usable block a stable `prev_phys_block` pointer (so coalesce
    // logic always finds something behind).
    //
    // The end sentinel is a zero-size USED block. block_is_last() returns
    // true for any block whose size is 0; we use that to halt forward walks.

    u8* base = static_cast<u8*>(buffer);

    BlockHeader* start_sentinel = reinterpret_cast<BlockHeader*>(base);
    start_sentinel->prev_phys_block = nullptr;
    start_sentinel->size_and_flags = 0; // size=0, in-use, prev not free

    // Pool layout: [start_sent (16 B) | free_block hdr (16 B) | free payload | end_sent (16 B)]
    // Three block headers consume 48 bytes total; the rest is the initial free payload.
    const usize free_block_size = capacity - 3U * kBlockHeaderOverhead;
    CRD_ASSERT((free_block_size & ~kSizeMask) == 0); // size has the bottom 2 bits available for flags
    CRD_ASSERT(free_block_size >= kBlockMinSize);

    BlockHeader* free_block = reinterpret_cast<BlockHeader*>(base + kBlockHeaderOverhead);
    free_block->prev_phys_block = start_sentinel;
    free_block->size_and_flags = free_block_size | kFreeBit;
    free_block->next_free = nullptr;
    free_block->prev_free = nullptr;

    BlockHeader* end_sentinel = reinterpret_cast<BlockHeader*>(base + kBlockHeaderOverhead + free_block_size);
    end_sentinel->prev_phys_block = free_block;
    end_sentinel->size_and_flags = 0 | kPrevFreeBit; // size=0 (last), in-use, prev_free=true

    m_pool = free_block; // user pool conceptually starts here
    m_pool_capacity = capacity;

    // Insert the single big free block into the free list.
    u32 fl = 0;
    u32 sl = 0;
    mapping_insert(free_block_size, fl, sl);

    // Inline insert (no this-> deref to keep init order simple):
    free_block->next_free = nullptr;
    free_block->prev_free = nullptr;
    m_free_lists[fl][sl] = free_block;
    m_fl_bitmap |= (u32{1} << fl);
    m_sl_bitmap[fl] |= (u32{1} << sl);
}

void TlsfAllocator::destroy_pool() noexcept
{
    // Nothing to do here yet — pool memory is managed by parent in dtor or
    // by the non-owning caller.
}

// ---- Free-list ops --------------------------------------------------------

namespace
{
void list_insert(BlockHeader* block, BlockHeader*& head) noexcept
{
    block->prev_free = nullptr;
    block->next_free = head;
    if (head != nullptr)
    {
        head->prev_free = block;
    }
    head = block;
}

void list_remove(BlockHeader* block, BlockHeader*& head) noexcept
{
    BlockHeader* prev = block->prev_free;
    BlockHeader* next = block->next_free;
    if (next != nullptr)
    {
        next->prev_free = prev;
    }
    if (prev != nullptr)
    {
        prev->next_free = next;
    }
    if (head == block)
    {
        head = next;
    }
}
} // namespace

// ---- Find / insert / remove free block -----------------------------------

namespace detail
{
BlockHeader* find_suitable_block(u32 fl_bitmap, const u32* sl_bitmap, void* const free_lists[24][32], u32& fl,
                                 u32& sl) noexcept
{
    // First, look in the same fl class for an SL >= sl.
    u32 sl_map = sl_bitmap[fl] & (~0U << sl);
    if (sl_map == 0U)
    {
        // No fit in this fl. Look at higher fl classes.
        const u32 fl_map = fl_bitmap & (~0U << (fl + 1U));
        if (fl_map == 0U)
        {
            return nullptr; // OOM
        }
        fl = static_cast<u32>(std::countr_zero(fl_map));
        sl_map = sl_bitmap[fl];
    }
    sl = static_cast<u32>(std::countr_zero(sl_map));
    return static_cast<BlockHeader*>(free_lists[fl][sl]);
}
} // namespace detail

namespace
{
inline void insert_free_block_helper(BlockHeader* block, u32& fl_bitmap, u32* sl_bitmap, void* free_lists[24][32],
                                     u32 fl, u32 sl) noexcept
{
    BlockHeader*& head = reinterpret_cast<BlockHeader*&>(free_lists[fl][sl]);
    list_insert(block, head);
    fl_bitmap |= (u32{1} << fl);
    sl_bitmap[fl] |= (u32{1} << sl);
}

inline void remove_free_block_helper(BlockHeader* block, u32& fl_bitmap, u32* sl_bitmap, void* free_lists[24][32],
                                     u32 fl, u32 sl) noexcept
{
    BlockHeader*& head = reinterpret_cast<BlockHeader*&>(free_lists[fl][sl]);
    list_remove(block, head);
    if (head == nullptr)
    {
        sl_bitmap[fl] &= ~(u32{1} << sl);
        if (sl_bitmap[fl] == 0U)
        {
            fl_bitmap &= ~(u32{1} << fl);
        }
    }
}

// Trim a free block by `gap` bytes from its leading edge. Returns the new
// (in-use) block at (block + gap); the leading bytes [block, block+gap) become
// a free block re-inserted into the free list.
//
// Preconditions:
//   - `block` is OUT of all free lists (caller removed it).
//   - `block` has kFreeBit set (we'll re-insert the leading remainder as free).
//   - `gap >= kBlockMinSize` so the leading remainder can host a free block.
//
// Invariants maintained (the previously-buggy ones are pinned by CRD_ASSERT):
//   - leading remainder: free, prev_phys_block preserved, kPrevFreeBit preserved.
//   - new (user) block: in-use, kPrevFreeBit set, prev_phys_block = remainder.
//   - block_next(new_block).prev_phys_block updated to new_block.
//   - block_next(new_block).kPrevFreeBit CLEARED (predecessor is in-use).
//     ← This was the previously-missing flag update that caused the v1 bug:
//     without clearing it, a later deallocate of the after-block would see
//     kPrevFreeBit=true and try to merge with a stale (absorbed) prev.
inline BlockHeader* trim_free_leading(BlockHeader* block, usize gap, u32& fl_bitmap, u32* sl_bitmap,
                                      void* free_lists[24][32]) noexcept
{
    CRD_ASSERT(gap >= kBlockMinSize);
    CRD_ASSERT(block_is_free(block));

    const usize original_size = block_size(block);
    CRD_ASSERT(original_size > gap);

    BlockHeader* new_block = reinterpret_cast<BlockHeader*>(reinterpret_cast<u8*>(block) + gap);

    new_block->prev_phys_block = block;
    new_block->size_and_flags = (original_size - gap) | kPrevFreeBit;

    block_set_size(block, gap - kBlockHeaderOverhead);

    BlockHeader* after_new = block_next(new_block);
    after_new->prev_phys_block = new_block;
    block_set_prev_used(after_new);

    u32 lfl = 0;
    u32 lsl = 0;
    mapping_insert(block_size(block), lfl, lsl);
    insert_free_block_helper(block, fl_bitmap, sl_bitmap, free_lists, lfl, lsl);

    CRD_ASSERT(block_is_free(block));
    CRD_ASSERT(block_size(block) == gap - kBlockHeaderOverhead);
    CRD_ASSERT(!block_is_free(new_block));
    CRD_ASSERT(block_prev_is_free(new_block));
    CRD_ASSERT(block_size(new_block) == original_size - gap);
    CRD_ASSERT(after_new->prev_phys_block == new_block);
    CRD_ASSERT(!block_prev_is_free(after_new));

    return new_block;
}
} // namespace

// ---- Block split / coalesce ----------------------------------------------

namespace
{
// Split `block` so the first part has user-payload size `size`. The remainder
// is returned as a new block (NOT yet inserted into any free list — caller
// decides). The original `block`'s size is updated.
BlockHeader* block_split(BlockHeader* block, usize size) noexcept
{
    const usize remaining = block_size(block) - size - kBlockHeaderOverhead;
    CRD_ASSERT(remaining >= kBlockMinSize);

    BlockHeader* remainder = reinterpret_cast<BlockHeader*>(reinterpret_cast<u8*>(block) + kBlockHeaderOverhead + size);
    remainder->prev_phys_block = block;
    remainder->size_and_flags = remaining; // free flag set by caller as needed

    block_set_size(block, size);

    // Next physical block's prev_phys_block must point to the remainder now.
    BlockHeader* next = block_next(remainder);
    next->prev_phys_block = remainder;
    return remainder;
}

// Merge `block` with the next physical block (which must be free). Returns the
// merged block (same address as `block`).
BlockHeader* block_merge_next(BlockHeader* block, u32& fl_bitmap, u32* sl_bitmap, void* free_lists[24][32]) noexcept
{
    BlockHeader* next = block_next(block);
    CRD_ASSERT(block_is_free(next));

    u32 fl = 0;
    u32 sl = 0;
    mapping_insert(block_size(next), fl, sl);
    remove_free_block_helper(next, fl_bitmap, sl_bitmap, free_lists, fl, sl);

    const usize merged_size = block_size(block) + kBlockHeaderOverhead + block_size(next);
    block_set_size(block, merged_size);

    // The block beyond `next`'s prev_phys_block must now point to `block`.
    BlockHeader* after = block_next(block);
    after->prev_phys_block = block;
    return block;
}

// Merge `block` with the previous physical block (which must be free). Returns
// the merged block (= the previous block's address).
BlockHeader* block_merge_prev(BlockHeader* block, u32& fl_bitmap, u32* sl_bitmap, void* free_lists[24][32]) noexcept
{
    BlockHeader* prev = block_prev(block);
    CRD_ASSERT(prev != nullptr && block_is_free(prev));

    u32 fl = 0;
    u32 sl = 0;
    mapping_insert(block_size(prev), fl, sl);
    remove_free_block_helper(prev, fl_bitmap, sl_bitmap, free_lists, fl, sl);

    const usize merged_size = block_size(prev) + kBlockHeaderOverhead + block_size(block);
    block_set_size(prev, merged_size);

    BlockHeader* after = block_next(prev);
    after->prev_phys_block = prev;
    return prev;
}
} // namespace

// ---- Allocate / deallocate -----------------------------------------------

void* TlsfAllocator::allocate(usize size, usize alignment)
{
    void* p = try_allocate(size, alignment);
    if (p == nullptr && size != 0)
    {
        CRD_FATAL("TlsfAllocator: out of memory");
    }
    return p;
}

void* TlsfAllocator::try_allocate(usize size, usize alignment)
{
    if (size == 0)
    {
        return nullptr;
    }
    CRD_ASSERT(is_pow2(alignment));

    // Round up size to a multiple of kAlignSize and clamp to min user size.
    usize adjusted = align_up(size, kAlignSize);
    if (adjusted < kBlockMinUserSize)
    {
        adjusted = kBlockMinUserSize;
    }

    // For alignment > kAlignSize we may need a leading-split. Per Conte:
    // request enough room that the worst-case alignment shift still leaves a
    // full free-block-min-size remainder both leading (if split) and trailing
    // (if requested size fits with split).
    const usize gap_minimum = kBlockMinSize; // free block must be ≥ this
    usize requested = adjusted;
    if (alignment > kAlignSize)
    {
        requested = adjusted + alignment + gap_minimum;
    }

    u32 fl = 0;
    u32 sl = 0;
    mapping_search(requested, fl, sl);
    BlockHeader* block = detail::find_suitable_block(m_fl_bitmap, m_sl_bitmap, m_free_lists, fl, sl);
    if (block == nullptr)
    {
        return nullptr;
    }
    remove_free_block_helper(block, m_fl_bitmap, m_sl_bitmap, m_free_lists, fl, sl);

    // Sanity: block is currently free (just removed from list); we maintain
    // that invariant explicitly here so the trim_free_leading path can rely
    // on it.
    CRD_ASSERT(block_is_free(block));

    // ---- Alignment-shift via leading-split -----------------------------
    if (alignment > kAlignSize)
    {
        u8* natural = block_payload(block);
        u8* aligned = static_cast<u8*>(align_up_ptr(natural, alignment));
        usize gap = static_cast<usize>(aligned - natural);

        // If the gap is non-zero but too small to host a free block, advance
        // by alignment until it is large enough.
        if (gap != 0 && gap < gap_minimum)
        {
            const usize gap_remain = gap_minimum - gap;
            const usize offset = (gap_remain > alignment) ? gap_remain : alignment;
            aligned = static_cast<u8*>(align_up_ptr(aligned + offset, alignment));
            gap = static_cast<usize>(aligned - natural);
        }

        if (gap != 0)
        {
            block = trim_free_leading(block, gap, m_fl_bitmap, m_sl_bitmap, m_free_lists);
            // Post-trim invariants:
            //   - returned `block` is in-use (kFreeBit clear), kPrevFreeBit
            //     set (its predecessor is the leading remainder, which is free).
            //   - leading remainder is free, in the free list, prev_phys_block
            //     points at the original block's predecessor.
            //   - block_next(block).prev_phys_block points at the new `block`.
            //   - block_next(block).kPrevFreeBit reflects whatever it had
            //     before the trim — we'll fix it in the trailing/no-split path.
            CRD_ASSERT(!block_is_free(block));
            CRD_ASSERT(block_prev_is_free(block));
            CRD_ASSERT(block_payload(block) == aligned);
        }
    }

    // ---- Trailing split (or no-split) ----------------------------------
    if (block_size(block) >= adjusted + kBlockHeaderOverhead + kBlockMinSize)
    {
        BlockHeader* remainder = block_split(block, adjusted);
        remainder->size_and_flags |= kFreeBit;

        u32 rfl = 0;
        u32 rsl = 0;
        mapping_insert(block_size(remainder), rfl, rsl);
        insert_free_block_helper(remainder, m_fl_bitmap, m_sl_bitmap, m_free_lists, rfl, rsl);

        BlockHeader* after_remainder = block_next(remainder);
        block_set_prev_free(after_remainder); // remainder is free
    }
    else
    {
        // No split — next block's prev is now in-use (the user block).
        BlockHeader* next = block_next(block);
        block_set_prev_used(next);
    }

    block_set_used(block);

    // CHECKPOINT: the user's block is in-use. block_next must reflect this in
    // its kPrevFreeBit. (After trailing-split, next is the remainder which IS
    // free, so kPrevFreeBit set is correct. After no-split, next is the
    // original after-block whose kPrevFreeBit must be CLEARED.)
    CRD_ASSERT(!block_is_free(block));

    m_stats.on_allocate(static_cast<u64>(block_size(block)));
    return block_payload(block);
}

void TlsfAllocator::deallocate(void* p) noexcept
{
    if (p == nullptr)
    {
        return;
    }
    CRD_ASSERT(owns(p));

    // v1: alignment is always <= kAlignSize, so user pointer is exactly
    // block + kBlockHeaderOverhead.
    BlockHeader* block = block_from_payload(p);
    CRD_ASSERT(!block_is_free(block));

    m_stats.on_deallocate(static_cast<u64>(block_size(block)));

    block_set_free(block);

    // Coalesce with previous physical block if free.
    if (block_prev_is_free(block))
    {
        block = block_merge_prev(block, m_fl_bitmap, m_sl_bitmap, m_free_lists);
    }

    // Coalesce with next physical block if free.
    BlockHeader* next = block_next(block);
    if (block_is_free(next))
    {
        block = block_merge_next(block, m_fl_bitmap, m_sl_bitmap, m_free_lists);
    }

    // Insert into free list.
    u32 fl = 0;
    u32 sl = 0;
    mapping_insert(block_size(block), fl, sl);
    insert_free_block_helper(block, m_fl_bitmap, m_sl_bitmap, m_free_lists, fl, sl);

    // Mark the next block's prev_free.
    BlockHeader* after = block_next(block);
    block_set_prev_free(after);
}

bool TlsfAllocator::owns(const void* p) const noexcept
{
    if (p == nullptr)
    {
        return false;
    }
    const u8* base = static_cast<const u8*>(m_pool) - kBlockHeaderOverhead;
    const u8* end = base + m_pool_capacity;
    const u8* pb = static_cast<const u8*>(p);
    return pb >= base && pb < end;
}

void* TlsfAllocator::reallocate(void* p, usize old_size, usize new_size, usize alignment)
{
    if (p == nullptr)
    {
        return allocate(new_size, alignment);
    }
    if (new_size == 0)
    {
        deallocate(p);
        return nullptr;
    }

    // In-place grow: if the next physical block is free and the combined
    // size is enough, merge + split.
    BlockHeader* block = block_from_payload(p);
    const usize current_cap = block_size(block);
    const usize needed = align_up(new_size, kAlignSize);

    if (needed <= current_cap)
    {
        // Shrink in place if there's enough left to split.
        if (current_cap >= needed + kBlockHeaderOverhead + kBlockMinSize)
        {
            BlockHeader* remainder = block_split(block, needed);
            remainder->size_and_flags |= kFreeBit;

            // Coalesce with the block after if free.
            BlockHeader* after_remainder = block_next(remainder);
            if (block_is_free(after_remainder))
            {
                remainder = block_merge_next(remainder, m_fl_bitmap, m_sl_bitmap, m_free_lists);
            }

            u32 rfl = 0;
            u32 rsl = 0;
            mapping_insert(block_size(remainder), rfl, rsl);
            insert_free_block_helper(remainder, m_fl_bitmap, m_sl_bitmap, m_free_lists, rfl, rsl);

            BlockHeader* after = block_next(remainder);
            block_set_prev_free(after);
        }
        return p;
    }

    BlockHeader* next = block_next(block);
    if (block_is_free(next))
    {
        const usize combined = current_cap + kBlockHeaderOverhead + block_size(next);
        if (combined >= needed)
        {
            // Merge + split for in-place grow.
            BlockHeader* merged = block_merge_next(block, m_fl_bitmap, m_sl_bitmap, m_free_lists);
            // After merge, `merged` is large enough.
            if (block_size(merged) >= needed + kBlockHeaderOverhead + kBlockMinSize)
            {
                BlockHeader* remainder = block_split(merged, needed);
                remainder->size_and_flags |= kFreeBit;

                u32 rfl = 0;
                u32 rsl = 0;
                mapping_insert(block_size(remainder), rfl, rsl);
                insert_free_block_helper(remainder, m_fl_bitmap, m_sl_bitmap, m_free_lists, rfl, rsl);

                BlockHeader* after = block_next(remainder);
                block_set_prev_free(after);
            }
            else
            {
                BlockHeader* after = block_next(merged);
                block_set_prev_used(after);
            }
            return p;
        }
    }

    // Fallback: allocate new + copy + free old.
    void* new_p = allocate(new_size, alignment);
    const usize copy = old_size < new_size ? old_size : new_size;
    if (copy > 0)
    {
        std::memcpy(new_p, p, copy);
    }
    deallocate(p);
    return new_p;
}

usize TlsfAllocator::allocation_size(const void* p) const noexcept
{
    if (p == nullptr || !owns(p))
    {
        return 0;
    }
    const BlockHeader* block = reinterpret_cast<const BlockHeader*>(static_cast<const u8*>(p) - kBlockHeaderOverhead);
    return block_size(block);
}

} // namespace crd::memory
