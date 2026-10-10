// crd-fuzz-memory-allocators: stateful operation-sequence fuzzing of the production allocators (DIAG.10a; harness
// contract docs/design/test-instruments.md). The first input byte picks an allocator and its configuration; the rest
// is a bounded stream of operations replayed against the allocator and an independent reference model.
//
// Only contract-valid operations reach a raw allocator, so a failure is an allocator defect, never undefined behaviour
// the harness introduced: allocation goes through try_allocate (TLSF and the growable TLSF are fatal on exhaustion
// through allocate), frees name only live blocks, a TLSF reallocate runs only when a probe shows its fallback cannot
// be the fatal out-of-memory path, ring epochs strictly increase and never exceed K in flight, and a stack marker is
// never older than the top. Malformed requests are part of the contract and are generated on purpose: zero and
// near-SIZE_MAX sizes, sizes past the arena, alignments at the top bit and non-power-of-two alignments must all be
// refused without changing anything. The checked layers take the hostile operations their contract promises to
// diagnose: the DiagnosticAllocator gets double frees, interior and foreign frees, redzone overruns and writes into
// quarantined blocks; the SlotMap gets stale handles, double erases and handles to reused slots.
//
// Oracles, after every operation: live blocks never overlap, every block is aligned, inside its allocator and still
// holds the pattern written into it; owns() and allocation_size() agree with the model; validate_structure() passes
// where the allocator has one; exact-capacity models (pool, linear, stack, offset) predict success and failure; the
// diagnostic layer reports exactly the violation seeded and nothing else. After the stream: everything is released,
// the statistics return to zero where they are tracked, the offset allocator coalesces back to one region, and the
// parent gets every byte back when the allocator is destroyed. The same bytes are replayed twice and must produce the
// same outcome trace (determinism).
#include <crd/fuzz/harness.hpp>

#include <crd/containers/slot_map.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/allocators/growable_linear_allocator.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/offset_allocator.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/allocators/ring_allocator.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/diagnostic_allocator.hpp>
#include <crd/memory/memory_stats.hpp>

#include <cstdint>
#include <cstring>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
namespace mem = crd::memory;

constexpr usize kMaxOps  = 2048U;
constexpr usize kMaxLive = 64U;

enum class Kind : u8
{
    Tlsf,
    Pool,
    Linear,
    Stack,
    GrowableLinear,
    GrowableTlsf,
    GrowablePool,
    Offset,
    Ring,
    Diagnostic,
    SlotMap,
    Count
};

// A cursor over the input: reads past the end yield zero, so a stream cut anywhere (mid-operation included) is a
// valid, shorter stream rather than a harness error.
class Reader
{
public:
    Reader(const u8* data, usize size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] bool done() const noexcept { return m_at >= m_size; }
    u8                 byte() noexcept
    {
        if (m_at >= m_size)
        {
            m_at = m_size + 1U;
            return 0U;
        }
        return m_data[m_at++];
    }
    u16 half() noexcept
    {
        const u16 lo = byte();
        const u16 hi = byte();
        return static_cast<u16>(lo | static_cast<u16>(hi << 8U));
    }
    u32 word() noexcept
    {
        const u32 lo = half();
        const u32 hi = half();
        return lo | (hi << 16U);
    }

private:
    const u8* m_data;
    usize     m_size;
    usize     m_at = 0U;
};

// The parent of every allocator under test: the per-input budget, with every backing block page aligned. Placement
// inside an arena depends on the arena's address modulo the requested alignment, so a parent whose blocks land at
// arbitrary 16-byte boundaries would make the two runs of one input disagree for reasons outside the allocator.
class Parent final : public mem::IAllocator
{
public:
    explicit Parent(usize budget = crd::fuzz::kDefaultBudgetBytes) noexcept : m_inner(budget)
    {
        m_name = "FuzzPageAlignedParent";
    }
    void* allocate(usize size, usize alignment) override
    {
        return m_inner.allocate(size, page_aligned(alignment));
    }
    [[nodiscard]] void* try_allocate(usize size, usize alignment) override
    {
        return m_inner.try_allocate(size, page_aligned(alignment));
    }
    void deallocate(void* p) noexcept override { m_inner.deallocate(p); }
    [[nodiscard]] bool  owns(const void* p) const noexcept override { return m_inner.owns(p); }
    [[nodiscard]] usize allocation_size(const void* p) const noexcept override { return m_inner.allocation_size(p); }
    [[nodiscard]] usize live_bytes() const noexcept { return m_inner.live_bytes(); }

private:
    static usize page_aligned(usize alignment) noexcept { return alignment < 4096U ? 4096U : alignment; }
    crd::fuzz::BudgetAllocator m_inner;
};

// The outcome trace both runs must agree on (FNV-1a over every observable result).
class Trace
{
public:
    void add(u64 value) noexcept
    {
        for (u32 i = 0U; i < 8U; ++i)
        {
            m_hash ^= static_cast<u8>(value >> (8U * i));
            m_hash *= 0x100000001B3ULL;
        }
    }
    [[nodiscard]] u64 hash() const noexcept { return m_hash; }

private:
    u64 m_hash = 0xCBF29CE484222325ULL;
};

// Request sizes, malformed ones included: small, medium, a raw 32-bit length, zero and near-SIZE_MAX.
usize decode_size(Reader& r, usize medium_cap) noexcept
{
    const u8 selector = r.byte();
    switch (selector & 7U)
    {
        case 0U:
        case 1U:
        case 2U:
        case 3U:
            return 1U + r.byte();
        case 4U:
        case 5U:
            return 1U + (static_cast<usize>(r.half()) % (medium_cap == 0U ? 1U : medium_cap));
        case 6U:
            return static_cast<usize>(r.word());
        default:
            return ~usize{0} - r.byte();
    }
}

// Alignments: powers of two up to 4 KiB, the top bit, and (marked by the return flag) a non-power-of-two value.
usize decode_alignment(Reader& r) noexcept
{
    const u8 selector = r.byte();
    if (selector == 0xFFU)
    {
        return usize{1} << (sizeof(usize) * 8U - 1U);
    }
    if (selector == 0xFEU)
    {
        return 24U; // not a power of two: refused by every try_allocate
    }
    return usize{1} << (selector % 13U);
}

struct Block
{
    u8*   p       = nullptr;
    usize size    = 0U;
    u8    pattern = 0U;
    u64   tag     = 0U; // ring: the epoch the claim belongs to; linear/stack: offset of its first byte
};

// The reference model of live blocks shared by every pointer-returning allocator.
class LiveSet
{
public:
    [[nodiscard]] usize count() const noexcept { return m_count; }
    [[nodiscard]] bool  full() const noexcept { return m_count == kMaxLive; }
    [[nodiscard]] Block& at(usize i) noexcept { return m_blocks[i]; }

    // A new block: aligned, non-overlapping with every live block, then stamped with its pattern.
    void add(u8* p, usize size, usize alignment, u8 pattern, u64 tag) noexcept
    {
        CRD_FUZZ_REQUIRE(p != nullptr);
        CRD_FUZZ_REQUIRE((reinterpret_cast<std::uintptr_t>(p) & (alignment - 1U)) == 0U);
        CRD_FUZZ_REQUIRE(!overlaps(p, size, kMaxLive));
        std::memset(p, pattern, size);
        m_blocks[m_count] = Block{p, size, pattern, tag};
        m_count += 1U;
    }

    [[nodiscard]] bool overlaps(const u8* p, usize size, usize skip) const noexcept
    {
        for (usize i = 0U; i < m_count; ++i)
        {
            if (i == skip)
            {
                continue;
            }
            const u8* lo = m_blocks[i].p;
            const u8* hi = lo + m_blocks[i].size;
            if (p < hi && lo < p + size)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static bool intact(const Block& b) noexcept
    {
        for (usize k = 0U; k < b.size; ++k)
        {
            if (b.p[k] != b.pattern)
            {
                return false;
            }
        }
        return true;
    }

    void require_all_intact() const noexcept
    {
        for (usize i = 0U; i < m_count; ++i)
        {
            CRD_FUZZ_REQUIRE(intact(m_blocks[i]));
        }
    }

    // Remove block i (its payload must have survived until now).
    void remove(usize i) noexcept
    {
        CRD_FUZZ_REQUIRE(intact(m_blocks[i]));
        m_blocks[i] = m_blocks[m_count - 1U];
        m_count -= 1U;
    }

    // Drop every block whose tag satisfies the predicate (linear/stack rewinds, ring retirement).
    template <typename Pred> void remove_if(Pred pred) noexcept
    {
        usize i = 0U;
        while (i < m_count)
        {
            if (pred(m_blocks[i]))
            {
                remove(i);
            }
            else
            {
                i += 1U;
            }
        }
    }

    [[nodiscard]] usize pick(Reader& r) const noexcept { return m_count == 0U ? 0U : r.byte() % m_count; }

private:
    Block m_blocks[kMaxLive] = {};
    usize m_count            = 0U;
};

u8 pattern_for(usize op) noexcept
{
    return static_cast<u8>((op * 131U + 7U) & 0xFFU);
}

// A request every allocator must refuse (and the model expects refused) whatever its state.
bool malformed(usize size, usize alignment) noexcept
{
    return size == 0U || !mem::is_pow2(alignment);
}

void require_stats_drained(const mem::IAllocator& a) noexcept
{
#if CRD_MEMORY_STATS_TRACKING
    const mem::MemoryStats::Snapshot s = a.stats().snapshot();
    CRD_FUZZ_REQUIRE(s.bytes_in_use == 0U);
#else
    (void)a;
#endif
}

void require_stats_balanced(const mem::IAllocator& a) noexcept
{
#if CRD_MEMORY_STATS_TRACKING
    const mem::MemoryStats::Snapshot s = a.stats().snapshot();
    CRD_FUZZ_REQUIRE(s.alloc_count == s.dealloc_count);
#else
    (void)a;
#endif
}

// ---- general-purpose heaps: TLSF and the growable TLSF -----------------------------------------------------------

template <typename Heap> bool heap_valid(Heap& heap) noexcept
{
    if constexpr (requires { heap.validate_structure(); })
    {
        return heap.validate_structure();
    }
    else
    {
        return true;
    }
}

template <typename Heap> void run_heap(Heap& heap, Reader& r, Trace& trace, usize medium_cap) noexcept
{
    LiveSet live;
    for (usize op = 0U; op < kMaxOps && !r.done(); ++op)
    {
        const u8 code = static_cast<u8>(r.byte() % 4U);
        if (code == 0U || live.count() == 0U)
        {
            const usize size      = decode_size(r, medium_cap);
            const usize alignment = decode_alignment(r);
            void* const p         = heap.try_allocate(size, alignment);
            trace.add(p != nullptr ? 1U : 0U);
            if (malformed(size, alignment))
            {
                CRD_FUZZ_REQUIRE(p == nullptr);
            }
            if (p != nullptr)
            {
                CRD_FUZZ_REQUIRE(heap.owns(p));
                CRD_FUZZ_REQUIRE(heap.allocation_size(p) >= size);
                if (live.full())
                {
                    heap.deallocate(p);
                }
                else
                {
                    live.add(static_cast<u8*>(p), size, alignment, pattern_for(op), 0U);
                }
            }
        }
        else if (code == 1U)
        {
            const usize i = live.pick(r);
            u8* const   p = live.at(i).p;
            live.remove(i);
            heap.deallocate(p);
        }
        else if (code == 2U)
        {
            // Reallocate within a bounded size. The fallback path of TLSF reallocate is the fatal allocate, so the
            // operation runs only when a probe proves an allocation of the new size can succeed right now.
            const usize i        = live.pick(r);
            const usize new_size = 1U + (static_cast<usize>(r.half()) % (medium_cap == 0U ? 1U : medium_cap));
            void* const probe    = heap.try_allocate(new_size, mem::kDefaultAlignment);
            trace.add(probe != nullptr ? 1U : 0U);
            if (probe != nullptr)
            {
                heap.deallocate(probe);
                Block&      b    = live.at(i);
                const usize keep = b.size < new_size ? b.size : new_size;
                CRD_FUZZ_REQUIRE(LiveSet::intact(b));
                u8* const np = static_cast<u8*>(heap.reallocate(b.p, b.size, new_size, mem::kDefaultAlignment));
                CRD_FUZZ_REQUIRE(np != nullptr);
                for (usize k = 0U; k < keep; ++k)
                {
                    CRD_FUZZ_REQUIRE(np[k] == b.pattern);
                }
                CRD_FUZZ_REQUIRE(heap.owns(np));
                CRD_FUZZ_REQUIRE(heap.allocation_size(np) >= new_size);
                b.p    = np;
                b.size = new_size;
                CRD_FUZZ_REQUIRE((reinterpret_cast<std::uintptr_t>(np) & (mem::kDefaultAlignment - 1U)) == 0U);
                CRD_FUZZ_REQUIRE(!live.overlaps(np, new_size, i));
                std::memset(np, b.pattern, new_size);
            }
        }
        else
        {
            live.require_all_intact();
        }
        CRD_FUZZ_REQUIRE(heap_valid(heap));
    }
    live.require_all_intact();
    while (live.count() > 0U)
    {
        u8* const p = live.at(0U).p;
        live.remove(0U);
        heap.deallocate(p);
        CRD_FUZZ_REQUIRE(heap_valid(heap));
    }
    require_stats_drained(heap);
    require_stats_balanced(heap);
}

void fuzz_tlsf(Reader& r, Trace& trace, Parent& parent) noexcept
{
    const usize capacity = mem::TlsfAllocator::min_pool_size() + static_cast<usize>(r.half()) * 4U;
    mem::TlsfAllocator heap(capacity, &parent, "fuzz-tlsf");
    CRD_FUZZ_REQUIRE(heap.validate_structure());
    run_heap(heap, r, trace, capacity);
}

void fuzz_growable_tlsf(Reader& r, Trace& trace) noexcept
{
    Parent parent(usize{2} << 20U); // chunk growth ends at this parent's budget, not the fatal path
    {
        const usize chunk = static_cast<usize>(r.half()) * 4U; // clamped up to the TLSF floor by the allocator
        mem::GrowableTlsfAllocator heap(chunk, &parent, "fuzz-growable-tlsf");
        run_heap(heap, r, trace, usize{64} << 10U);
        trace.add(heap.num_chunks());
    }
    CRD_FUZZ_REQUIRE(parent.live_bytes() == 0U);
}

// ---- fixed-slot pools ------------------------------------------------------------------------------------------

template <typename Pool> void run_pool(Pool& pool, Reader& r, Trace& trace, usize slot_count_or_zero) noexcept
{
    LiveSet live;
    for (usize op = 0U; op < kMaxOps && !r.done(); ++op)
    {
        const u8 code = static_cast<u8>(r.byte() % 4U);
        if (code == 0U || code == 1U || live.count() == 0U)
        {
            const usize size      = decode_size(r, pool.slot_size() * 2U);
            const usize alignment = decode_alignment(r);
            void* const p         = pool.try_allocate(size, alignment);
            trace.add(p != nullptr ? 1U : 0U);
            const bool fits = !malformed(size, alignment) && size <= pool.slot_size() &&
                              alignment <= pool.slot_alignment();
            if (!fits)
            {
                CRD_FUZZ_REQUIRE(p == nullptr);
            }
            else if (slot_count_or_zero != 0U)
            {
                // A fixed pool is an exact model: a fitting request succeeds iff a slot is free.
                CRD_FUZZ_REQUIRE((p != nullptr) == (live.count() < slot_count_or_zero));
            }
            if (p != nullptr)
            {
                CRD_FUZZ_REQUIRE(pool.owns(p));
                CRD_FUZZ_REQUIRE(pool.allocation_size(p) >= size);
                if (pool.slot_size() > 1U)
                {
                    CRD_FUZZ_REQUIRE(!pool.owns(static_cast<u8*>(p) + 1U)); // an interior pointer is never a slot
                }
                if (live.full())
                {
                    pool.deallocate(p);
                }
                else
                {
                    live.add(static_cast<u8*>(p), size, alignment, pattern_for(op), 0U);
                }
            }
        }
        else if (code == 2U)
        {
            const usize i = live.pick(r);
            u8* const   p = live.at(i).p;
            live.remove(i);
            pool.deallocate(p);
        }
        else
        {
            live.require_all_intact();
        }
        CRD_FUZZ_REQUIRE(pool.slots_in_use() == live.count());
        if constexpr (requires { pool.validate_structure(); })
        {
            CRD_FUZZ_REQUIRE(pool.validate_structure());
        }
    }
    while (live.count() > 0U)
    {
        u8* const p = live.at(0U).p;
        live.remove(0U);
        pool.deallocate(p);
    }
    CRD_FUZZ_REQUIRE(pool.slots_in_use() == 0U);
    require_stats_drained(pool);
    require_stats_balanced(pool);
}

void fuzz_pool(Reader& r, Trace& trace, Parent& parent) noexcept
{
    const usize slot_size  = 8U + (r.byte() % 249U);
    const usize slot_count = 1U + (r.byte() % 96U);
    const usize alignment  = usize{8} << (r.byte() % 4U);
    mem::PoolAllocator pool(slot_size, slot_count, alignment, &parent, "fuzz-pool");
    CRD_FUZZ_REQUIRE(pool.validate_structure());
    run_pool(pool, r, trace, slot_count);
}

void fuzz_growable_pool(Reader& r, Trace& trace) noexcept
{
    Parent parent(usize{256} << 10U);
    {
        const usize slot_size = 8U + (r.byte() % 249U);
        const usize alignment = usize{8} << (r.byte() % 4U);
        const usize per_page  = 1U + (r.byte() % 32U);
        mem::GrowablePoolAllocator pool(slot_size, alignment, per_page, &parent, "fuzz-growable-pool");
        run_pool(pool, r, trace, 0U);
        trace.add(pool.page_count());
    }
    CRD_FUZZ_REQUIRE(parent.live_bytes() == 0U);
}

// ---- bump arenas: linear, stack (markers) and the growable linear ----------------------------------------------

// The exact model of a bump arena over [base, base + capacity) at offset `used`.
bool bump_fits(const u8* base, usize used, usize capacity, usize size, usize alignment, usize* new_used) noexcept
{
    const std::uintptr_t current = reinterpret_cast<std::uintptr_t>(base) + used;
    if (alignment - 1U > ~std::uintptr_t{0} - current)
    {
        return false;
    }
    const std::uintptr_t aligned = (current + (alignment - 1U)) & ~static_cast<std::uintptr_t>(alignment - 1U);
    const usize          start   = used + static_cast<usize>(aligned - current);
    if (start > capacity || size > capacity - start)
    {
        return false;
    }
    *new_used = start + size;
    return true;
}

template <typename Arena> void run_bump(Arena& arena, Reader& r, Trace& trace, bool has_markers) noexcept
{
    LiveSet     live;
    usize       markers[16] = {};
    usize       marker_count = 0U;
    const u8*   base         = nullptr; // learned from the first block: offset 0 is base + (first offset)
    for (usize op = 0U; op < kMaxOps && !r.done(); ++op)
    {
        const u8 code = static_cast<u8>(r.byte() % 6U);
        if (code <= 2U || live.count() == 0U)
        {
            const usize size      = decode_size(r, arena.capacity());
            const usize alignment = decode_alignment(r);
            const usize before    = arena.used();
            void* const p         = arena.try_allocate(size, alignment);
            trace.add(p != nullptr ? 1U : 0U);
            trace.add(arena.used());
            if (malformed(size, alignment))
            {
                CRD_FUZZ_REQUIRE(p == nullptr);
            }
            if (base == nullptr && p != nullptr)
            {
                base = static_cast<const u8*>(p) - (arena.used() - size);
            }
            if (base != nullptr && !malformed(size, alignment))
            {
                usize      expected = 0U;
                const bool fits     = bump_fits(base, before, arena.capacity(), size, alignment, &expected);
                CRD_FUZZ_REQUIRE((p != nullptr) == fits);
                if (fits)
                {
                    CRD_FUZZ_REQUIRE(arena.used() == expected);
                }
            }
            if (p == nullptr)
            {
                CRD_FUZZ_REQUIRE(arena.used() == before); // a refusal leaves the arena untouched
            }
            else
            {
                CRD_FUZZ_REQUIRE(arena.owns(p));
                CRD_FUZZ_REQUIRE(arena.owns(static_cast<u8*>(p) + (size - 1U)));
                if (!live.full())
                {
                    live.add(static_cast<u8*>(p), size, alignment, pattern_for(op), arena.used() - size);
                }
            }
        }
        else if (code == 3U && has_markers)
        {
            if (marker_count < 16U)
            {
                markers[marker_count] = arena.used();
                marker_count += 1U;
            }
        }
        else if (code == 4U && has_markers && marker_count > 0U)
        {
            // Rewind to a marker (popping every newer one): the blocks above it are gone, those below survive.
            const usize m    = r.byte() % marker_count;
            const usize mark = markers[m];
            marker_count     = m;
            // The model drops (and verifies) the popped blocks first: the rewind poisons them.
            live.remove_if([mark](const Block& b) { return b.tag >= mark; });
            if constexpr (requires { arena.reset_to(typename Arena::Marker{}); })
            {
                typename Arena::Marker marker = arena.mark();
                marker.offset                 = mark;
                arena.reset_to(marker);
            }
            else
            {
                arena.reset_to(mark);
            }
            CRD_FUZZ_REQUIRE(arena.used() == mark);
        }
        else if (code == 5U)
        {
            live.remove_if([](const Block&) { return true; });
            arena.reset();
            marker_count = 0U;
            CRD_FUZZ_REQUIRE(arena.used() == 0U);
        }
        live.require_all_intact();
#if CRD_MEMORY_STATS_TRACKING
        CRD_FUZZ_REQUIRE(arena.stats().snapshot().bytes_in_use == arena.used());
#endif
    }
    live.remove_if([](const Block&) { return true; });
    arena.reset();
    require_stats_drained(arena);
}

// The linear allocator's marker is a raw offset; adapt it to the same shape as the stack's.
class LinearArena
{
public:
    LinearArena(usize capacity, mem::IAllocator* parent) : m_arena(capacity, parent, "fuzz-linear") {}
    [[nodiscard]] usize capacity() const noexcept { return m_arena.capacity(); }
    [[nodiscard]] usize used() const noexcept { return m_arena.used(); }
    void* try_allocate(usize size, usize alignment) { return m_arena.try_allocate(size, alignment); }
    [[nodiscard]] bool owns(const void* p) const noexcept { return m_arena.owns(p); }
    void reset_to(usize offset) noexcept { m_arena.reset_to(offset); }
    void reset() noexcept { m_arena.reset(); }
    [[nodiscard]] const mem::MemoryStats& stats() const noexcept { return m_arena.stats(); }
    [[nodiscard]] const mem::IAllocator& allocator() const noexcept { return m_arena; }

private:
    mem::LinearAllocator m_arena;
};

void require_stats_drained(const LinearArena& a) noexcept
{
    require_stats_drained(a.allocator());
}

void fuzz_linear(Reader& r, Trace& trace, Parent& parent)
{
    const usize capacity = 1U + static_cast<usize>(r.half()) * 2U;
    LinearArena arena(capacity, &parent);
    run_bump(arena, r, trace, true);
}

void fuzz_stack(Reader& r, Trace& trace, Parent& parent)
{
    const usize capacity = 1U + static_cast<usize>(r.half()) * 2U;
    mem::StackAllocator arena(capacity, &parent, "fuzz-stack");
    run_bump(arena, r, trace, true);
}

void fuzz_growable_linear(Reader& r, Trace& trace)
{
    Parent parent(usize{1} << 20U);
    {
        const usize chunk = 64U + static_cast<usize>(r.half());
        mem::GrowableLinearAllocator arena(chunk, &parent, "fuzz-growable-linear");
        LiveSet live;
        for (usize op = 0U; op < kMaxOps && !r.done(); ++op)
        {
            const u8 code = static_cast<u8>(r.byte() % 8U);
            if (code < 7U || live.count() == 0U)
            {
                const usize size      = decode_size(r, usize{16} << 10U);
                const usize alignment = decode_alignment(r);
                const usize before    = arena.bytes_used();
                void* const p         = arena.try_allocate(size, alignment);
                trace.add(p != nullptr ? 1U : 0U);
                if (malformed(size, alignment))
                {
                    CRD_FUZZ_REQUIRE(p == nullptr);
                }
                if (p == nullptr)
                {
                    CRD_FUZZ_REQUIRE(arena.bytes_used() == before);
                }
                else
                {
                    CRD_FUZZ_REQUIRE(arena.owns(p));
                    CRD_FUZZ_REQUIRE(arena.owns(static_cast<u8*>(p) + (size - 1U)));
                    CRD_FUZZ_REQUIRE(arena.bytes_used() >= before + size);
                    if (!live.full())
                    {
                        live.add(static_cast<u8*>(p), size, alignment, pattern_for(op), 0U);
                    }
                }
            }
            else
            {
                live.remove_if([](const Block&) { return true; });
                arena.reset();
                CRD_FUZZ_REQUIRE(arena.bytes_used() == 0U);
            }
            live.require_all_intact();
#if CRD_MEMORY_STATS_TRACKING
            CRD_FUZZ_REQUIRE(arena.stats().snapshot().bytes_in_use == arena.bytes_used());
#endif
        }
        trace.add(arena.num_chunks());
        live.remove_if([](const Block&) { return true; });
        arena.reset();
        require_stats_drained(arena);
    }
    CRD_FUZZ_REQUIRE(parent.live_bytes() == 0U);
}

// ---- the offset allocator (offsets only, no memory) --------------------------------------------------------------

void fuzz_offset(Reader& r, Trace& trace, Parent& parent)
{
    const u32 capacity  = 1U + (r.word() % (u32{1} << 20U));
    const u32 max_nodes = 1U + (r.byte() % 80U);
    mem::OffsetAllocator heap(capacity, max_nodes, &parent, "fuzz-offset");
    CRD_FUZZ_REQUIRE(heap.free_storage() == capacity);

    struct Range
    {
        mem::OffsetAllocator::Allocation handle;
        u32                              size;
    };
    Range live[kMaxLive] = {};
    usize count          = 0U;
    for (usize op = 0U; op < kMaxOps && !r.done(); ++op)
    {
        const u8 code = static_cast<u8>(r.byte() % 8U);
        if (code < 4U || count == 0U)
        {
            u32 size = 0U;
            switch (r.byte() & 3U)
            {
                case 0U:
                    size = 1U + r.byte();
                    break;
                case 1U:
                    size = 1U + r.half();
                    break;
                case 2U:
                    size = r.word(); // zero and 0xFFFFFFFF-class lengths included
                    break;
                default:
                    size = capacity + r.byte(); // just past the span
                    break;
            }
            const u8  selector  = r.byte();
            const u32 alignment = selector == 0xFEU ? 24U : (u32{1} << (selector % 12U));
            const mem::OffsetAllocator::Allocation a = heap.allocate(size, alignment);
            trace.add(a.valid() ? a.offset : 0xFFFFFFFFFFFFFFFFULL);
            if (size == 0U || !mem::is_pow2(alignment) || static_cast<u64>(size) > capacity)
            {
                CRD_FUZZ_REQUIRE(!a.valid());
            }
            if (a.valid())
            {
                CRD_FUZZ_REQUIRE((a.offset & (alignment - 1U)) == 0U);
                CRD_FUZZ_REQUIRE(static_cast<u64>(a.offset) + size <= capacity);
                for (usize i = 0U; i < count; ++i)
                {
                    const u64 lo = live[i].handle.offset;
                    const u64 hi = lo + live[i].size;
                    CRD_FUZZ_REQUIRE(!(a.offset < hi && lo < static_cast<u64>(a.offset) + size));
                }
                if (count == kMaxLive)
                {
                    heap.free(a);
                }
                else
                {
                    live[count] = Range{a, size};
                    count += 1U;
                }
            }
        }
        else if (code < 7U)
        {
            const usize i = r.byte() % count;
            heap.free(live[i].handle);
            live[i] = live[count - 1U];
            count -= 1U;
        }
        else
        {
            heap.reset();
            count = 0U;
            CRD_FUZZ_REQUIRE(heap.free_storage() == capacity);
        }
        CRD_FUZZ_REQUIRE(heap.free_storage() <= capacity);
        // The free regions the bins hold are the free storage: their sum is it, and none is larger than it.
        const mem::OffsetAllocator::StorageReport now = heap.storage_report();
        CRD_FUZZ_REQUIRE(now.total_free_space == heap.free_storage());
        CRD_FUZZ_REQUIRE(now.largest_free_region <= heap.free_storage());
    }
    while (count > 0U)
    {
        heap.free(live[count - 1U].handle);
        count -= 1U;
    }
    // Every free region coalesced back: the whole span is one free region again.
    const mem::OffsetAllocator::StorageReport report = heap.storage_report();
    CRD_FUZZ_REQUIRE(heap.free_storage() == capacity);
    CRD_FUZZ_REQUIRE(report.total_free_space == capacity);
    CRD_FUZZ_REQUIRE(report.largest_free_region == capacity);
    // A request is rounded up to its size bin, so a span whose capacity no bin represents exactly can never be handed
    // out whole, even fresh: the drained allocator must answer the whole-span request as a fresh one does.
    mem::OffsetAllocator                   fresh(capacity, max_nodes, &parent, "fuzz-offset-fresh");
    const mem::OffsetAllocator::Allocation expected = fresh.allocate(capacity, 1U);
    const mem::OffsetAllocator::Allocation whole    = heap.allocate(capacity, 1U);
    CRD_FUZZ_REQUIRE(whole.valid() == expected.valid());
    if (whole.valid())
    {
        CRD_FUZZ_REQUIRE(whole.offset == 0U);
        heap.free(whole);
        fresh.free(expected);
    }
}

// ---- the epoch ring --------------------------------------------------------------------------------------------

void fuzz_ring(Reader& r, Trace& trace, Parent& parent)
{
    const usize capacity = 64U + static_cast<usize>(r.half());
    const usize k        = usize{2} << (r.byte() % 4U); // 2, 4, 8 or 16
    mem::RingAllocator ring(capacity, &parent, k, "fuzz-ring");
    LiveSet live;
    u64     fence      = 0U; // the open epoch's fence (current_epoch()); epochs below it are closed
    u64     unretired  = 0U; // the lowest closed epoch not retired yet (== fence when none is outstanding)
    // retire(c) frees the claims of every closed epoch with fence <= c and nothing of the open epoch.
    auto retire = [&](u64 c) noexcept
    {
        const u64 open = fence;
        live.remove_if([c, open](const Block& b) { return b.tag <= c && b.tag < open; }); // before the poison
        ring.retire(c);
        if (c + 1U > unretired)
        {
            unretired = c + 1U < fence ? c + 1U : fence;
        }
    };
    for (usize op = 0U; op < kMaxOps && !r.done(); ++op)
    {
        const u8 code = static_cast<u8>(r.byte() % 5U);
        if (code <= 1U)
        {
            const usize size      = decode_size(r, capacity);
            const u8    selector  = r.byte();
            // Up to 128: an alignment past the buffer's 64 is refused, like a non-power-of-two.
            const usize alignment = selector == 0xFEU ? 24U : (usize{1} << (selector % 8U));
            void* const p         = ring.try_claim(size, alignment);
            trace.add(p != nullptr ? 1U : 0U);
            if (malformed(size, alignment) || size > capacity || alignment > mem::kCachelineSize)
            {
                CRD_FUZZ_REQUIRE(p == nullptr);
            }
            if (p != nullptr)
            {
                CRD_FUZZ_REQUIRE(ring.in_use_bytes() <= capacity);
                if (live.full())
                {
                    // Untracked but still owned by the open epoch: keep it out of the model's overlap set only.
                    std::memset(p, 0, size);
                }
                else
                {
                    live.add(static_cast<u8*>(p), size, alignment, pattern_for(op), fence);
                }
            }
        }
        else if (code == 2U || code == 3U)
        {
            // Close the open epoch. Fences increase by one, so closed epochs occupy consecutive mark slots and the
            // K-in-flight precondition is "fewer than K closed epochs unretired": retire the oldest first.
            if (fence - unretired >= k)
            {
                retire(unretired);
            }
            ring.begin_epoch(fence + 1U);
            fence += 1U;
            CRD_FUZZ_REQUIRE(ring.current_epoch() == fence);
        }
        else if (fence > 0U)
        {
            retire(r.byte() % fence); // some closed epoch: the open one is never retired
        }
        live.require_all_intact();
        trace.add(ring.in_use_bytes());
    }
    // Close the open epoch and retire everything: the ring is empty again.
    if (fence > 0U)
    {
        retire(fence - 1U);
    }
    ring.begin_epoch(fence + 1U);
    fence += 1U;
    retire(fence - 1U);
    CRD_FUZZ_REQUIRE(live.count() == 0U);
    CRD_FUZZ_REQUIRE(ring.in_use_bytes() == 0U);
}

// ---- the diagnostic decorator over TLSF: the hostile operations it promises to diagnose ---------------------------

struct Violations
{
    mem::ViolationKind kinds[32] = {};
    const void*        ptrs[32]  = {};
    usize              count     = 0U;
    usize              total     = 0U;

    void clear() noexcept { count = 0U; }
    [[nodiscard]] bool only(mem::ViolationKind kind, const void* p) const noexcept
    {
        if (count == 0U)
        {
            return false;
        }
        for (usize i = 0U; i < count && i < 32U; ++i)
        {
            if (kinds[i] != kind || ptrs[i] != p)
            {
                return false;
            }
        }
        return true;
    }
};

void record_violation(const mem::Violation& v, void* user)
{
    auto* const out = static_cast<Violations*>(user);
    if (out->count < 32U)
    {
        out->kinds[out->count] = v.kind;
        out->ptrs[out->count]  = v.ptr;
    }
    out->count += 1U;
    out->total += 1U;
}

alignas(64) u8 g_foreign[256]; // memory no allocator here ever hands out

void fuzz_diagnostic(Reader& r, Trace& trace, Parent& parent)
{
    crd::fuzz::BudgetAllocator metadata(usize{8} << 20U);
    const usize        capacity = mem::TlsfAllocator::min_pool_size() + static_cast<usize>(r.half()) * 4U;
    mem::TlsfAllocator backing(capacity, &parent, "fuzz-diag-backing");
    {
        mem::DiagnosticConfig cfg;
        cfg.redzone_bytes             = 8U << (r.byte() % 3U); // 8, 16 or 32
        cfg.quarantine_capacity_bytes = static_cast<usize>(r.half());
        cfg.max_live_records          = 4U + (r.byte() % 60U);
        cfg.max_stacks                = 64U;
        cfg.capture_stacks            = false; // stack capture would make the trace depend on the call site
        mem::DiagnosticAllocator diag(&backing, cfg, &metadata, "fuzz-diag");
        Violations violations;
        diag.set_violation_handler(&record_violation, &violations);

        LiveSet live;          // tracked live blocks (provenance records)
        u8*     passthrough[kMaxLive] = {};
        usize   passthrough_count     = 0U;
        u8*     last_freed            = nullptr; // the most recent free, while it may still sit in quarantine
        usize   last_freed_size       = 0U;
        for (usize op = 0U; op < kMaxOps && !r.done(); ++op)
        {
            violations.clear();
            const u8 code = static_cast<u8>(r.byte() % 9U);
            if (code <= 2U || live.count() == 0U)
            {
                const usize size       = decode_size(r, capacity);
                const usize alignment  = decode_alignment(r);
                const u64   saturation = diag.metadata_saturation_count();
                void* const p          = diag.try_allocate(size, alignment);
                trace.add(p != nullptr ? 1U : 0U);
                if (malformed(size, alignment))
                {
                    CRD_FUZZ_REQUIRE(p == nullptr);
                }
                if (p != nullptr && diag.metadata_saturation_count() != saturation)
                {
                    // The record table is full: an untracked passthrough block, reported once, never owned.
                    CRD_FUZZ_REQUIRE(violations.only(mem::ViolationKind::MetadataSaturation, p));
                    CRD_FUZZ_REQUIRE(!diag.owns(p));
                    if (passthrough_count == kMaxLive)
                    {
                        diag.deallocate(p);
                    }
                    else
                    {
                        passthrough[passthrough_count] = static_cast<u8*>(p);
                        passthrough_count += 1U;
                    }
                }
                else if (p != nullptr)
                {
                    CRD_FUZZ_REQUIRE(violations.count == 0U);
                    CRD_FUZZ_REQUIRE(diag.owns(p));
                    CRD_FUZZ_REQUIRE(diag.allocation_size(p) == size);
                    const mem::Provenance prov = diag.provenance_of(p);
                    CRD_FUZZ_REQUIRE(prov.found && !prov.freed && prov.size == size);
                    if (live.full())
                    {
                        diag.deallocate(p);
                    }
                    else
                    {
                        live.add(static_cast<u8*>(p), size, alignment, pattern_for(op), 0U);
                    }
                }
                else
                {
                    CRD_FUZZ_REQUIRE(violations.count == 0U);
                }
            }
            else if (code == 3U)
            {
                const usize i    = live.pick(r);
                u8* const   p    = live.at(i).p;
                const usize size = live.at(i).size;
                live.remove(i);
                diag.deallocate(p);
                CRD_FUZZ_REQUIRE(violations.count == 0U);
                last_freed      = p;
                last_freed_size = size;
                if (size <= cfg.quarantine_capacity_bytes)
                {
                    const mem::Provenance prov = diag.provenance_of(p);
                    CRD_FUZZ_REQUIRE(prov.found && prov.freed); // just freed: it is in quarantine now
                }
            }
            else if (code == 4U && last_freed != nullptr && !live.overlaps(last_freed, 1U, kMaxLive))
            {
                // A double free of the most recent free. Quarantined: reported as a double free. Evicted (the block
                // went back to the backing heap, and its address is not a live block now): reported as unknown. While
                // an untracked passthrough block is live, an evicted pointer is indistinguishable from it (the
                // saturation limit the contract states), so that case is not issued.
                const mem::Provenance prov        = diag.provenance_of(last_freed);
                const bool            quarantined = prov.found && prov.freed;
                if (quarantined || passthrough_count == 0U)
                {
                    diag.deallocate(last_freed);
                    CRD_FUZZ_REQUIRE(violations.only(
                        quarantined ? mem::ViolationKind::DoubleFree : mem::ViolationKind::UnknownPointer, last_freed));
                }
                last_freed = nullptr;
            }
            else if (code == 5U && passthrough_count == 0U)
            {
                // A foreign pointer, or an interior pointer of a live block: never handed out, so reported and
                // refused while no untracked passthrough block exists to make the pointer ambiguous.
                u8* target = g_foreign + (r.byte() % 128U);
                if ((r.byte() & 1U) != 0U && live.count() > 0U)
                {
                    const Block& b = live.at(live.pick(r));
                    if (b.size > 1U)
                    {
                        target = b.p + 1U + (r.byte() % (b.size - 1U));
                    }
                }
                diag.deallocate(target);
                CRD_FUZZ_REQUIRE(violations.only(mem::ViolationKind::UnknownPointer, target));
            }
            else if (code == 6U && live.count() > 0U && cfg.redzone_bytes > 0U)
            {
                // A seeded overrun inside the rear redzone: found by the sweep, then repaired.
                Block&      b      = live.at(live.pick(r));
                const usize offset = r.byte() % cfg.redzone_bytes;
                u8* const   at     = b.p + b.size + offset;
                *at                = static_cast<u8>(~0xFDU);
                CRD_FUZZ_REQUIRE(diag.check_all_redzones() >= 1U);
                CRD_FUZZ_REQUIRE(violations.only(mem::ViolationKind::RedzoneOverrun, b.p));
                *at = 0xFDU;
                violations.clear();
                CRD_FUZZ_REQUIRE(diag.check_all_redzones() == 0U);
            }
            else if (code == 7U && last_freed != nullptr && last_freed_size > 0U)
            {
                // A write after free into a block still in quarantine: found by the sweep, then repaired.
                const mem::Provenance prov = diag.provenance_of(last_freed);
                if (prov.found && prov.freed)
                {
                    u8* const at = last_freed + (r.byte() % last_freed_size);
                    *at          = static_cast<u8>(~0xDEU);
                    CRD_FUZZ_REQUIRE(diag.check_all_redzones() >= 1U);
                    CRD_FUZZ_REQUIRE(violations.only(mem::ViolationKind::QuarantineUseAfterFree, last_freed));
                    *at = 0xDEU;
                    violations.clear();
                    CRD_FUZZ_REQUIRE(diag.check_all_redzones() == 0U);
                }
            }
            else if (code == 8U && passthrough_count > 0U)
            {
                passthrough_count -= 1U;
                diag.deallocate(passthrough[passthrough_count]);
                CRD_FUZZ_REQUIRE(violations.count == 0U);
            }
            live.require_all_intact();
            CRD_FUZZ_REQUIRE(backing.validate_structure());
        }
        violations.clear();
        CRD_FUZZ_REQUIRE(diag.check_all_redzones() == 0U);
        while (live.count() > 0U)
        {
            u8* const p = live.at(0U).p;
            live.remove(0U);
            diag.deallocate(p);
        }
        while (passthrough_count > 0U)
        {
            passthrough_count -= 1U;
            diag.deallocate(passthrough[passthrough_count]);
        }
        CRD_FUZZ_REQUIRE(violations.count == 0U);
        trace.add(violations.total);
        CRD_FUZZ_REQUIRE(backing.validate_structure());
    }
    // The decorator gone, the quarantine is released: the backing heap holds nothing.
    CRD_FUZZ_REQUIRE(backing.validate_structure());
    require_stats_drained(backing);
    CRD_FUZZ_REQUIRE(metadata.live_bytes() == 0U);
}

// ---- generation-checked handles ----------------------------------------------------------------------------------

void fuzz_slot_map(Reader& r, Trace& trace, Parent& parent)
{
    using Map = crd::containers::SlotMap<u64>;
    Map map(&parent);
    struct Entry
    {
        Map::Handle handle;
        u64         value = 0U;
    };
    Entry live[kMaxLive] = {};
    usize live_count     = 0U;
    Map::Handle stale[kMaxLive] = {};
    usize       stale_count     = 0U;
    for (usize op = 0U; op < kMaxOps && !r.done(); ++op)
    {
        const u8 code = static_cast<u8>(r.byte() % 6U);
        if ((code <= 1U || live_count == 0U) && live_count < kMaxLive)
        {
            const u64         value = r.word();
            const Map::Handle h     = map.insert(value);
            trace.add((static_cast<u64>(h.index) << 32U) | h.generation);
            CRD_FUZZ_REQUIRE(h.valid());
            for (usize i = 0U; i < stale_count; ++i)
            {
                CRD_FUZZ_REQUIRE(!(stale[i] == h)); // a reused slot never revives an old handle
            }
            live[live_count] = Entry{h, value};
            live_count += 1U;
        }
        else if (code == 2U && live_count > 0U)
        {
            const usize i = r.byte() % live_count;
            CRD_FUZZ_REQUIRE(map.erase(live[i].handle));
            CRD_FUZZ_REQUIRE(!map.contains(live[i].handle));
            CRD_FUZZ_REQUIRE(!map.erase(live[i].handle)); // a double erase is refused
            if (stale_count < kMaxLive)
            {
                stale[stale_count] = live[i].handle;
                stale_count += 1U;
            }
            live[i] = live[live_count - 1U];
            live_count -= 1U;
        }
        else if (code == 3U && stale_count > 0U)
        {
            const Map::Handle h = stale[r.byte() % stale_count];
            CRD_FUZZ_REQUIRE(!map.contains(h));
            CRD_FUZZ_REQUIRE(map.get(h) == nullptr);
            CRD_FUZZ_REQUIRE(!map.erase(h));
        }
        else if (code == 4U)
        {
            // A forged handle: a random index and generation is either a live handle or refused.
            Map::Handle forged;
            forged.index      = r.byte();
            forged.generation = r.byte();
            bool is_live      = false;
            for (usize i = 0U; i < live_count; ++i)
            {
                if (live[i].handle == forged)
                {
                    is_live = true;
                }
            }
            CRD_FUZZ_REQUIRE(map.contains(forged) == is_live);
            if (!is_live)
            {
                CRD_FUZZ_REQUIRE(map.get(forged) == nullptr);
                CRD_FUZZ_REQUIRE(!map.erase(forged));
            }
        }
        for (usize i = 0U; i < live_count; ++i)
        {
            const u64* const v = map.get(live[i].handle);
            CRD_FUZZ_REQUIRE(v != nullptr && *v == live[i].value);
        }
        CRD_FUZZ_REQUIRE(map.size() == live_count);
    }
    for (usize i = 0U; i < live_count; ++i)
    {
        CRD_FUZZ_REQUIRE(map.erase(live[i].handle));
    }
    CRD_FUZZ_REQUIRE(map.empty());
}

u64 run_once(const u8* data, usize size)
{
    Trace                      trace;
    Parent                     parent;
    {
        Reader     r(data, size);
        const Kind kind = static_cast<Kind>(r.byte() % static_cast<u8>(Kind::Count));
        trace.add(static_cast<u64>(kind));
        switch (kind)
        {
            case Kind::Tlsf:
                fuzz_tlsf(r, trace, parent);
                break;
            case Kind::Pool:
                fuzz_pool(r, trace, parent);
                break;
            case Kind::Linear:
                fuzz_linear(r, trace, parent);
                break;
            case Kind::Stack:
                fuzz_stack(r, trace, parent);
                break;
            case Kind::GrowableLinear:
                fuzz_growable_linear(r, trace);
                break;
            case Kind::GrowableTlsf:
                fuzz_growable_tlsf(r, trace);
                break;
            case Kind::GrowablePool:
                fuzz_growable_pool(r, trace);
                break;
            case Kind::Offset:
                fuzz_offset(r, trace, parent);
                break;
            case Kind::Ring:
                fuzz_ring(r, trace, parent);
                break;
            case Kind::Diagnostic:
                fuzz_diagnostic(r, trace, parent);
                break;
            case Kind::SlotMap:
            case Kind::Count:
                fuzz_slot_map(r, trace, parent);
                break;
        }
    }
    // Every allocator returned its backing memory to the parent when it was destroyed.
    CRD_FUZZ_REQUIRE(parent.live_bytes() == 0U);
    return trace.hash();
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const u64 first  = run_once(data, size);
    const u64 second = run_once(data, size);
    CRD_FUZZ_REQUIRE(first == second); // the same operations give the same outcomes
    return 0;
}

namespace
{
// Seed inputs are written in the decoder's own encoding: a kind byte, the kind's configuration, then operations.
class Seed
{
public:
    explicit Seed(Kind kind) { m_bytes.push_back(static_cast<u8>(kind)); }

    Seed& b(u32 value)
    {
        m_bytes.push_back(static_cast<u8>(value));
        return *this;
    }
    Seed& h(u32 value) { return b(value & 0xFFU).b((value >> 8U) & 0xFFU); }
    Seed& w(u32 value) { return h(value & 0xFFFFU).h(value >> 16U); }

    // decode_size encodings: small (1..256), medium (1 + h % cap), raw 32-bit, SIZE_MAX - k.
    Seed& small(u32 n) { return b(0U).b(n - 1U); }
    Seed& medium(u32 n) { return b(4U).h(n - 1U); }
    Seed& raw(u32 n) { return b(6U).w(n); }
    Seed& extreme(u32 k) { return b(7U).b(k); }
    // decode_alignment encodings: 1 << log2, the top bit, a non-power-of-two.
    Seed& align(u32 log2) { return b(log2); }
    Seed& top_bit() { return b(0xFFU); }
    Seed& not_pow2() { return b(0xFEU); }

    void emit(crd::fuzz::SeedSink& sink, const char* name) const { sink.add(name, m_bytes.data(), m_bytes.size()); }
    // A copy cut `drop` bytes short: a stream that ends inside its last operation.
    void emit_truncated(crd::fuzz::SeedSink& sink, const char* name, usize drop) const
    {
        sink.add(name, m_bytes.data(), m_bytes.size() > drop ? m_bytes.size() - drop : 0U);
    }

private:
    crd::containers::Array<u8> m_bytes;
};
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    sink.add_text("empty", "");

    // TLSF (capacity = floor + 4 * h). Ops: 0 alloc, 1 free, 2 realloc (pick, half), 3 sweep.
    {
        Seed s(Kind::Tlsf);
        s.h(2048U);
        s.b(0U).small(100U).align(4U).b(0U).medium(1500U).align(5U).b(0U).small(48U).align(4U);
        s.b(1U).b(1U);                   // free the middle block: coalescing
        s.b(2U).b(0U).h(1499U);          // grow (in place or move)
        s.b(2U).b(0U).h(15U);            // shrink in place
        s.b(3U).b(1U).b(0U).b(1U).b(0U); // sweep, frees
        s.emit(sink, "tlsf-alloc-free-realloc");
        s.emit_truncated(sink, "tlsf-truncated-mid-op", 1U);
    }
    {
        // Malformed requests: SIZE_MAX - k, size 0, a size past the pool, the top-bit and a non-power-of-two
        // alignment. All refused; the heap stays whole.
        Seed s(Kind::Tlsf);
        s.h(64U);
        s.b(0U).extreme(0U).align(4U).b(0U).extreme(15U).align(4U).b(0U).raw(0U).align(4U);
        s.b(0U).raw(0xFFFFFFFFU).align(4U).b(0U).small(32U).top_bit().b(0U).small(32U).not_pow2();
        s.b(0U).small(64U).align(12U).b(3U);
        s.emit(sink, "tlsf-malformed-lengths");
    }
    {
        // Exhaustion then recovery on a tiny pool.
        Seed s(Kind::Tlsf);
        s.h(16U);
        for (u32 i = 0U; i < 12U; ++i)
        {
            s.b(0U).small(32U).align(4U);
        }
        for (u32 i = 0U; i < 12U; ++i)
        {
            s.b(1U).b(0U);
        }
        s.b(0U).small(200U).align(4U);
        s.emit(sink, "tlsf-exhaust-recover");
    }

    // Pool (slot 8 + b % 249, count 1 + b % 96, alignment 8 << b % 4). Ops: 0/1 alloc, 2 free, 3 sweep.
    {
        Seed s(Kind::Pool);
        s.b(24U).b(3U).b(1U); // 32-byte slots, 4 slots, 16-byte alignment
        for (u32 i = 0U; i < 6U; ++i)
        {
            s.b(0U).small(32U).align(4U); // the fifth and sixth are refused: exact exhaustion
        }
        s.b(0U).small(33U).align(4U).b(0U).small(8U).align(5U).b(0U).raw(0U).align(3U); // too big, too aligned, 0
        s.b(2U).b(0U).b(2U).b(1U).b(0U).small(1U).align(0U).b(3U);
        s.emit(sink, "pool-exact-exhaustion");
    }
    {
        Seed s(Kind::GrowablePool);
        s.b(56U).b(2U).b(3U); // 64-byte slots aligned 32, 4 slots per page
        for (u32 i = 0U; i < 20U; ++i)
        {
            s.b(0U).small(64U).align(5U);
        }
        for (u32 i = 0U; i < 10U; ++i)
        {
            s.b(2U).b(i * 3U);
        }
        s.b(0U).small(65U).align(5U).b(0U).small(8U).align(6U);
        s.emit(sink, "growable-pool-pages");
    }

    // Linear and stack (capacity 1 + 2h). Ops: 0-2 alloc, 3 mark, 4 rewind to marker b, 5 reset.
    {
        // Padding between an odd-sized block and an aligned one: the statistics must account for it.
        Seed s(Kind::Linear);
        s.h(512U);
        s.b(0U).small(3U).align(0U).b(0U).small(5U).align(6U).b(3U).b(0U).small(7U).align(4U);
        s.b(4U).b(0U).b(0U).small(1U).align(9U).b(5U).b(0U).small(255U).align(0U);
        s.b(0U).extreme(3U).align(0U).b(0U).small(9U).top_bit();
        s.emit(sink, "linear-padding-rewind");
    }
    {
        Seed s(Kind::Stack);
        s.h(300U);
        s.b(3U).b(0U).small(10U).align(3U).b(3U).b(0U).small(20U).align(5U).b(3U).b(0U).small(30U).align(7U);
        s.b(4U).b(1U).b(0U).small(40U).align(2U).b(4U).b(0U).b(0U).medium(601U).align(0U);
        s.b(0U).medium(600U).align(0U);
        s.emit(sink, "stack-markers");
    }
    {
        // An allocation larger than the chunk gets its own chunk; reset rewinds every chunk.
        Seed s(Kind::GrowableLinear);
        s.h(64U); // 128-byte chunks
        s.b(0U).small(100U).align(3U).b(0U).medium(5000U).align(6U).b(0U).small(17U).align(4U);
        s.b(7U).b(0U).small(200U).align(0U).b(0U).extreme(1U).align(0U).b(0U).raw(0U).align(0U);
        s.emit(sink, "growable-linear-oversize");
    }
    {
        // Chunk growth until the parent's budget refuses: a refusal, never the fatal path.
        Seed s(Kind::GrowableTlsf);
        s.h(1024U); // 4 KiB chunks
        for (u32 i = 0U; i < 40U; ++i)
        {
            s.b(0U).medium(60000U).align(4U);
        }
        s.b(1U).b(3U).b(2U).b(0U).h(200U).b(0U).extreme(0U).align(4U);
        s.emit(sink, "growable-tlsf-to-budget");
    }

    // Offset (capacity 1 + w % 2^20, nodes 1 + b % 80). Ops: 0-3 alloc (selector, length, alignment), 4-6 free,
    // 7 reset.
    {
        Seed s(Kind::Offset);
        s.w(4095U).b(3U); // 4096 bytes, 4 nodes: a split needs a node, so node exhaustion refuses
        for (u32 i = 0U; i < 6U; ++i)
        {
            s.b(0U).b(0U).b(99U).b(4U);
        }
        s.b(4U).b(1U).b(4U).b(0U).b(0U).b(0U).b(255U).b(0U);
        s.b(0U).b(2U).w(0xFFFFFFFFU).b(0U).b(0U).b(2U).w(0U).b(0U).b(0U).b(3U).b(0U).b(0U).b(0U).b(0U).b(0xFEU);
        s.b(7U).b(0U).b(1U).h(4095U).b(0U);
        s.emit(sink, "offset-nodes-and-lengths");
    }

    // Ring (capacity 64 + h, K = 2 << b % 4). Ops: 0/1 claim (size, alignment selector), 2/3 close epoch, 4 retire.
    {
        Seed s(Kind::Ring);
        s.h(192U).b(0U); // 256 bytes, K = 2
        for (u32 i = 0U; i < 8U; ++i)
        {
            s.b(0U).small(60U).b(6U).b(2U);
        }
        s.b(0U).small(10U).b(7U).b(0U).small(10U).b(0xFEU).b(0U).extreme(0U).b(0U).b(4U).b(0U).b(4U).b(5U);
        s.emit(sink, "ring-epochs-wrap");
    }

    // DiagnosticAllocator over TLSF (backing h, redzone 8 << b % 3, quarantine h, records 4 + b % 60).
    // Ops: 0-2 alloc, 3 free, 4 double free, 5 foreign/interior free, 6 overrun, 7 write after free, 8 passthrough.
    {
        Seed s(Kind::Diagnostic);
        s.h(4096U).b(1U).h(4096U).b(28U);
        s.b(0U).small(40U).align(4U).b(0U).small(24U).align(5U).b(0U).small(100U).align(3U);
        s.b(3U).b(0U).b(4U);             // free, then a double free while quarantined
        s.b(6U).b(0U).b(15U);            // overrun into the rear redzone
        s.b(3U).b(0U).b(7U).b(5U);       // free, write after free into the quarantined block
        s.b(5U).b(3U).b(0U);             // a foreign pointer
        s.b(5U).b(9U).b(1U).b(0U).b(4U); // an interior pointer of a live block
        s.emit(sink, "diag-double-free-overrun-uaf");
    }
    {
        // No quarantine: a double free names an evicted pointer, which must never reach the backing heap.
        Seed s(Kind::Diagnostic);
        s.h(4096U).b(0U).h(0U).b(28U);
        s.b(0U).small(64U).align(4U).b(0U).small(64U).align(4U).b(3U).b(0U).b(4U).b(5U).b(0U).b(1U).b(0U).b(10U);
        s.emit(sink, "diag-evicted-double-free-interior");
    }
    {
        // Four records: the fifth allocation saturates the table and is an untracked passthrough block.
        Seed s(Kind::Diagnostic);
        s.h(4096U).b(2U).h(64U).b(0U);
        for (u32 i = 0U; i < 7U; ++i)
        {
            s.b(0U).small(16U).align(4U);
        }
        s.b(8U).b(8U).b(3U).b(0U).b(4U);
        s.emit(sink, "diag-saturation-passthrough");
    }

    // SlotMap. Ops: 0/1 insert (word), 2 erase (and a double erase), 3 stale handle, 4 forged handle.
    {
        Seed s(Kind::SlotMap);
        s.b(0U).w(11U).b(0U).w(22U).b(0U).w(33U);
        s.b(2U).b(1U).b(3U).b(0U).b(0U).w(44U); // erase, stale lookup, an insert reusing the slot
        s.b(3U).b(0U).b(4U).b(1U).b(1U).b(4U).b(1U).b(2U).b(4U).b(0xFFU).b(0U);
        s.b(2U).b(0U).b(2U).b(0U).b(2U).b(0U).b(3U).b(1U);
        s.emit(sink, "slotmap-stale-reuse-forged");
    }
}
