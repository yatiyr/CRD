// Array<T> freeze guard + FrozenView (detour D-002 v2).
//
// freeze()/unfreeze() bracket a "no structural mutation" scope; element access
// stays allowed. The guard is debug-only — the assert-fires tests are gated on
// CRD_ENABLE_ASSERTS; the behavioural tests run in every config.

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::containers::Array;
using crd::containers::FrozenView;
} // namespace

TEST_CASE("Array freeze/unfreeze toggles is_frozen and nests", "[containers][freeze]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "freeze-test");
    Array<int> a(8, &alloc);
    a.push_back(1);
    a.push_back(2);

    CHECK_FALSE(a.is_frozen());
    a.freeze();
#if CRD_ENABLE_ASSERTS
    CHECK(a.is_frozen());
#endif
    a.freeze(); // nested
    a.unfreeze();
#if CRD_ENABLE_ASSERTS
    CHECK(a.is_frozen()); // still frozen — one freeze outstanding
#endif
    a.unfreeze();
    CHECK_FALSE(a.is_frozen());
}

TEST_CASE("Array stays element-accessible while frozen", "[containers][freeze]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "freeze-test");
    Array<int> a(16, &alloc);
    for (int i = 0; i < 8; ++i)
    {
        a.push_back(i);
    }

    a.freeze();
    // Reads + in-place element writes are all legal while frozen.
    CHECK(a.size() == 8U);
    CHECK(a[3] == 3);
    a[3] = 30;
    CHECK(a[3] == 30);
    int sum = 0;
    for (int v : a)
    {
        sum += v;
    }
    CHECK(sum == (0 + 1 + 2 + 30 + 4 + 5 + 6 + 7));
    CHECK(a.data()[0] == 0);
    a.unfreeze();
}

TEST_CASE("FrozenView freezes for its lifetime and exposes element access", "[containers][freeze]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "freeze-test");
    Array<int> a(16, &alloc);
    for (int i = 0; i < 8; ++i)
    {
        a.push_back(i);
    }

    {
        FrozenView<int> fv(a);
#if CRD_ENABLE_ASSERTS
        CHECK(a.is_frozen());
#endif
        CHECK(fv.size() == 8U);
        CHECK(fv[5] == 5);
        fv[5] = 50;
        CHECK(a[5] == 50);
        int sum = 0;
        for (int v : fv)
        {
            sum += v;
        }
        CHECK(sum == (0 + 1 + 2 + 3 + 4 + 50 + 6 + 7));

        // Move transfers the freeze ownership; the moved-from view no longer unfreezes.
        FrozenView<int> moved(std::move(fv));
#if CRD_ENABLE_ASSERTS
        CHECK(a.is_frozen());
#endif
        CHECK(moved.size() == 8U);
    }
    CHECK_FALSE(a.is_frozen()); // unfrozen when the (moved-to) view went out of scope
}

#if CRD_ENABLE_ASSERTS
namespace
{
int g_assert_hits = 0;
} // namespace

TEST_CASE("Array structural mutation while frozen asserts", "[containers][freeze]")
{
    // Save whatever was installed (crd-log installs its own when logging is up) and restore at the end.
    const crd::AssertHandler prev_handler = crd::get_assert_handler();
    const crd::AssertPlatformHandler prev_platform_handler = crd::get_assert_platform_handler();
    crd::set_assert_platform_handler([](const char*) -> int { return 0; }); // no MessageBox, don't break
    crd::set_assert_handler([](const char*, const char*, int, const char*) { ++g_assert_hits; });

    auto make = [](crd::memory::TlsfAllocator& al)
    {
        Array<int> a(8, &al);
        a.push_back(1);
        a.push_back(2);
        return a;
    };

    // Each block: fresh array, freeze, run ONE structural mutator, expect the
    // freeze assert. (Bounds asserts may also fire on the post-mutation state —
    // we only require >= 1 hit, which is the freeze guard catching the misuse.)
    {
        crd::memory::TlsfAllocator al(crd::usize{1} << 16, nullptr, "freeze-test");
        Array<int> a = make(al);
        a.freeze();
        g_assert_hits = 0;
        a.push_back(3);
        CHECK(g_assert_hits >= 1);
        a.unfreeze();
    }
    {
        crd::memory::TlsfAllocator al(crd::usize{1} << 16, nullptr, "freeze-test");
        Array<int> a = make(al);
        a.freeze();
        g_assert_hits = 0;
        (void)a.emplace_back(4);
        CHECK(g_assert_hits >= 1);
        a.unfreeze();
    }
    {
        crd::memory::TlsfAllocator al(crd::usize{1} << 16, nullptr, "freeze-test");
        Array<int> a = make(al);
        a.freeze();
        g_assert_hits = 0;
        a.pop_back();
        CHECK(g_assert_hits >= 1);
        a.unfreeze();
    }
    {
        crd::memory::TlsfAllocator al(crd::usize{1} << 16, nullptr, "freeze-test");
        Array<int> a = make(al);
        a.freeze();
        g_assert_hits = 0;
        a.clear();
        CHECK(g_assert_hits >= 1);
        a.unfreeze();
    }
    {
        crd::memory::TlsfAllocator al(crd::usize{1} << 16, nullptr, "freeze-test");
        Array<int> a = make(al);
        a.freeze();
        g_assert_hits = 0;
        a.resize(1);
        CHECK(g_assert_hits >= 1);
        a.unfreeze();
    }
    {
        crd::memory::TlsfAllocator al(crd::usize{1} << 16, nullptr, "freeze-test");
        Array<int> a = make(al);
        a.freeze();
        g_assert_hits = 0;
        a.reserve(64);
        CHECK(g_assert_hits >= 1);
        a.unfreeze();
    }
    {
        crd::memory::TlsfAllocator al(crd::usize{1} << 16, nullptr, "freeze-test");
        Array<int> a = make(al);
        a.freeze();
        g_assert_hits = 0;
        a.erase(0);
        CHECK(g_assert_hits >= 1);
        a.unfreeze();
    }
    {
        crd::memory::TlsfAllocator al(crd::usize{1} << 16, nullptr, "freeze-test");
        Array<int> a = make(al);
        a.freeze();
        g_assert_hits = 0;
        a.swap_remove(0);
        CHECK(g_assert_hits >= 1);
        a.unfreeze();
    }
    {
        crd::memory::TlsfAllocator al(crd::usize{1} << 16, nullptr, "freeze-test");
        Array<int> a = make(al);
        a.freeze();
        g_assert_hits = 0;
        a.insert(1, 99);
        CHECK(g_assert_hits >= 1);
        a.unfreeze();
    }
    {
        // move-from a frozen array
        crd::memory::TlsfAllocator al(crd::usize{1} << 16, nullptr, "freeze-test");
        Array<int> a = make(al);
        a.freeze();
        g_assert_hits = 0;
        Array<int> b(std::move(a));
        CHECK(g_assert_hits >= 1);
        // a is now moved-from + still "frozen" by its counter; don't touch it further.
    }

    crd::set_assert_handler(prev_handler);
    crd::set_assert_platform_handler(prev_platform_handler);
}
#endif // CRD_ENABLE_ASSERTS
