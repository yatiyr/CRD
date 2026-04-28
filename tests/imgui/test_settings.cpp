#include <crd/config/config.hpp>
#include <crd/imgui/settings.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ImGui settings load from config with expected defaults", "[imgui][config]")
{
    crd::config::Config cfg;
    REQUIRE(
        cfg.load_from_string("[imgui]\ndocking = true\nshow_demo_window = false\n[imgui.theme]\npreset = 'classic'\n"));

    const auto settings = crd::imgui::load_settings(cfg);
    REQUIRE(settings.docking);
    REQUIRE_FALSE(settings.show_demo_window);
    REQUIRE(settings.theme_preset == "classic");
    REQUIRE_FALSE(settings.multi_viewport);
}
