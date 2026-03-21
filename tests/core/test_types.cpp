#include <catch2/catch_test_macros.hpp>
#include <crd/core/types.hpp>

TEST_CASE("Type sizes are correct", "[core][types]")
{
    REQUIRE(sizeof(crd::i8)  == 1);
    REQUIRE(sizeof(crd::i16) == 2);
    REQUIRE(sizeof(crd::i32) == 4);
    REQUIRE(sizeof(crd::i64) == 8);

    REQUIRE(sizeof(crd::f32) == 4);
    REQUIRE(sizeof(crd::f64) == 8);
}