#pragma once

// crd-draw — overlay submission (RET-6, ADR-0105: re-founded on gpu-context; the frame-graph pass — ADR-0066 §9 —
// retired with the rhi renderer).
//
// `submit_overlay` composes the contents of a `RenderBuffer` ONTO an existing raster target through the
// `IRasterContext::draw_overlay` seam: color loadOp=LOAD (the scene stays), standard alpha blending, and a
// READ-ONLY depth test per primitive DepthMode when the target carries a depth buffer. Call it AFTER the scene has
// drawn into `target` — the overlay goes last, exactly where the frame-graph pass used to sit.
//
// The draw order preserves the rhi original's compose-on-top semantics bit for bit: the grid first (under
// everything), then triangles, then lines — each in Test → Always → GreaterDimmed variant order, with XRay
// primitives emitted TWICE (full color under Test: the visible portion; alpha-dimmed under GreaterDimmed: the
// occluded portion). Per ADR-0066 §19.1.
//
// Pre-conditions: `crd::draw::init(ctx, raster, ...)` once at startup; uninitialised or overlay-disabled calls
// no-op (return true) so consumers wire it unconditionally.

#include <crd/core/types.hpp>
#include <crd/draw/theme.hpp>
#include <crd/draw/types.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>

namespace crd::draw
{
class RenderBuffer;

struct OverlayPassConfig
{
    // Camera matrices used by the vertex shader to project endpoints: pass `projection * view`.
    crd::math::Mat4f view_proj{};

    // Viewport pixel dimensions — the screen-space quad expansion computes pixel-perfect line widths from these.
    // Match the target's size.
    crd::math::Vec2f viewport_px{1.0F, 1.0F};

    // Category-mask bitfield. Bit N enables Category::N. Default = all on. 0 renders nothing (the vertex shader
    // degenerates every primitive off-screen).
    crd::u32 category_mask = 0xFFFFFFFFU;

    // Wall-clock seconds for lifetime fade (d2+).
    crd::f32 time_s = 0.0F;

    // RET-6: the DepthMode::Test comparison under THIS scene's depth convention (standard-Z scenes: LessEqual —
    // visible when closer). GreaterDimmed uses the complement automatically; Always ignores depth. Only consulted
    // when `target` carries a depth buffer.
    crd::gpu::DepthCompare depth_test = crd::gpu::DepthCompare::LessEqual;

    // d2-grid: infinite faded floor grid (Blender / Unity-editor style). Renders before all primitives.
    // d2-theme: defaults are the engine baseline; call `apply_theme()` to pull sizes + colors from the DrawTheme.
    struct GridConfig
    {
        bool             enabled         = false;
        crd::math::Vec3f camera_pos      {0.0F, 0.0F, 0.0F}; // world-space camera xyz
        crd::f32         plane_y         = 0.0F;             // world-space Y of the grid plane
        crd::f32         primary_cell    = 1.0F;             // primary grid cell size (m)
        crd::f32         secondary_cell  = 0.25F;            // secondary cell size (m); set <= 0 to skip
        crd::u32         primary_color   = 0xFFC8C8C8U;      // packed RGBA8 (default light grey)
        crd::u32         secondary_color = 0x80808080U;      // packed RGBA8 (default mid grey, half alpha)
        crd::u32         axis_x_color    = 0xFF5233FFU;      // packed RGBA8 (Blender X = 255,51,82, line at z=0)
        crd::u32         axis_z_color    = 0xFFFF9028U;      // packed RGBA8 (Blender Z = 40,144,255, line at x=0)
        crd::f32         fade_distance   = 50.0F;            // alpha = 0 at this XZ distance

        // Pull cell sizes + grid colors + axis colors from the active theme. Leaves `enabled`, `camera_pos` and
        // `plane_y` untouched (per-frame / per-scene, not themable). Returns *this for chaining.
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

// Compose `buffer` (+ the grid, when enabled) over `target`'s existing contents. Returns false only on a REAL
// failure (an upload or draw refused); uninitialised / disabled / empty submissions are successful no-ops.
[[nodiscard]] bool submit_overlay(crd::gpu::IRasterTarget& target, const RenderBuffer& buffer,
                                  const OverlayPassConfig& config);

} // namespace crd::draw
