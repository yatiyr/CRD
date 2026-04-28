#include <crd/config/config.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

TEST_CASE("Config loads simple TOML and returns typed values", "[config]")
{
    crd::config::Config cfg;
    REQUIRE(cfg.load_from_string("title = 'Cerid'\ncount = 7\nratio = 0.5\nenabled = true\n"));

    REQUIRE(cfg.get<crd::containers::String>("title", crd::containers::String{}) == "Cerid");
    REQUIRE(cfg.get<int>("count", 0) == 7);
    REQUIRE(std::fabs(cfg.get<crd::f32>("ratio", 0.0f) - 0.5f) < 0.0001f);
    REQUIRE(cfg.get<bool>("enabled", false));
}

TEST_CASE("Config missing keys use fallback", "[config]")
{
    crd::config::Config cfg;
    REQUIRE(cfg.load_from_string("answer = 42\n"));
    REQUIRE(cfg.get<int>("missing.key", 99) == 99);
}

TEST_CASE("Config type mismatch uses fallback", "[config]")
{
    crd::config::Config cfg;
    REQUIRE(cfg.load_from_string("value = 'hello'\n"));
    REQUIRE(cfg.get<int>("value", 12) == 12);
}

TEST_CASE("Config supports nested dot paths", "[config]")
{
    crd::config::Config cfg;
    REQUIRE(cfg.load_from_string("[imgui.theme]\npreset = 'dark'\n"));
    REQUIRE(cfg.get<crd::containers::String>("imgui.theme.preset", crd::containers::String{}) == "dark");
}

TEST_CASE("Config supports arrays and Vec4f", "[config]")
{
    crd::config::Config cfg;
    REQUIRE(cfg.load_from_string("nums = [1, 2, 3]\ncolor = [0.1, 0.2, 0.3, 1.0]\n"));

    const auto nums = cfg.get<crd::containers::Array<crd::i64>>("nums", crd::containers::Array<crd::i64>{});
    REQUIRE(nums.size() == 3u);
    REQUIRE(nums[0] == 1);
    REQUIRE(nums[2] == 3);

    const auto color = cfg.get<crd::math::Vec4f>("color", {});
    REQUIRE(std::fabs(color.x - 0.1f) < 0.0001f);
    REQUIRE(std::fabs(color.w - 1.0f) < 0.0001f);
}

TEST_CASE("Config set creates nested tables and roundtrips", "[config]")
{
    crd::config::Config cfg;
    cfg.set<int>("render.frames_in_flight", 2);
    cfg.set<crd::containers::String>("render.backend", crd::containers::String("vulkan"));

    REQUIRE(cfg.get<int>("render.frames_in_flight", 0) == 2);
    REQUIRE(cfg.get<crd::containers::String>("render.backend", crd::containers::String{}) == "vulkan");
}
