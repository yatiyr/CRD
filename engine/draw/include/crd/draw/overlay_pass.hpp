#pragma once

// crd-draw -- overlay-pass helper (Phase 3.1 v1a-draw d0c, ADR-0066 sec 9).
//
// `add_draw_overlay_pass` registers a frame-graph pass that consumes a
// scene color attachment (write, alpha-blended) and an optional scene
// depth attachment (read), then renders every line in the supplied
// `RenderBuffer` as an anti-aliased screen-space quad.
//
// Call from inside your IRenderPath::build() AFTER all your scene-render
// passes have been declared. The overlay pass goes last so it composes on
// top of the rendered scene.
//
// Pre-conditions:
//   - `crd::draw::init(rm, device, ...)` has been called once at startup.
//   - `is_initialised()` returns true. (If not, the function is a no-op
//     so consumers can wire it unconditionally during development.)
//
// d0c renders LINES only. Triangles + points + glyphs land in d1+.

#include <crd/core/types.hpp>
#include <crd/draw/theme.hpp>
#include <crd/draw/types.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>
#include <crd/renderer/frame_graph.hpp>

namespace crd::draw
{
class RenderBuffer;

struct OverlayPassConfig
{
    // Camera matrices used by the vertex shader to project endpoints.
    // Pass `ctx.camera.projection * ctx.camera.view` from your IRenderPath.
    crd::math::Mat4f view_proj{};

    // Viewport pixel dimensions. Used by the screen-space quad expansion
    // to compute pixel-perfect line widths regardless of camera distance.
    // Match the size of the scene_color attachment.
    crd::math::Vec2f viewport_px{1.0F, 1.0F};

    // Frame-in-flight index (0..frames_in_flight-1). Selects which slot
    // of the per-frame instance buffer ring to write to. Typically
    // `ctx.frame_index % frames_in_flight`.
    crd::u32 frame_in_flight_index = 0;

    // Category-mask bitfield. Bit N enables Category::N. Default = all on.
    // Set to 0 to render nothing (the pass still runs, but every primitive
    // is degenerate'd off-screen by the vertex shader).
    crd::u32 category_mask = 0xFFFFFFFFU;

    // Wall-clock seconds for lifetime fade. Pass an accumulating timer.
    // Used by the fragment shader's lifetime decay (d2+; ignored in d0c).
    crd::f32 time_s = 0.0F;

    // d2-grid: infinite faded floor grid (Blender / Unity-editor style).
    // Renders before all primitives so lines/triangles compose on top.
    // When `grid.enabled = false` the grid pipeline is skipped entirely.
    //
    // d2-theme: defaults are the engine baseline; call `apply_theme()` to
    // pull cell sizes + colors from the active `DrawTheme`.
    struct GridConfig
    {
        bool             enabled         = false;
        crd::math::Vec3f camera_pos      {0.0F, 0.0F, 0.0F}; // world-space camera xyz
        crd::f32         plane_y         = 0.0F;             // world-space Y of the grid plane
        crd::f32         primary_cell    = 1.0F;             // primary grid cell size (m)
        crd::f32         secondary_cell  = 0.25F;             // secondary cell size (m); set <= 0 to skip
        crd::u32         primary_color   = 0xFFC8C8C8U;      // packed RGBA8 (default light grey)
        crd::u32         secondary_color = 0x80808080U;      // packed RGBA8 (default mid grey, half alpha)
        crd::u32         axis_x_color    = 0xFF5233FFU;      // packed RGBA8 (Blender X = 255,51,82, line at z=0)
        crd::u32         axis_z_color    = 0xFFFF9028U;      // packed RGBA8 (Blender Z = 40,144,255, line at x=0)
        crd::f32         fade_distance   = 50.0F;            // alpha = 0 at this XZ distance

        // Pull cell sizes + grid colors + axis colors from the active theme.
        // Leaves `enabled`, `camera_pos` and `plane_y` untouched (those are
        // per-frame / per-scene rather than themable). Returns *this so it
        // can be chained.
        GridConfig& apply_theme(const DrawTheme& theme = current_theme()) noexcept
        {
            primary_cell    = theme.grid_primary_cell;
            secondary_cell  = theme.grid_secondary_cell;
            fade_distance   = theme.grid_fade_distance;
            primary_color   = theme.grid_primary.packed_rgba();
            secondary_color = theme.grid_secondary.packed_rgba();
            axis_x_color    = theme.grid_axis_x.packed_rgba();
            axis_z_color    = theme.grid_axis_z.packed_rgba();
            return *this;
        }
    };
    GridConfig grid{};
};

// Add the overlay pass. The pass `name` becomes the frame-graph pass name
// (handy for capture-tool labelling).
void add_draw_overlay_pass(crd::renderer::FrameGraph&  fg,
                           crd::renderer::ImageHandle  scene_color,
                           crd::renderer::ImageHandle  scene_depth,
                           const RenderBuffer&         buffer,
                           const OverlayPassConfig&    config,
                           const char*                 name = "draw-overlay");

} // namespace crd::draw
