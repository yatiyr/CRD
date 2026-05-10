#pragma once

// crd-draw -- DrawTheme (Phase 3.1 v1a-draw d2-theme, ADR-0066 sec 19.2).
//
// A single struct of palette + grid sizing knobs that consumers (sandbox,
// editor, debug overlays) read instead of hardcoding colors. The defaults
// follow the Blender / RViz convention: X = red, Y = green, Z = blue,
// neutral grey grid, mid-grey subdivisions.
//
// Two consumption sites:
//   1. `OverlayPassConfig::GridConfig::apply_theme(theme)` -- copies grid
//      sizing + grid + axis colors into the per-frame overlay config.
//   2. Future debug-viz code can read `current_theme().body_dynamic` etc.
//      instead of the global `kBody*` constants in `types.hpp` -- enabling
//      project-wide palette overrides without recompiling shape callers.
//
// Theme is expected to be set ONCE at startup (e.g. from a TOML config
// loaded by the application). It is read every frame; concurrent writes
// during frame submit are not supported -- treat it as a `set-once,
// read-many` global.

#include <crd/core/types.hpp>
#include <crd/draw/types.hpp>

namespace crd::draw
{
struct DrawTheme
{
    // -- Grid (consumed by infinite-grid pipeline + GridConfig defaults) --
    // Axis hues match Blender's 3D View axis gizmo (see kAxisX/Y/Z in
    // types.hpp for the source citation).
    Color    grid_primary       = {200, 200, 200, 255};  // light grey -- 1m gridlines
    Color    grid_secondary     = {128, 128, 128, 128};  // mid grey   -- 0.25m subdivisions
    Color    grid_axis_x        = kAxisX;                 // Blender X (255, 51,  82)
    Color    grid_axis_y        = kAxisY;                 // Blender Y (139, 220,  0) -- side-grid (v1b+)
    Color    grid_axis_z        = kAxisZ;                 // Blender Z ( 40, 144, 255)
    crd::f32 grid_primary_cell   = 1.0F;
    crd::f32 grid_secondary_cell = 0.25F;
    crd::f32 grid_fade_distance  = 50.0F;

    // -- Semantic palette (debug-viz helpers may opt in by reading these) --
    // Defaults intentionally match the `kBody*` / `kContact*` literal
    // constants in types.hpp, so existing call sites that pass the literal
    // continue to render identically until they migrate.
    Color body_dynamic   = kBodyDynamic;
    Color body_static    = kBodyStatic;
    Color body_kinematic = kBodyKinematic;
    Color body_asleep    = kBodyAsleep;
    Color contact_point  = kContactPoint;
    Color contact_normal = kContactNormal;
    Color joint_frame    = kJointFrame;
    Color velocity_arrow = kVelocityArrow;
    Color aabb           = kAabb;
};

// Read the active theme (set once at startup; defaults to a fresh
// `DrawTheme{}` if `set_theme` was never called).
[[nodiscard]] const DrawTheme& current_theme() noexcept;

// Replace the active theme. Not thread-safe with concurrent reads --
// call once at startup or between frames.
void set_theme(const DrawTheme& theme) noexcept;

} // namespace crd::draw
