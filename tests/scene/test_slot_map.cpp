#include <crd/containers/array.hpp>
#include <crd/scene/slot_map.hpp>

#include <catch2/catch_test_macros.hpp>
#include <random>

using crd::scene::EntityId;
using crd::scene::SlotMap;

TEST_CASE("Fresh SlotMap has no alive entities", "[scene][slot_map]")
{
    SlotMap map;
    CHECK(map.alive_count() == 0U);
    CHECK_FALSE(map.is_alive(EntityId::null()));
    CHECK_FALSE(map.is_alive(EntityId{}));
    CHECK(map.begin() == map.end());
}

TEST_CASE("allocate produces alive handle, never index 0", "[scene][slot_map]")
{
    SlotMap map;

    EntityId a = map.allocate();
    CHECK(a.index() != 0U);
    CHECK_FALSE(a.is_null());
    CHECK(map.is_alive(a));
    CHECK(map.alive_count() == 1U);

    EntityId b = map.allocate();
    CHECK(b.index() != 0U);
    CHECK(b.index() != a.index());
    CHECK(map.is_alive(b));
    CHECK(map.alive_count() == 2U);
}

TEST_CASE("free invalidates the stale handle (generation collision)", "[scene][slot_map]")
{
    SlotMap map;
    EntityId e = map.allocate();
    REQUIRE(map.is_alive(e));

    map.free(e);

    CHECK_FALSE(map.is_alive(e)); // same handle now stale
    CHECK(map.alive_count() == 0U);
}

TEST_CASE("allocate after free reuses index with bumped generation", "[scene][slot_map]")
{
    SlotMap map;
    EntityId first = map.allocate();
    const crd::u32 first_index = first.index();
    const crd::u32 first_gen = first.generation();

    map.free(first);
    EntityId second = map.allocate();

    CHECK(second.index() == first_index);    // free-list LIFO returns same slot
    CHECK(second.generation() != first_gen); // generation must differ
    CHECK(map.is_alive(second));
    CHECK_FALSE(map.is_alive(first)); // stale handle still invalid
    CHECK(map.alive_count() == 1U);
}

TEST_CASE("free-list LIFO order preserved across multi-step alloc/free", "[scene][slot_map]")
{
    SlotMap map;
    EntityId a = map.allocate();
    EntityId b = map.allocate();
    EntityId c = map.allocate();
    REQUIRE(map.alive_count() == 3U);

    map.free(b);
    map.free(a);
    REQUIRE(map.alive_count() == 1U);
    REQUIRE(map.is_alive(c));

    // Free list head was a (most-recent free); next should be b.
    EntityId reused1 = map.allocate();
    EntityId reused2 = map.allocate();

    CHECK(reused1.index() == a.index());
    CHECK(reused2.index() == b.index());
    CHECK(map.alive_count() == 3U);
}

TEST_CASE("Mixed allocate/free preserves invariants", "[scene][slot_map]")
{
    SlotMap map;
    crd::containers::Array<EntityId> live;
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed) — deterministic seed for test repro
    std::mt19937 rng{42};

    constexpr int steps = 1000;
    int expected_alive = 0;

    for (int i = 0; i < steps; ++i)
    {
        const bool do_free = (live.size() > 0) && (rng() % 3 == 0);
        if (do_free)
        {
            const auto idx = static_cast<crd::usize>(rng() % live.size());
            EntityId e = live[idx];
            REQUIRE(map.is_alive(e));
            map.free(e);
            CHECK_FALSE(map.is_alive(e));
            live.swap_remove(idx);
            --expected_alive;
        }
        else
        {
            EntityId e = map.allocate();
            CHECK(e.index() != 0U);
            CHECK(map.is_alive(e));
            live.push_back(e);
            ++expected_alive;
        }
        REQUIRE(map.alive_count() == static_cast<crd::u32>(expected_alive));
    }

    // Every surviving handle should still resolve.
    for (EntityId e : live)
    {
        CHECK(map.is_alive(e));
    }
}

TEST_CASE("Iterator yields alive entities only", "[scene][slot_map]")
{
    SlotMap map;
    EntityId a = map.allocate();
    EntityId b = map.allocate();
    EntityId c = map.allocate();
    map.free(b); // create a hole

    crd::containers::Array<EntityId> seen;
    for (EntityId e : map)
    {
        seen.push_back(e);
    }

    CHECK(seen.size() == 2U);
    CHECK(map.alive_count() == 2U);

    bool saw_a = false;
    bool saw_c = false;
    for (EntityId e : seen)
    {
        if (e == a)
            saw_a = true;
        if (e == c)
            saw_c = true;
        CHECK_FALSE(e == b);
    }
    CHECK(saw_a);
    CHECK(saw_c);
}

TEST_CASE("Slot 0 is reserved as the null sentinel", "[scene][slot_map]")
{
    SlotMap map;
    CHECK_FALSE(map.is_alive(EntityId{0}));
    CHECK_FALSE(map.is_alive(EntityId::null()));
    CHECK_FALSE(map.is_alive(EntityId::make(0, 1))); // bogus gen on slot 0

    // After many allocations, slot 0 still never alive.
    for (int i = 0; i < 16; ++i)
    {
        (void)map.allocate();
    }
    CHECK_FALSE(map.is_alive(EntityId::make(0, 0)));
    CHECK_FALSE(map.is_alive(EntityId::make(0, 99)));
}

TEST_CASE("Out-of-range index yields dead lookup", "[scene][slot_map]")
{
    SlotMap map;
    (void)map.allocate();

    CHECK_FALSE(map.is_alive(EntityId::make(9999U, 1U)));
    CHECK_FALSE(map.is_alive(EntityId::make(std::numeric_limits<crd::u32>::max(), 1U)));
}
