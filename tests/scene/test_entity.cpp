#include <crd/scene/entity.hpp>

#include <catch2/catch_test_macros.hpp>
#include <limits>

using crd::scene::EntityId;

TEST_CASE("EntityId default-constructs to null", "[scene][entity]")
{
    EntityId e{};
    CHECK(e.is_null());
    CHECK(e.raw == 0);
    CHECK(e == EntityId::null());
    CHECK(EntityId::null().is_null());
}

TEST_CASE("EntityId::make round-trips through index() and generation()", "[scene][entity]")
{
    SECTION("zeroes")
    {
        EntityId e = EntityId::make(0, 0);
        CHECK(e.index() == 0U);
        CHECK(e.generation() == 0U);
        CHECK(e.is_null()); // raw == 0 → null
    }

    SECTION("typical values")
    {
        EntityId e = EntityId::make(42U, 7U);
        CHECK(e.index() == 42U);
        CHECK(e.generation() == 7U);
        CHECK_FALSE(e.is_null());
    }

    SECTION("max u32 boundaries")
    {
        constexpr crd::u32 max32 = std::numeric_limits<crd::u32>::max();
        EntityId e1 = EntityId::make(max32, 0U);
        CHECK(e1.index() == max32);
        CHECK(e1.generation() == 0U);

        EntityId e2 = EntityId::make(0U, max32);
        CHECK(e2.index() == 0U);
        CHECK(e2.generation() == max32);

        EntityId e3 = EntityId::make(max32, max32);
        CHECK(e3.index() == max32);
        CHECK(e3.generation() == max32);
    }
}

TEST_CASE("EntityId equality and inequality", "[scene][entity]")
{
    EntityId a = EntityId::make(5, 1);
    EntityId b = EntityId::make(5, 1);
    EntityId c = EntityId::make(5, 2); // different generation
    EntityId d = EntityId::make(6, 1); // different index

    CHECK(a == b);
    CHECK_FALSE(a == c);
    CHECK_FALSE(a == d);
    CHECK(a != c);
    CHECK(a != d);
    CHECK_FALSE(a != b);
}

TEST_CASE("EntityId is_null returns true only when raw == 0", "[scene][entity]")
{
    CHECK(EntityId{}.is_null());
    CHECK(EntityId{0}.is_null());
    CHECK_FALSE(EntityId::make(1, 0).is_null()); // index 1, gen 0
    CHECK_FALSE(EntityId::make(0, 1).is_null()); // index 0, gen 1
    CHECK_FALSE(EntityId{static_cast<crd::u64>(-1)}.is_null());
}

TEST_CASE("EntityId is trivially copyable and 8 bytes", "[scene][entity]")
{
    STATIC_REQUIRE(sizeof(EntityId) == 8);
    STATIC_REQUIRE(std::is_trivially_copyable_v<EntityId>);
}
