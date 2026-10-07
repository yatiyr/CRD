// DIAG.3b -- allocator-aware AddressSanitizer boundaries. Two halves:
//  - In-process positives: correct use of every poisoning allocator (pool, growable pool, linear, stack, growable
//    linear, nested arenas, TLSF, growable TLSF, ring; odd and large slots, small and over-aligned objects, in-place
//    resizes) and of Array's container live range (every mutator, growth, copies, arena-backed and packed buffers)
//    stays clean under ASan and keeps its data; where the build has ASan, the shadow state is asserted directly
//    (asan_is_poisoned only queries the shadow, it never touches the memory). Without ASan the helpers are no-ops and
//    report nothing poisoned.
//  - The negative control: crd-diag-allocator-poison-specimen performs one intentional stale or out-of-range read per
//    mode in a bounded child. Under ASan each must end in the report its mode declares, use-after-poison for an
//    allocator's poison and container-overflow for an Array's unused capacity (exit 42); without ASan the specimen
//    reports InstrumentAbsent. No undefined behaviour runs in this process.
// Contract: docs/design/runtime-diagnostics.md#diag-3b.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/diag/specimen_runner.hpp>
#include <crd/memory/allocators/growable_linear_allocator.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/allocators/ring_allocator.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/asan_poison.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <utility>

namespace
{
namespace mem = crd::memory;

#if CRD_MEM_ASAN
constexpr bool kAsan = true;
#else
constexpr bool kAsan = false;
#endif

// Every one of the first n bytes of p is addressable (on every build).
bool all_live(const void* p, crd::usize n)
{
    const auto* const b = static_cast<const crd::u8*>(p);
    for (crd::usize i = 0; i < n; ++i)
    {
        if (mem::asan_is_poisoned(b + i))
        {
            return false;
        }
    }
    return true;
}

// Under ASan: the first `live` bytes of p are addressable and byte `live` is poisoned. Without ASan: nothing is
// reported poisoned. Only the shadow is queried.
bool live_exactly(const void* p, crd::usize live)
{
    const auto* const b = static_cast<const crd::u8*>(p);
    if (!kAsan)
    {
        return !mem::asan_is_poisoned(b) && !mem::asan_is_poisoned(b + live);
    }
    return all_live(b, live) && mem::asan_is_poisoned(b + live);
}

// Under ASan the byte at p is poisoned; without ASan it never is.
bool poisoned(const void* p)
{
    return mem::asan_is_poisoned(p) == kAsan;
}

bool holds(const void* p, crd::usize n, crd::u8 value)
{
    const auto* const b = static_cast<const crd::u8*>(p);
    for (crd::usize i = 0; i < n; ++i)
    {
        if (b[i] != value)
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("allocator asan: the poison helpers are no-ops without ASan", "[memory][diag][asan]")
{
    alignas(16) crd::u8 buffer[64] = {};
    mem::asan_poison(buffer + 16, 32U);
    CHECK(poisoned(buffer + 16));
    CHECK(poisoned(buffer + 47));
    CHECK_FALSE(mem::asan_is_poisoned(buffer));
    CHECK_FALSE(mem::asan_is_poisoned(buffer + 48));
    mem::asan_unpoison(buffer + 16, 32U);
    CHECK_FALSE(mem::asan_is_poisoned(buffer + 16));
    buffer[20] = 7U; // addressable again on every build
    CHECK(buffer[20] == 7U);
}

TEST_CASE("allocator asan: a pool poisons free slots whole and exposes live slots whole", "[memory][diag][asan]")
{
    mem::PoolAllocator pool(48U, 4U, 16U, nullptr, "asan-pool");
    // A free slot is poisoned header included; the walk reads each link just in time and leaves it poisoned.
    CHECK(pool.validate_structure());
    auto* const a = static_cast<crd::u8*>(pool.allocate(24U, 16U));
    REQUIRE(a != nullptr);
    CHECK(all_live(a, 48U)); // the logical allocation is the slot (allocation_size() reports it)
    CHECK(pool.allocation_size(a) == 48U);
    std::memset(a, 0x11, 48U);

    auto* const b = static_cast<crd::u8*>(pool.allocate(3U, 16U)); // a small object still gets its whole slot
    REQUIRE(b != nullptr);
    CHECK(all_live(b, 48U));
    std::memset(b, 0x22, 48U);

    pool.deallocate(a);
    CHECK(poisoned(a)); // the free-list header faults too ...
    CHECK(poisoned(a + 47));
    CHECK(pool.validate_structure()); // ... yet the list still walks
    CHECK(poisoned(a));               // and the walk put the header's poison back
    CHECK(all_live(b, 48U));          // a live neighbour is untouched

    auto* const c = static_cast<crd::u8*>(pool.allocate(48U, 16U)); // reuse: the whole slot is live again
    CHECK(c == a);
    CHECK(all_live(c, 48U));
    std::memset(c, 0x33, 48U);
    CHECK(holds(b, 48U, 0x22)); // a neighbour keeps its bytes
    pool.deallocate(b);
    pool.deallocate(c);
    CHECK(pool.validate_structure());
    CHECK(pool.slots_free() == 4U);
}

TEST_CASE("allocator asan: odd, large and over-aligned pool slots stay usable under poisoning", "[memory][diag][asan]")
{
    SECTION("odd 4099-byte slots packed at an 8-byte stride")
    {
        mem::PoolAllocator pool(4099U, 3U, 8U, nullptr, "asan-pool-odd");
        REQUIRE(pool.slot_size() == 4104U);
        crd::u8* slots[3] = {};
        for (crd::u8*& s : slots)
        {
            s = static_cast<crd::u8*>(pool.allocate(4099U, 8U));
            REQUIRE(s != nullptr);
            CHECK(all_live(s, 4104U));
            std::memset(s, 0x44, 4104U);
        }
        pool.deallocate(slots[1]);
        CHECK(poisoned(slots[1]));
        CHECK(poisoned(slots[1] + 4103U));
        CHECK(holds(slots[0], 4104U, 0x44));
        CHECK(holds(slots[2], 4104U, 0x44));
        slots[1] = static_cast<crd::u8*>(pool.allocate(4099U, 8U));
        std::memset(slots[1], 0x45, 4104U);
        for (crd::u8* s : slots)
        {
            pool.deallocate(s);
        }
        CHECK(pool.validate_structure());
    }
    SECTION("large 64 KiB slots, over-aligned to 256")
    {
        mem::PoolAllocator pool(64U * 1024U, 2U, 256U, nullptr, "asan-pool-large");
        auto* const p = static_cast<crd::u8*>(pool.allocate(64U * 1024U - 1U, 256U));
        REQUIRE(p != nullptr);
        CHECK((reinterpret_cast<crd::usize>(p) % 256U) == 0U);
        CHECK(all_live(p, 64U * 1024U));
        std::memset(p, 0x46, 64U * 1024U);
        pool.deallocate(p);
        CHECK(poisoned(p));
        CHECK(poisoned(p + 64U * 1024U - 1U));
    }
    SECTION("growable pool across pages")
    {
        mem::GrowablePoolAllocator pool(40U, 8U, 2U, nullptr, "asan-gpool");
        crd::u8* slots[5] = {};
        for (crd::u8*& s : slots)
        {
            s = static_cast<crd::u8*>(pool.allocate(36U, 8U));
            REQUIRE(s != nullptr);
            CHECK(all_live(s, 40U));
            std::memset(s, 0x47, 40U);
        }
        CHECK(pool.page_count() == 3U);
        pool.deallocate(slots[2]);
        CHECK(poisoned(slots[2]));
        CHECK(poisoned(slots[2] + 39U));
        auto* const again = static_cast<crd::u8*>(pool.allocate(10U, 8U));
        CHECK(again == slots[2]);
        CHECK(all_live(again, 40U));
        for (crd::u8* s : slots)
        {
            pool.deallocate(s);
        }
        CHECK(pool.slots_in_use() == 0U);
    }
}

TEST_CASE("allocator asan: arenas expose live slices, keep padding poisoned and re-poison on reset",
          "[memory][diag][asan]")
{
    SECTION("linear: one-byte and over-aligned slices, rewind and reuse")
    {
        mem::LinearAllocator a(4096U, nullptr, "asan-linear");
        auto* const one = static_cast<crd::u8*>(a.allocate(1U, 16U));
        CHECK(live_exactly(one, 1U));
        auto* const wide = static_cast<crd::u8*>(a.allocate(64U, 64U));
        CHECK((reinterpret_cast<crd::usize>(wide) % 64U) == 0U);
        CHECK(live_exactly(wide, 64U));
        CHECK(poisoned(wide - 1)); // alignment padding
        std::memset(wide, 0x51, 64U);
        {
            mem::LinearScope scope(a);
            auto* const inner = static_cast<crd::u8*>(a.allocate(32U, 16U));
            std::memset(inner, 0x52, 32U);
            CHECK(live_exactly(inner, 32U));
        }
        CHECK(holds(wide, 64U, 0x51)); // the rewind kept what came before the scope
        CHECK(poisoned(wide + 64));    // and poisoned what the scope had
        a.reset();
        CHECK(poisoned(wide));
        auto* const reused = static_cast<crd::u8*>(a.allocate(128U, 16U));
        CHECK(live_exactly(reused, 128U));
        std::memset(reused, 0x53, 128U);
    }
    SECTION("stack: a popped frame goes stale, the frame below stays live")
    {
        mem::StackAllocator a(4096U, nullptr, "asan-stack");
        auto* const below = static_cast<crd::u8*>(a.allocate(48U, 16U));
        std::memset(below, 0x54, 48U);
        crd::u8* top = nullptr;
        {
            mem::StackScope frame(a);
            top = static_cast<crd::u8*>(a.allocate(48U, 16U));
            std::memset(top, 0x55, 48U);
        }
        CHECK(poisoned(top));
        CHECK(live_exactly(below, 48U));
        CHECK(holds(below, 48U, 0x54));
    }
    SECTION("growable linear: every chunk re-poisons on reset and is reused clean")
    {
        mem::GrowableLinearAllocator a(1024U, nullptr, "asan-glinear");
        crd::u8* first = nullptr;
        for (int i = 0; i < 40; ++i) // spills into several chunks
        {
            auto* const p = static_cast<crd::u8*>(a.allocate(100U, 16U));
            REQUIRE(p != nullptr);
            CHECK(live_exactly(p, 100U));
            std::memset(p, 0x56, 100U);
            if (first == nullptr)
            {
                first = p;
            }
        }
        CHECK(a.num_chunks() > 1U);
        a.reset();
        CHECK(poisoned(first));
        for (int i = 0; i < 40; ++i) // reuse of the rewound chunks is clean
        {
            auto* const p = static_cast<crd::u8*>(a.allocate(100U, 16U));
            REQUIRE(p != nullptr);
            std::memset(p, 0x57, 100U);
        }
    }
    SECTION("nested arenas: a child returns exactly its slice and never unpoisons the parent's tail")
    {
        mem::LinearAllocator parent(4096U, nullptr, "asan-parent");
        auto* const slice = static_cast<crd::u8*>(parent.allocate(256U, 16U));
        {
            mem::LinearAllocator child(slice, 256U, "asan-child");
            CHECK(poisoned(slice)); // the child owns the slice's poison while it lives
            auto* const p = static_cast<crd::u8*>(child.allocate(64U, 16U));
            std::memset(p, 0x58, 64U);
            {
                mem::PoolAllocator grandchild(child.allocate(128U, 16U), 32U, 4U, 16U, "asan-grandchild");
                auto* const g = static_cast<crd::u8*>(grandchild.allocate(32U, 16U));
                std::memset(g, 0x59, 32U);
            }
            CHECK(holds(p, 64U, 0x58));
        }
        CHECK(live_exactly(slice, 256U)); // returned unpoisoned, and the parent's tail is still poisoned
        std::memset(slice, 0x5A, 256U);
    }
}

TEST_CASE("allocator asan: TLSF poisons every header and free block and exposes live blocks whole",
          "[memory][diag][asan]")
{
    SECTION("headers, freed blocks and coalescing")
    {
        mem::TlsfAllocator heap(64U * 1024U, nullptr, "asan-tlsf");
        CHECK(heap.validate_structure()); // the walk opens each header word just in time
        auto* const a = static_cast<crd::u8*>(heap.allocate(13U, 16U));
        auto* const b = static_cast<crd::u8*>(heap.allocate(48U, 16U));
        auto* const c = static_cast<crd::u8*>(heap.allocate(1U, 16U));
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        REQUIRE(c != nullptr);
        CHECK(heap.allocation_size(a) == 16U); // a 13-byte request gets a 16-byte block: the logical allocation
        CHECK(live_exactly(a, 16U));           // ... ending at the next block's header
        CHECK(live_exactly(b, 48U));
        CHECK(poisoned(b - 1)); // b's own header
        CHECK(poisoned(b - 16));
        std::memset(a, 0x61, 16U);
        std::memset(b, 0x62, 48U);
        std::memset(c, 0x63, heap.allocation_size(c));

        heap.deallocate(b);
        CHECK(poisoned(b)); // the free-list links ...
        CHECK(poisoned(b + 47));
        CHECK(heap.validate_structure()); // ... still walk
        CHECK(poisoned(b));               // and stay poisoned afterwards
        CHECK(holds(a, 16U, 0x61));       // live neighbours keep their bytes
        heap.deallocate(a);               // coalesces with b: a's payload and b's absorbed header are poisoned
        CHECK(poisoned(a));
        CHECK(poisoned(b - 16));
        CHECK(heap.validate_structure());

        auto* const d = static_cast<crd::u8*>(heap.allocate(64U, 16U)); // reuse of the merged block is clean
        REQUIRE(d != nullptr);
        CHECK(all_live(d, 64U));
        std::memset(d, 0x64, 64U);
        heap.deallocate(c);
        heap.deallocate(d);
        CHECK(heap.validate_structure());
    }
    SECTION("over-aligned, large and odd blocks")
    {
        mem::TlsfAllocator heap(512U * 1024U, nullptr, "asan-tlsf-shapes");
        auto* const small = static_cast<crd::u8*>(heap.allocate(24U, 16U));
        auto* const aligned = static_cast<crd::u8*>(heap.allocate(100U, 256U));
        auto* const large = static_cast<crd::u8*>(heap.allocate(64U * 1024U, 64U));
        auto* const odd = static_cast<crd::u8*>(heap.allocate(4099U, 16U));
        REQUIRE(small != nullptr);
        REQUIRE(aligned != nullptr);
        REQUIRE(large != nullptr);
        REQUIRE(odd != nullptr);
        CHECK(mem::is_aligned(aligned, 256U));
        CHECK(mem::is_aligned(large, 64U));
        CHECK(live_exactly(aligned, heap.allocation_size(aligned)));
        CHECK(poisoned(aligned - 1)); // its header
        CHECK(live_exactly(large, heap.allocation_size(large)));
        CHECK(live_exactly(odd, heap.allocation_size(odd)));
        std::memset(small, 0x71, 24U);
        std::memset(aligned, 0x72, heap.allocation_size(aligned));
        std::memset(large, 0x73, heap.allocation_size(large));
        std::memset(odd, 0x74, heap.allocation_size(odd));
        heap.deallocate(large);
        CHECK(poisoned(large + 32U * 1024U));
        CHECK(holds(small, 24U, 0x71));
        CHECK(holds(aligned, 100U, 0x72));
        CHECK(holds(odd, 4099U, 0x74));
        CHECK(heap.validate_structure());
        heap.deallocate(small);
        heap.deallocate(aligned);
        heap.deallocate(odd);
        CHECK(heap.validate_structure());
    }
    SECTION("in-place shrink poisons the split-off tail, in-place grow exposes the extension")
    {
        mem::TlsfAllocator heap(64U * 1024U, nullptr, "asan-tlsf-realloc");
        auto* const p = static_cast<crd::u8*>(heap.allocate(256U, 16U));
        REQUIRE(p != nullptr);
        std::memset(p, 0x75, 256U);
        auto* const shrunk = static_cast<crd::u8*>(heap.reallocate(p, 256U, 32U, 16U));
        REQUIRE(shrunk == p);
        CHECK(live_exactly(shrunk, 32U)); // byte 32 is the split-off block's header
        CHECK(poisoned(shrunk + 100));
        auto* const grown = static_cast<crd::u8*>(heap.reallocate(shrunk, 32U, 512U, 16U));
        REQUIRE(grown == p); // the free tail after it lets it grow in place
        CHECK(all_live(grown, 512U));
        CHECK(holds(grown, 32U, 0x75));
        std::memset(grown, 0x76, 512U);
        CHECK(heap.validate_structure());
        heap.deallocate(grown);
        CHECK(heap.validate_structure());
    }
    SECTION("a caller buffer and a growable heap are returned addressable, exactly their range")
    {
        alignas(16) static crd::u8 buffer[8192];
        {
            mem::TlsfAllocator heap(buffer, sizeof(buffer), "asan-tlsf-caller");
            CHECK(poisoned(buffer)); // the start sentinel
            auto* const p = static_cast<crd::u8*>(heap.allocate(64U, 16U));
            REQUIRE(p != nullptr);
            std::memset(p, 0x77, 64U);
        }
        CHECK(all_live(buffer, sizeof(buffer)));
        std::memset(buffer, 0, sizeof(buffer));

        mem::GrowableTlsfAllocator grow(4096U, nullptr, "asan-gtlsf");
        auto* const first = static_cast<crd::u8*>(grow.allocate(3000U, 16U));
        auto* const second = static_cast<crd::u8*>(grow.allocate(6000U, 16U)); // more than the first chunk has left
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        CHECK(grow.num_chunks() >= 2U);
        CHECK(all_live(second, 6000U));
        std::memset(first, 0x78, 3000U);
        std::memset(second, 0x79, 6000U);
        grow.deallocate(second);
        CHECK(poisoned(second + 1024));
        CHECK(holds(first, 3000U, 0x78));
    }
}

TEST_CASE("allocator asan: a ring poisons unclaimed and retired space", "[memory][diag][asan]")
{
    mem::RingAllocator ring(1024U, nullptr, 4U, "asan-ring");
    auto* const a = static_cast<crd::u8*>(ring.try_claim(64U, 16U));
    REQUIRE(a != nullptr);
    CHECK(live_exactly(a, 64U)); // the space past the head is unclaimed
    std::memset(a, 0x81, 64U);
    auto* const b = static_cast<crd::u8*>(ring.try_claim(64U, 64U)); // 64-aligned: lands right after a
    REQUIRE(b != nullptr);
    CHECK(b - a == 64);
    auto* const pad = static_cast<crd::u8*>(ring.try_claim(32U, 16U));
    REQUIRE(pad != nullptr);
    std::memset(pad, 0x85, 32U);
    auto* const e = static_cast<crd::u8*>(ring.try_claim(64U, 64U)); // 32 bytes of alignment padding before it
    REQUIRE(e != nullptr);
    CHECK(e - pad == 64);
    CHECK(poisoned(e - 1));
    CHECK(live_exactly(e, 64U));
    std::memset(b, 0x82, 64U);
    std::memset(e, 0x86, 64U);

    ring.begin_epoch(1U);
    auto* const c = static_cast<crd::u8*>(ring.try_claim(600U, 16U)); // epoch 1, offsets 256..856
    REQUIRE(c != nullptr);
    std::memset(c, 0x83, 600U);
    ring.retire(0U); // a, b, pad and e retire; c is still in flight
    CHECK(poisoned(a));
    CHECK(poisoned(b + 63));
    CHECK(poisoned(e));
    CHECK(all_live(c, 600U));

    ring.begin_epoch(2U);
    auto* const d = static_cast<crd::u8*>(ring.try_claim(200U, 16U)); // wraps to offset 0 over retired space
    REQUIRE(d == a);
    CHECK(live_exactly(d, 200U));
    CHECK(poisoned(c + 600)); // the wasted tail before the wrap was never claimed
    std::memset(d, 0x84, 200U);
    CHECK(holds(c, 600U, 0x83));
    ring.begin_epoch(3U);
    ring.retire(2U); // the retired span wraps: the tail end of the buffer and its start
    CHECK(poisoned(c));
    CHECK(poisoned(c + 599));
    CHECK(poisoned(d));
    CHECK(poisoned(d + 199));
    CHECK(ring.in_use_bytes() == 0U);
}

TEST_CASE("allocator asan: an Array marks its unused capacity as a container live range", "[memory][diag][asan]")
{
    namespace cont = crd::containers;
    cont::Array<crd::u64> a;
    a.reserve(16U);
    const crd::u64* data = a.data();
    const auto* bytes = reinterpret_cast<const crd::u8*>(data);
    CHECK(live_exactly(data, 0U)); // reserved but empty: the whole capacity is marked
    CHECK(poisoned(bytes + 127));  // the last capacity byte
    a.push_back(1U);
    a.push_back(2U);
    a.push_back(3U);
    CHECK(live_exactly(data, 24U));
    a.pop_back();
    CHECK(live_exactly(data, 16U));
    a.resize(10U);
    CHECK(live_exactly(data, 80U));
    a.resize(2U, 5U);
    CHECK(live_exactly(data, 16U));
    a.insert(0U, 9U);
    CHECK(live_exactly(data, 24U));
    a.erase(0U);
    CHECK(live_exactly(data, 16U));
    a.swap_remove(0U);
    CHECK(live_exactly(data, 8U));
    a.resize_uninitialized(5U);
    CHECK(live_exactly(data, 40U));
    a.emplace_back(7U);
    CHECK(a.try_push_back(8U));
    CHECK(live_exactly(data, 56U));
    a.clear();
    CHECK(live_exactly(data, 0U));

    // Growth relocates into a new buffer marked at the new size; copies are marked at theirs.
    for (crd::u64 i = 0; i < 20U; ++i)
    {
        a.push_back(i * 3U);
    }
    REQUIRE(a.capacity() > a.size());
    CHECK(live_exactly(a.data(), 160U));
    cont::Array<crd::u64> copy;
    copy.reserve(64U);
    copy = a;
    CHECK(live_exactly(copy.data(), 160U));
    const cont::Array<crd::u64> constructed(a);
    CHECK(all_live(constructed.data(), 160U));
    cont::Array<crd::u64> moved(std::move(copy));
    CHECK(live_exactly(moved.data(), 160U)); // the buffer moves with its marking
    a.shrink_to_fit();
    CHECK(a.capacity() == 20U);
    CHECK(all_live(a.data(), 160U));
    for (crd::u64 i = 0; i < 20U; ++i)
    {
        CHECK(a[i] == i * 3U);
        CHECK(moved[i] == i * 3U);
        CHECK(constructed[i] == i * 3U);
    }
}

TEST_CASE("allocator asan: an Array hands an arena slice back whole and never marks a neighbour",
          "[memory][diag][asan]")
{
    namespace cont = crd::containers;
    SECTION("an aligned slice: the arena's tail stays poisoned, the slice returns fully addressable")
    {
        mem::LinearAllocator arena(1024U, nullptr, "asan-array-arena");
        const crd::u64* data = nullptr;
        {
            cont::Array<crd::u64> arr(&arena);
            arr.reserve(8U);
            arr.push_back(1U);
            data = arr.data();
            CHECK(live_exactly(data, 8U));
            CHECK(poisoned(reinterpret_cast<const crd::u8*>(data) + 64)); // the arena's unhanded tail
        }
        // LinearAllocator::deallocate keeps the slice; the array lifted its marking before handing it back.
        CHECK(live_exactly(data, 64U));
    }
    SECTION("a packed slice sharing granules with live neighbours")
    {
        mem::LinearAllocator arena(1024U, nullptr, "asan-array-packed");
        auto* const lead = static_cast<crd::u8*>(arena.allocate(3U, 1U));
        REQUIRE(lead != nullptr);
        REQUIRE((reinterpret_cast<crd::usize>(lead) & 7U) == 0U);
        cont::Array<crd::u8> arr(&arena);
        arr.reserve(20U); // bytes [3, 23): neither end on a granule
        REQUIRE(arr.data() == lead + 3);
        auto* const next = static_cast<crd::u8*>(arena.allocate(5U, 1U)); // bytes [23, 28)
        REQUIRE(next == lead + 23);
        std::memset(lead, 0x11, 3U);
        std::memset(next, 0x22, 5U);
        arr.push_back(1U);
        arr.push_back(2U);
        CHECK(poisoned(arr.data() + 5)); // byte 8, the first whole granule of the capacity
        CHECK(all_live(lead, 3U));
        CHECK(all_live(next, 5U));
        for (crd::u8 i = 2U; i < 20U; ++i)
        {
            arr.push_back(i);
        }
        CHECK(arr.capacity() == 20U);
        CHECK(all_live(arr.data(), 20U));
        CHECK(holds(lead, 3U, 0x11));
        CHECK(holds(next, 5U, 0x22));
        arr.clear();
        CHECK(all_live(lead, 3U));
        CHECK(all_live(next, 5U));
        CHECK(holds(next, 5U, 0x22));
    }
}

TEST_CASE("allocator asan: every poisoned access is the declared ASan report, or reported absent",
          "[memory][diag][asan][harness]")
{
    namespace cd = crd::diag;
    namespace cont = crd::containers;
    const char* const modes[] = {
        "linear-reset",         "linear-rewind",  "linear-overrun",   "linear-small-overrun", "linear-underrun",
        "linear-nested-parent", "stack-pop",      "glinear-reset",    "pool-freed-slot",      "pool-overrun",
        "pool-underrun",        "pool-odd-freed", "pool-odd-overrun", "gpool-freed-slot",     "gpool-overrun",
        "tlsf-freed",           "tlsf-overrun",   "tlsf-underrun",    "tlsf-aligned",         "tlsf-large",
        "tlsf-shrink",          "gtlsf-freed",    "ring-retired",     "ring-overrun",         "ring-underrun",
        "array-past-size",      "array-pop",      "array-clear",      "array-shrink",         "array-odd-bytes",
        "array-arena",
    };
    for (const char* mode : modes)
    {
        cd::Expectation e;
        e.want = cd::Expectation::Want::SanitizerCatch;
        e.expected_identity = cont::String{"crd-diag-allocator-poison-specimen"};
        cont::Array<cont::String> args;
        args.push_back(cont::String{mode});
        const cd::Outcome o = cd::run_specimen(cont::String{CRD_DIAG_ALLOCATOR_POISON_SPECIMEN}, args, e);
        INFO("mode=" << mode << " verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code
                     << " sanitizer=" << o.sanitizer.c_str() << " reason=" << o.reason.c_str());
        if (kAsan)
        {
            // 42 = ASan reported the mode's declared kind (use-after-poison, or container-overflow for an array-*
            // mode); 43 = some other report; 95 = the allocator layout the mode needs did not occur; 0 = the read
            // returned (no boundary).
            CHECK(o.verdict == cd::Verdict::SanitizerCaught);
            CHECK(o.exit_code == 42);
        }
        else
        {
            // The allocator steps still ran; 96 would mean an unknown mode.
            CHECK(o.verdict == cd::Verdict::InstrumentAbsent);
            CHECK(o.exit_code == 0);
        }
    }
}
