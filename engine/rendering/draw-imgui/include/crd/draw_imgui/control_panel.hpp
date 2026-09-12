#pragma once

// crd-draw-imgui -- ImGui control panel for crd-draw (Phase 3.1 v1a-draw d4).
//
// Peer module that depends on crd-draw + crd-imgui. Lives in its own
// target so headless / DAW builds that don't link ImGui aren't forced to
// drag it into crd-draw transitively (module isolation per CLAUDE.md).
//
// Public API: a single function that draws an `ImGui::Begin / End`-bracketed
// window exposing the master enable, master scale, per-category checkboxes,
// theme picker, and the grid sub-panel. Call it from inside an ImGui frame
// (between `ImGui::NewFrame()` and `ImGui::Render()`).
//
// Single-path: the panel mutates the runtime state it's handed (the master-
// enable global, the active theme, an OverlayPassConfig instance owned by
// the consumer). No callbacks, no observer registration; the caller owns
// the state, the panel just edits it.

namespace crd::draw
{
struct OverlayPassConfig;
}

namespace crd::draw_imgui
{
// Draw the "Debug Draw" control panel. Pass the `OverlayPassConfig` you
// hand to `add_draw_overlay_pass` each frame -- mutations from the panel
// (master-scale, category mask, grid params) take effect immediately on the
// next submitted frame.
//
// The master-enable toggle and theme picker write to the crd-draw process
// globals (`crd::draw::set_overlay_enabled`, `crd::draw::set_theme`); no
// state is owned by the panel itself.
void draw_control_panel(crd::draw::OverlayPassConfig& cfg);

} // namespace crd::draw_imgui
