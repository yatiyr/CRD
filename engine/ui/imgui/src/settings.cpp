#include <crd/imgui/settings.hpp>

namespace crd::imgui
{
Settings load_settings(const crd::config::Config& config) noexcept
{
    Settings settings;
    settings.docking = config.get<bool>("imgui.docking", true);
    settings.multi_viewport = config.get<bool>("imgui.multi_viewport", false);
    settings.show_demo_window = config.get<bool>("imgui.show_demo_window", true);
    settings.show_metrics_window = config.get<bool>("imgui.show_metrics_window", false);
    settings.show_stats_panel = config.get<bool>("imgui.show_stats_panel", true);
    settings.theme_preset = config.get<crd::containers::String>("imgui.theme.preset", crd::containers::String("dark"));
    return settings;
}
} // namespace crd::imgui
