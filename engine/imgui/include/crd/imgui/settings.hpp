#pragma once

#include <crd/config/config.hpp>

namespace crd::imgui
{
struct Settings
{
    bool docking = true;
    bool multi_viewport = false;
    bool show_demo_window = true;
    bool show_metrics_window = false;
    bool show_stats_panel = true;
    crd::containers::String theme_preset{"dark"};
};

[[nodiscard]] Settings load_settings(const crd::config::Config& config) noexcept;
} // namespace crd::imgui
