// DIAG.3b -- allocator-aware AddressSanitizer boundaries. Two halves:
//  - In-process positives: correct use of every poisoning allocator (pool, growable pool, linear, stack, growable
//    linear, nested arenas, odd and large slots, small and over-aligned objects) stays clean under ASan and keeps its
//    data; where the build has ASan, the shadow state is asserted directly (asan_is_poisoned only queries the shadow,
//    it never touches the memory). Without ASan the helpers are no-ops and report nothing poisoned.
//  - The negative control: crd-diag-allocator-poison-specimen performs one intentional stale or out-of-range read per
//    mode in a bounded child. Under ASan each must end in a use-after-poison report (exit 42); without ASan the
//    specimen reports InstrumentAbsent. No undefined behaviour runs in this process.
// Contract: docs/design/runtime-diagnostics.md#diag-3b.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/diag/specimen_runner.hpp>
#include <crd/memory/allocators/growable_linear_allocator.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/asan_poison.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

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

TEST_CASE("allocator asan: every poisoned access is a use-after-poison report, or reported absent",
          "[memory][diag][asan][harness]")
{
    namespace cd = crd::diag;
    namespace cont = crd::containers;
    const char* const modes[] = {
        "linear-reset",         "linear-rewind",  "linear-overrun",   "linear-small-overrun", "linear-underrun",
        "linear-nested-parent", "stack-pop",      "glinear-reset",    "pool-freed-slot",      "pool-overrun",
        "pool-underrun",        "pool-odd-freed", "pool-odd-overrun", "gpool-freed-slot",     "gpool-overrun",
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
            // 42 = ASan reported use-after-poison; 43 = some other report; 95 = the pool slots were not neighbours;
            // 0 = the read returned (no boundary).
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
