// crd::containers::AtomicArray<T> + CacheLinePadded — single-threaded behaviour.
// Concurrent stress lives in tests/stress/test_atomic_array_stress.cpp.

#include <crd/containers/array.hpp>
#include <crd/containers/atomic_array.hpp>
#include <crd/core/assert.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::usize;
using crd::containers::Array;
using crd::containers::AtomicArray;
using crd::containers::CacheLinePadded;

int g_dtor_count = 0;
struct Tracked
{
    int v = 0;
    Tracked() = default;
    explicit Tracked(int x) : v(x) {}
    ~Tracked() { ++g_dtor_count; }
};
static_assert(!std::is_trivially_destructible_v<Tracked>);
} // namespace

TEST_CASE("AtomicArray push/size/full + read back", "[containers][atomic_array]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "aa-test");
    constexpr usize kCap = 16U;
    AtomicArray<u64> a(kCap, &alloc);

    CHECK(a.capacity() == kCap);
    CHECK(a.empty());

    for (u64 i = 0; i < kCap; ++i)
    {
        const usize idx = a.push(i * 10ULL);
        CHECK(idx == static_cast<usize>(i)); // single-threaded → indices are sequential
    }
    CHECK(a.size() == kCap);
    CHECK(a.full());

    for (usize i = 0; i < kCap; ++i)
    {
        CHECK(a[i] == static_cast<u64>(i) * 10ULL);
    }
    u64 sum = 0;
    for (u64 v : a)
    {
        sum += v;
    }
    CHECK(sum == 10ULL * (0 + 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15));

    a.clear();
    CHECK(a.empty());
    CHECK_FALSE(a.full());
    CHECK(a.push(42ULL) == 0U);
    CHECK(a.size() == 1U);
    CHECK(a[0] == 42ULL);
}

TEST_CASE("AtomicArray destroys constructed elements", "[containers][atomic_array]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "aa-test");
    g_dtor_count = 0;
    {
        AtomicArray<Tracked> a(8U, &alloc);
        for (int i = 0; i < 5; ++i)
        {
            (void)a.emplace(i);
        }
        CHECK(g_dtor_count == 0);
        a.clear(); // destroys the 5
        CHECK(g_dtor_count == 5);
        (void)a.emplace(99);
        (void)a.emplace(100);
    } // dtor destroys the remaining 2
    CHECK(g_dtor_count == 7);
}

#if CRD_ENABLE_ASSERTS
namespace
{
int g_aa_assert_hits = 0;
} // namespace

TEST_CASE("AtomicArray overflow asserts and returns npos", "[containers][atomic_array]")
{
    const crd::AssertHandler prev_h = crd::get_assert_handler();
    const crd::AssertPlatformHandler prev_ph = crd::get_assert_platform_handler();
    crd::set_assert_platform_handler([](const char*) -> int { return 0; });
    crd::set_assert_handler([](const char*, const char*, int, const char*) { ++g_aa_assert_hits; });

    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "aa-test");
    AtomicArray<u64> a(2U, &alloc);
    CHECK(a.push(1ULL) == 0U);
    CHECK(a.push(2ULL) == 1U);
    g_aa_assert_hits = 0;
    CHECK(a.push(3ULL) == AtomicArray<u64>::npos);
    CHECK(g_aa_assert_hits >= 1);

    crd::set_assert_handler(prev_h);
    crd::set_assert_platform_handler(prev_ph);
}
#endif

TEST_CASE("CacheLinePadded is one cache line and forwards value access", "[containers][atomic_array]")
{
    static_assert(alignof(CacheLinePadded<u32>) == 64U);
    static_assert(sizeof(CacheLinePadded<u32>) == 64U);
    static_assert(std::is_trivially_copyable_v<CacheLinePadded<u32>>); // so Array<CacheLinePadded<u32>> works/grows

    CacheLinePadded<int> c(5);
    CHECK(c.value == 5);
    CacheLinePadded<u32> d;
    CHECK(d.value == 0U); // value-initialised
}

TEST_CASE("Array<CacheLinePadded<u32>> + atomic_ref as an atomic counter array", "[containers][atomic_array]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "aa-test");
    Array<CacheLinePadded<u32>> hits(8U, &alloc);
    hits.resize(8U); // zero-initialised

    // Each slot lives on its own cache line; ops go through atomic_ref.
    std::atomic_ref<u32>(hits[3].value).fetch_add(5U, std::memory_order_relaxed);
    std::atomic_ref<u32>(hits[3].value).fetch_add(2U, std::memory_order_relaxed);
    std::atomic_ref<u32>(hits[7].value).store(9U, std::memory_order_relaxed);
    CHECK(hits[3].value == 7U);
    CHECK(hits[7].value == 9U);
    CHECK(hits[0].value == 0U);

    // Growth must still work (CacheLinePadded<u32> is trivially copyable).
    for (u32 i = 0; i < 100U; ++i)
    {
        hits.push_back(CacheLinePadded<u32>(i));
    }
    CHECK(hits.size() == 108U);
    CHECK(hits[10].value == 2U); // 10 - 8
}
