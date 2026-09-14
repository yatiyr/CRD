// DIAG.3e -- generation-checked handles: stale handles and reused slots are rejected.
// Contract: docs/design/runtime-diagnostics.md#diag-3e.

#include <crd/containers/slot_map.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::containers::SlotMap;

// A move-only, resource-owning payload: `live_counter` counts owners, and move TRANSFERS
// ownership (the moved-from shell no longer owns), so the counter tracks live resources rather
// than lingering shells. This proves SlotMap needs only move (no copy, no default ctor) and that
// erase releases the resource.
struct MoveOnly
{
    int  value;
    int* live_counter;

    explicit MoveOnly(int v, int* counter) : value(v), live_counter(counter) { ++(*live_counter); }
    MoveOnly(const MoveOnly&)            = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&& o) noexcept : value(o.value), live_counter(o.live_counter) { o.live_counter = nullptr; }
    MoveOnly& operator=(MoveOnly&& o) noexcept
    {
        if (this != &o)
        {
            if (live_counter)
                --(*live_counter);
            value          = o.value;
            live_counter   = o.live_counter;
            o.live_counter = nullptr;
        }
        return *this;
    }
    ~MoveOnly()
    {
        if (live_counter)
            --(*live_counter);
    }
};
} // namespace

TEST_CASE("slot_map: insert, get and a stale handle after erase", "[containers][slot_map][diag]")
{
    SlotMap<int> m;
    CHECK(m.empty());

    const auto h = m.insert(42);
    CHECK(h.valid());
    CHECK(m.size() == 1U);
    REQUIRE(m.get(h) != nullptr);
    CHECK(*m.get(h) == 42);
    CHECK(m.contains(h));

    CHECK(m.erase(h));
    CHECK(m.get(h) == nullptr); // the handle is now stale
    CHECK_FALSE(m.contains(h));
    CHECK(m.empty());

    CHECK_FALSE(m.erase(h)); // double-erase rejected, not corruption
}

TEST_CASE("slot_map: a reused slot invalidates the old handle", "[containers][slot_map][diag]")
{
    SlotMap<int> m;

    const auto a = m.insert(1);
    CHECK(m.erase(a));

    // The next insert reuses a's slot index but at a bumped generation.
    const auto b = m.insert(2);
    CHECK(b.index == a.index);      // same physical slot
    CHECK(b.generation != a.generation);

    CHECK_FALSE(m.contains(a));     // the old handle does NOT resurrect the reused slot
    CHECK(m.get(a) == nullptr);
    REQUIRE(m.get(b) != nullptr);
    CHECK(*m.get(b) == 2);
}

TEST_CASE("slot_map: the null/default handle is always rejected", "[containers][slot_map][diag]")
{
    SlotMap<int> m;
    SlotMap<int>::Handle null{};
    CHECK_FALSE(null.valid());
    CHECK_FALSE(m.contains(null));
    CHECK(m.get(null) == nullptr);
    CHECK_FALSE(m.erase(null));

    // A handle for an index that does not exist yet is rejected too.
    SlotMap<int>::Handle bogus{999U, 1U};
    CHECK_FALSE(m.contains(bogus));
    CHECK(m.get(bogus) == nullptr);
}

TEST_CASE("slot_map: many live handles stay independently valid", "[containers][slot_map][diag]")
{
    SlotMap<int> m;
    SlotMap<int>::Handle hs[16];
    for (int i = 0; i < 16; ++i)
        hs[i] = m.insert(i * 10);
    CHECK(m.size() == 16U);

    // Erase the evens; odds remain valid, evens go stale.
    for (int i = 0; i < 16; i += 2)
        CHECK(m.erase(hs[i]));
    CHECK(m.size() == 8U);
    for (int i = 0; i < 16; ++i)
    {
        if (i % 2 == 0)
            CHECK(m.get(hs[i]) == nullptr);
        else
            CHECK((m.get(hs[i]) != nullptr && *m.get(hs[i]) == i * 10));
    }
}

TEST_CASE("slot_map: move-only payloads are stored and destroyed exactly once", "[containers][slot_map][diag]")
{
    int live = 0;
    {
        SlotMap<MoveOnly> m;
        const auto        h = m.insert(MoveOnly{7, &live});
        REQUIRE(m.get(h) != nullptr);
        CHECK(m.get(h)->value == 7);
        CHECK(live == 1); // exactly one live instance after the temporary is gone

        CHECK(m.erase(h));
        CHECK(live == 0); // erase ran the destructor
        CHECK(m.get(h) == nullptr);
    }
    CHECK(live == 0); // no leak on SlotMap destruction
}
