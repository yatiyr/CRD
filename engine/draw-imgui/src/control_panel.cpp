// crd-draw-imgui -- control_panel impl (Phase 3.1 v1a-draw d4).

#include <crd/draw_imgui/control_panel.hpp>

#include <crd/draw/overlay_pass.hpp>
#include <crd/draw/renderer.hpp>
#include <crd/draw/theme.hpp>
#include <crd/draw/types.hpp>

#include <imgui.h>

namespace crd::draw_imgui
{
namespace
{
// Closed-enum index for the theme dropdown. Index 0 = active engine
// default; index 1 = a high-contrast preset useful for screenshots /
// presentations. Picker writes via crd::draw::set_theme.
struct NamedTheme
{
    const char*           label;
    crd::draw::DrawTheme  theme;
};

NamedTheme make_default_theme() noexcept
{
    return NamedTheme{"Default (Blender hues)", crd::draw::DrawTheme{}};
}

NamedTheme make_high_contrast_theme() noexcept
{
    crd::draw::DrawTheme t{};
    t.grid_primary       = {255, 255, 255, 255};
    t.grid_secondary     = {180, 180, 180, 200};
    t.grid_axis_x        = {255,   0,   0, 255};
    t.grid_axis_y        = {  0, 255,   0, 255};
    t.grid_axis_z        = {  0,   0, 255, 255};
    t.grid_primary_cell   = 1.0F;
    t.grid_secondary_cell = 0.25F;
    t.grid_fade_distance  = 50.0F;
    return NamedTheme{"High contrast", t};
}

// Cerid Category enum entries -- pinned at d4 close. New categories APPEND
// (keep these labels stable; the bit indices match Category enum values).
constexpr const char* kCategoryLabels[] = {
    "Physics",  "Audio",    "Sdf",      "Nav",
    "Scene",    "Renderer", "User0",    "User1",
    "User2",    "Debug",    "Gizmo",    "Brush",
};
constexpr int kCategoryCount = sizeof(kCategoryLabels) / sizeof(kCategoryLabels[0]);
} // namespace

void draw_control_panel(crd::draw::OverlayPassConfig& cfg)
{
    ImGui::Begin("Debug Draw", nullptr);

    // ------ Master enable + scale ------
    bool enabled = crd::draw::is_overlay_enabled();
    if (ImGui::Checkbox("overlay enabled", &enabled))
    {
        crd::draw::set_overlay_enabled(enabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("api v%u", static_cast<unsigned>(crd::draw::kDrawApiVersion));

    // ------ Category mask ------
    if (ImGui::CollapsingHeader("Categories", ImGuiTreeNodeFlags_DefaultOpen))
    {
        crd::u32 mask = cfg.category_mask;
        for (int i = 0; i < kCategoryCount; ++i)
        {
            const crd::u32 bit = 1U << static_cast<crd::u32>(i);
            bool on = (mask & bit) != 0U;
            if (ImGui::Checkbox(kCategoryLabels[i], &on))
            {
                if (on) { mask |=  bit; }
                else    { mask &= ~bit; }
            }
            // 4 columns of checkboxes
            if ((i % 4) != 3 && i + 1 < kCategoryCount) { ImGui::SameLine(); }
        }
        cfg.category_mask = mask;
        if (ImGui::Button("All"))  { cfg.category_mask = 0xFFFFFFFFU; }
        ImGui::SameLine();
        if (ImGui::Button("None")) { cfg.category_mask = 0U; }
    }

    // ------ Theme picker ------
    if (ImGui::CollapsingHeader("Theme"))
    {
        static const NamedTheme themes[] = {
            make_default_theme(),
            make_high_contrast_theme(),
        };
        static int selected = 0;
        const char* combo_preview = themes[selected].label;
        if (ImGui::BeginCombo("preset", combo_preview))
        {
            for (int i = 0; i < 2; ++i)
            {
                const bool is_selected = (selected == i);
                if (ImGui::Selectable(themes[i].label, is_selected))
                {
                    selected = i;
                    crd::draw::set_theme(themes[i].theme);
                    // Re-pull cell sizes + grid + axis colors into the
                    // live OverlayPassConfig so the change is visible in
                    // the same frame.
                    cfg.grid.apply_theme();
                }
                if (is_selected) { ImGui::SetItemDefaultFocus(); }
            }
            ImGui::EndCombo();
        }
    }

    // ------ Grid sub-panel ------
    if (ImGui::CollapsingHeader("Grid", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("grid enabled", &cfg.grid.enabled);
        ImGui::SliderFloat("plane_y",        &cfg.grid.plane_y,        -10.0F,  10.0F);
        ImGui::SliderFloat("primary_cell",   &cfg.grid.primary_cell,    0.05F, 10.0F);
        ImGui::SliderFloat("secondary_cell", &cfg.grid.secondary_cell,  0.01F,  5.0F);
        ImGui::SliderFloat("fade_distance",  &cfg.grid.fade_distance,   5.0F, 500.0F);
    }

    ImGui::End();
}

} // namespace crd::draw_imgui
