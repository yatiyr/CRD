#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::scene::EntityId;
using crd::scene::World;

TEST_CASE("Fresh World is empty", "[scene][world]")
{
    World w;
    CHECK(w.entity_count() == 0U);
    CHECK(w.pending_destroy_count() == 0U);
    CHECK_FALSE(w.is_alive(EntityId::null()));
}

TEST_CASE("spawn produces alive entity", "[scene][world]")
{
    World w;
    EntityId e = w.spawn();
    CHECK_FALSE(e.is_null());
    CHECK(w.is_alive(e));
    CHECK(w.entity_count() == 1U);
}

TEST_CASE("destroy is deferred until flush_destroys", "[scene][world]")
{
    World w;
    EntityId e = w.spawn();
    REQUIRE(w.is_alive(e));

    w.destroy(e);
    CHECK(w.is_alive(e)); // still alive — destroy is deferred
    CHECK(w.entity_count() == 1U);
    CHECK(w.pending_destroy_count() == 1U);

    w.flush_destroys();
    CHECK_FALSE(w.is_alive(e));
    CHECK(w.entity_count() == 0U);
    CHECK(w.pending_destroy_count() == 0U);
}

TEST_CASE("flush_destroys drains all queued handles", "[scene][world]")
{
    World w;
    EntityId a = w.spawn();
    EntityId b = w.spawn();
    EntityId c = w.spawn();
    REQUIRE(w.entity_count() == 3U);

    w.destroy(a);
    w.destroy(c);
    CHECK(w.pending_destroy_count() == 2U);
    CHECK(w.is_alive(a));
    CHECK(w.is_alive(b));
    CHECK(w.is_alive(c));

    w.flush_destroys();
    CHECK_FALSE(w.is_alive(a));
    CHECK(w.is_alive(b));
    CHECK_FALSE(w.is_alive(c));
    CHECK(w.entity_count() == 1U);
}

TEST_CASE("destroy_immediate frees synchronously", "[scene][world]")
{
    World w;
    EntityId e = w.spawn();
    REQUIRE(w.is_alive(e));

    w.destroy_immediate(e);
    CHECK_FALSE(w.is_alive(e));
    CHECK(w.entity_count() == 0U);
    CHECK(w.pending_destroy_count() == 0U);
}

TEST_CASE("destroy_immediate of stale handle is a no-op", "[scene][world]")
{
    World w;
    EntityId e = w.spawn();
    w.destroy_immediate(e);
    REQUIRE_FALSE(w.is_alive(e));

    // Calling again must not assert or corrupt state.
    w.destroy_immediate(e);
    CHECK(w.entity_count() == 0U);
}

TEST_CASE("Double destroy of the same handle is safe across flush", "[scene][world]")
{
    World w;
    EntityId e = w.spawn();
    w.destroy(e);
    w.destroy(e);
    CHECK(w.pending_destroy_count() == 2U);

    w.flush_destroys();
    CHECK(w.entity_count() == 0U);
    CHECK_FALSE(w.is_alive(e));
}

TEST_CASE("Iteration after destroy/flush only yields surviving entities", "[scene][world]")
{
    World w;
    EntityId a = w.spawn();
    EntityId b = w.spawn();
    EntityId c = w.spawn();
    w.destroy(b);
    w.flush_destroys();

    int count = 0;
    bool saw_a = false;
    bool saw_c = false;
    for (EntityId e : w)
    {
        ++count;
        if (e == a)
            saw_a = true;
        if (e == c)
            saw_c = true;
        CHECK_FALSE(e == b);
    }
    CHECK(count == 2);
    CHECK(saw_a);
    CHECK(saw_c);
}
