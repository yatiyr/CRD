// crd::containers sort + heap algorithm tests — Phase 3.1 v0d.
//
// Two tiers:
//   1. Functional correctness — output is sorted, n-th element correct,
//      heap invariants hold.
//   2. Deterministic stability — sorting random inputs from MULTIPLE
//      starting permutations gives identical output (bit-exact across
//      configs is verified by the 12-config sweep).

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>

#include <algorithm>
#include <functional>

using crd::i32;
using crd::u32;
using crd::usize;

// ===========================================================================
// sort + stable_sort
// ===========================================================================

TEST_CASE("sort empty + single", "[sort]")
{
    crd::containers::Array<i32> empty;
    crd::containers::sort(empty.data(), empty.data() + empty.size());

    crd::containers::Array<i32> single = { 7 };
    crd::containers::sort(single.data(), single.data() + single.size());
    REQUIRE(single[0] == 7);
}

TEST_CASE("sort small in-place", "[sort]")
{
    crd::containers::Array<i32> a = { 5, 2, 8, 1, 9, 3, 7, 4, 6, 0 };
    crd::containers::sort(a.data(), a.data() + a.size());
    for (usize i = 0; i + 1 < a.size(); ++i) REQUIRE(a[i] <= a[i + 1]);
}

TEST_CASE("sort large random ascending+descending+already-sorted", "[sort]")
{
    constexpr usize k_n = 200;
    crd::containers::Array<i32> ascending;      ascending.resize(k_n);
    crd::containers::Array<i32> descending;     descending.resize(k_n);
    crd::containers::Array<i32> already_sorted; already_sorted.resize(k_n);
    crd::containers::Array<i32> shuffled;       shuffled.resize(k_n);
    for (usize i = 0; i < k_n; ++i)
    {
        ascending[i]      = static_cast<i32>(i);
        descending[i]     = static_cast<i32>(k_n - i);
        already_sorted[i] = static_cast<i32>(i);
        shuffled[i]       = static_cast<i32>((i * 31 + 7) % k_n);
    }

    crd::containers::sort(ascending.data(),      ascending.data() + ascending.size());
    crd::containers::sort(descending.data(),     descending.data() + descending.size());
    crd::containers::sort(already_sorted.data(), already_sorted.data() + already_sorted.size());
    crd::containers::sort(shuffled.data(),       shuffled.data() + shuffled.size());

    for (usize i = 0; i + 1 < k_n; ++i)
    {
        REQUIRE(ascending[i]      <= ascending[i + 1]);
        REQUIRE(descending[i]     <= descending[i + 1]);
        REQUIRE(already_sorted[i] <= already_sorted[i + 1]);
        REQUIRE(shuffled[i]       <= shuffled[i + 1]);
    }
}

TEST_CASE("sort with custom comparator (descending)", "[sort]")
{
    crd::containers::Array<i32> a = { 5, 2, 8, 1, 9, 3 };
    crd::containers::sort(a.data(), a.data() + a.size(), std::greater<i32>{});
    for (usize i = 0; i + 1 < a.size(); ++i) REQUIRE(a[i] >= a[i + 1]);
}

TEST_CASE("stable_sort preserves relative order for equal keys", "[sort][stable]")
{
    struct Item { i32 key; i32 original_index; };
    crd::containers::Array<Item> a = {
        {3, 0}, {1, 1}, {3, 2}, {2, 3}, {1, 4}, {3, 5}, {2, 6}
    };
    crd::containers::stable_sort(a.data(), a.data() + a.size(),
        [](const Item& x, const Item& y) { return x.key < y.key; }, a.allocator());

    // After sort: keys are 1,1,2,2,3,3,3
    // Within each key, original_index must be ascending (stability).
    REQUIRE(a[0].key == 1); REQUIRE(a[0].original_index == 1);
    REQUIRE(a[1].key == 1); REQUIRE(a[1].original_index == 4);
    REQUIRE(a[2].key == 2); REQUIRE(a[2].original_index == 3);
    REQUIRE(a[3].key == 2); REQUIRE(a[3].original_index == 6);
    REQUIRE(a[4].key == 3); REQUIRE(a[4].original_index == 0);
    REQUIRE(a[5].key == 3); REQUIRE(a[5].original_index == 2);
    REQUIRE(a[6].key == 3); REQUIRE(a[6].original_index == 5);
}

TEST_CASE("stable_sort reusable-scratch overload matches + reuses buffer", "[sort][stable]")
{
    struct Item { i32 key; i32 original_index; };
    crd::containers::Array<Item> a = {
        {3, 0}, {1, 1}, {3, 2}, {2, 3}, {1, 4}, {3, 5}, {2, 6}
    };
    crd::containers::Array<Item> scratch(a.allocator());
    auto cmp = [](const Item& x, const Item& y) { return x.key < y.key; };
    crd::containers::stable_sort(a.data(), a.data() + a.size(), cmp, scratch);  // scratch overload
    REQUIRE(a[0].key == 1); REQUIRE(a[0].original_index == 1);
    REQUIRE(a[1].original_index == 4);
    REQUIRE(a[4].key == 3); REQUIRE(a[4].original_index == 0);
    // Reuse the same scratch on a second sort (no per-call alloc; must still be correct).
    crd::containers::Array<Item> b = { {2, 0}, {1, 1}, {2, 2}, {1, 3} };
    crd::containers::stable_sort(b.data(), b.data() + b.size(), cmp, scratch);
    REQUIRE(b[0].original_index == 1);
    REQUIRE(b[1].original_index == 3);
    REQUIRE(b[2].original_index == 0);
    REQUIRE(b[3].original_index == 2);
}

TEST_CASE("sort and stable_sort agree on all-equal keys", "[sort][stable]")
{
    crd::containers::Array<i32> all_same_a = { 7, 7, 7, 7, 7, 7, 7, 7 };
    crd::containers::Array<i32> all_same_b = { 7, 7, 7, 7, 7, 7, 7, 7 };
    crd::containers::sort(all_same_a.data(), all_same_a.data() + all_same_a.size());
    crd::containers::stable_sort(all_same_b.data(), all_same_b.data() + all_same_b.size(), all_same_b.allocator());
    REQUIRE(all_same_a == all_same_b);
}

// ===========================================================================
// nth_element
// ===========================================================================

TEST_CASE("nth_element places nth correctly", "[sort][nth]")
{
    crd::containers::Array<i32> a = { 5, 2, 8, 1, 9, 3, 7, 4, 6, 0 };
    const usize nth = 4;
    crd::containers::nth_element(a.data(), a.data() + nth, a.data() + a.size());

    // a[nth] should be the 4th smallest = 4 (sorted: 0,1,2,3,4,5,6,7,8,9).
    REQUIRE(a[nth] == 4);
    // Left side ≤ a[nth].
    for (usize i = 0; i < nth; ++i) REQUIRE(a[i] <= a[nth]);
    // Right side ≥ a[nth].
    for (usize i = nth + 1; i < a.size(); ++i) REQUIRE(a[i] >= a[nth]);
}

TEST_CASE("nth_element on large random input", "[sort][nth]")
{
    constexpr usize k_n = 200;
    crd::containers::Array<i32> a; a.resize(k_n);
    for (usize i = 0; i < k_n; ++i) a[i] = static_cast<i32>((i * 31 + 11) % k_n);

    const usize nth = k_n / 3;
    crd::containers::nth_element(a.data(), a.data() + nth, a.data() + k_n);
    for (usize i = 0; i < nth; ++i)        REQUIRE(a[i] <= a[nth]);
    for (usize i = nth + 1; i < k_n; ++i)   REQUIRE(a[i] >= a[nth]);
}

// ===========================================================================
// Heap operations
// ===========================================================================

TEST_CASE("make_heap + pop_heap (sort_heap) sorts correctly", "[sort][heap]")
{
    crd::containers::Array<i32> a = { 5, 2, 8, 1, 9, 3, 7, 4, 6, 0 };
    crd::containers::make_heap(a.data(), a.data() + a.size());

    // After make_heap, the root is the max.
    REQUIRE(a[0] == 9);
    // Heap invariant: every parent ≥ children.
    for (usize i = 0; i < a.size() / 2; ++i)
    {
        const usize l = 2 * i + 1;
        const usize r = l + 1;
        if (l < a.size()) REQUIRE(a[i] >= a[l]);
        if (r < a.size()) REQUIRE(a[i] >= a[r]);
    }

    crd::containers::sort_heap(a.data(), a.data() + a.size());
    for (usize i = 0; i + 1 < a.size(); ++i) REQUIRE(a[i] <= a[i + 1]);
}

TEST_CASE("push_heap maintains invariant", "[sort][heap]")
{
    crd::containers::Array<i32> a = { 9, 7, 8, 5, 6, 3, 4, 1, 2 };
    crd::containers::make_heap(a.data(), a.data() + a.size());

    a.push_back(10);
    crd::containers::push_heap(a.data(), a.data() + a.size());
    REQUIRE(a[0] == 10);  // 10 is the new max

    a.push_back(0);
    crd::containers::push_heap(a.data(), a.data() + a.size());
    REQUIRE(a[0] == 10);  // 0 doesn't displace
}

TEST_CASE("pop_heap repeatedly extracts max", "[sort][heap]")
{
    crd::containers::Array<i32> a = { 5, 2, 8, 1, 9, 3, 7, 4, 6 };
    crd::containers::make_heap(a.data(), a.data() + a.size());

    crd::containers::Array<i32> popped;
    while (a.size() > 0)
    {
        crd::containers::pop_heap(a.data(), a.data() + a.size());
        popped.push_back(a[a.size() - 1]);
        a.pop_back();
    }

    // popped = [9, 8, 7, 6, 5, 4, 3, 2, 1] (descending; max-heap order)
    for (usize i = 0; i + 1 < popped.size(); ++i)
    {
        REQUIRE(popped[i] >= popped[i + 1]);
    }
}

TEST_CASE("make_heap with custom comparator (min-heap)", "[sort][heap]")
{
    crd::containers::Array<i32> a = { 5, 2, 8, 1, 9, 3, 7, 4, 6, 0 };
    crd::containers::make_heap(a.data(), a.data() + a.size(), std::greater<i32>{});

    // With greater<>, the heap is a MIN-heap: root is smallest.
    REQUIRE(a[0] == 0);
}

// ===========================================================================
// Determinism cross-checks
// ===========================================================================

TEST_CASE("sort is deterministic across input permutations of same multiset",
          "[sort][determinism]")
{
    // Two different permutations of the same multiset should produce
    // identical sorted output (under our stable contract).
    crd::containers::Array<i32> perm1 = { 3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5 };
    crd::containers::Array<i32> perm2 = { 5, 9, 1, 5, 2, 3, 6, 4, 3, 1, 5 };
    REQUIRE(perm1.size() == perm2.size());

    crd::containers::sort(perm1.data(), perm1.data() + perm1.size());
    crd::containers::sort(perm2.data(), perm2.data() + perm2.size());

    REQUIRE(perm1 == perm2);
}

TEST_CASE("Sort vs std::sort: same elements, in-order check",
          "[sort][determinism]")
{
    constexpr usize k_n = 200;
    crd::containers::Array<i32> a; a.resize(k_n);
    crd::containers::Array<i32> b; b.resize(k_n);
    for (usize i = 0; i < k_n; ++i) { a[i] = static_cast<i32>((i * 17 + 5) % 100); b[i] = a[i]; }

    crd::containers::sort(a.data(), a.data() + a.size());
    std::sort(b.data(), b.data() + b.size());  // crd-lint-allow-std-math

    // Both must contain the same multiset; both are sorted.
    REQUIRE(a == b);
}
