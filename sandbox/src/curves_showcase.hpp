#pragma once

// ---------------------------------------------------------------------------
// Sandbox curves showcase -- Phase 3.1.7 v10e.
//
// Fourth top-level scene (selectable via the same ImGui "Scene" dropdown that
// gates Physics / GeometryViz / DrawShowcase). Visualises every
// `crd-geometry-curves` curve kind with:
//
//   - Per-curve-kind picker (Polyline / QuadBezier / CubicBezier /
//     CubicHermite / CatmullRom / BSpline / CircularArc / EllipseArc).
//   - Per-kind control-point editor (ImGui DragFloat3 sliders -- see
//     `project_gizmos_direct_manipulation_cluster.md`; 3D-viewport drag
//     gizmos are filed for the future UI cluster).
//   - Sample-count slider (controls the resolution of the rendered curve).
//   - Frame mode toggle: Off / Frenet (per-sample tangent/normal/binormal
//     hairs) / RMF (Wang 2008 rotation-minimising frame walk).
//   - Closed-flag toggle (kinds that support it).
//   - Catmull-Rom: Uniform / Centripetal parameterisation toggle.
//
// Design rules (mirror geometry_showcase.hpp):
//   * Pure data + pure functions. `CurvesShowcaseState` lives on
//     `SandboxLayer`; `render_curves_showcase` emits into the caller's
//     `RenderBuffer`. Allocator is passed in (eylem TLSF).
//   * Switching curve kinds preserves per-kind tuning.
//   * Default parameters are chosen so every kind renders something
//     visible inside the 4096-line per-frame draw budget at sample
//     count <= 64.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/geometry/curves/curves.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::sandbox
{

// Curve kind picker. Matches `crd::geometry::curves::CurveKind3` order, plus
// `Helix` as a CatmullRom-backed convenience for the canonical helix demo
// the cinematic-camera consumer pulls on.
enum class ShowcaseCurveKind : crd::u8
{
    Polyline      = 0,
    QuadBezier    = 1,
    CubicBezier   = 2,
    CubicHermite  = 3,
    CatmullRom    = 4,
    BSpline       = 5,
    CircularArc   = 6,
    EllipseArc    = 7,
    Helix         = 8,
};

// Frame visualisation mode.
enum class ShowcaseFrameMode : crd::u8
{
    Off    = 0,
    Frenet = 1,
    Rmf    = 2,
};

// Per-curve-kind parameter state. Lives inline on SandboxLayer so freely
// switching kinds preserves tunings. Defaults chosen to land a visible
// curve in the default camera framing.
struct CurvesShowcaseState
{
    ShowcaseCurveKind kind         = ShowcaseCurveKind::CubicBezier;
    crd::u32          n_samples    = 32U;
    ShowcaseFrameMode frame_mode   = ShowcaseFrameMode::Off;
    crd::f32          frame_axis_len = 0.25F;
    crd::f32          line_width   = 2.0F;
    bool              show_control_points = true;
    crd::f32          control_point_size  = 0.06F;

    // ---- Polyline ----
    // 6 control points along a gentle S-curve.
    crd::math::Vec3f polyline_pts[6] = {
        crd::math::Vec3f(-2.0F, 0.0F, 0.0F),
        crd::math::Vec3f(-1.0F, 0.5F, 0.0F),
        crd::math::Vec3f(0.0F, -0.5F, 0.0F),
        crd::math::Vec3f(1.0F, 0.5F, 0.0F),
        crd::math::Vec3f(2.0F, -0.5F, 0.0F),
        crd::math::Vec3f(3.0F, 0.0F, 0.0F),
    };
    bool polyline_closed = false;

    // ---- QuadBezier3 ----
    crd::math::Vec3f quad_p0 = crd::math::Vec3f(-2.0F, 0.0F, 0.0F);
    crd::math::Vec3f quad_p1 = crd::math::Vec3f(0.0F, 2.0F, 0.0F);
    crd::math::Vec3f quad_p2 = crd::math::Vec3f(2.0F, 0.0F, 0.0F);

    // ---- CubicBezier3 ----
    crd::math::Vec3f cubic_p0 = crd::math::Vec3f(-2.0F, 0.0F, 0.0F);
    crd::math::Vec3f cubic_p1 = crd::math::Vec3f(-1.0F, 1.5F, 0.0F);
    crd::math::Vec3f cubic_p2 = crd::math::Vec3f(1.0F, -1.5F, 0.0F);
    crd::math::Vec3f cubic_p3 = crd::math::Vec3f(2.0F, 0.0F, 0.0F);

    // ---- CubicHermite3 ----
    crd::math::Vec3f hermite_p0 = crd::math::Vec3f(-2.0F, 0.0F, 0.0F);
    crd::math::Vec3f hermite_p1 = crd::math::Vec3f(2.0F, 0.0F, 0.0F);
    crd::math::Vec3f hermite_t0 = crd::math::Vec3f(2.0F, 2.0F, 0.0F);
    crd::math::Vec3f hermite_t1 = crd::math::Vec3f(2.0F, -2.0F, 0.0F);

    // ---- CatmullRom3 ----
    // 6 control points, same S-curve shape as polyline default.
    crd::math::Vec3f catmull_pts[6] = {
        crd::math::Vec3f(-2.0F, 0.0F, 0.0F),
        crd::math::Vec3f(-1.0F, 0.8F, 0.0F),
        crd::math::Vec3f(0.0F, -0.8F, 0.0F),
        crd::math::Vec3f(1.0F, 0.8F, 0.0F),
        crd::math::Vec3f(2.0F, -0.8F, 0.0F),
        crd::math::Vec3f(3.0F, 0.0F, 0.0F),
    };
    bool                                       catmull_closed = false;
    crd::geometry::curves::CatmullRomParam catmull_param  =
        crd::geometry::curves::CatmullRomParam::Centripetal;

    // ---- BSpline3 ----
    // 6 control points; uniform-open knot vector built at render time.
    crd::math::Vec3f bspline_pts[6] = {
        crd::math::Vec3f(-2.0F, 0.0F, 0.0F),
        crd::math::Vec3f(-1.0F, 1.0F, 0.0F),
        crd::math::Vec3f(0.0F, 0.0F, 0.0F),
        crd::math::Vec3f(1.0F, -1.0F, 0.0F),
        crd::math::Vec3f(2.0F, 0.0F, 0.0F),
        crd::math::Vec3f(3.0F, 1.0F, 0.0F),
    };

    // ---- CircularArc3 ----
    crd::math::Vec3f arc_center = crd::math::Vec3f(0.0F, 0.0F, 0.0F);
    crd::math::Vec3f arc_axis_u = crd::math::Vec3f(1.0F, 0.0F, 0.0F);
    crd::math::Vec3f arc_axis_v = crd::math::Vec3f(0.0F, 0.0F, 1.0F);
    crd::f32         arc_radius        = 1.5F;
    crd::f32         arc_sweep_radians = 6.2831853F; // 2pi -- full circle
    bool             arc_closed        = true;

    // ---- EllipseArc3 ----
    crd::math::Vec3f ellipse_center = crd::math::Vec3f(0.0F, 0.0F, 0.0F);
    crd::math::Vec3f ellipse_axis_u = crd::math::Vec3f(1.0F, 0.0F, 0.0F);
    crd::math::Vec3f ellipse_axis_v = crd::math::Vec3f(0.0F, 0.0F, 1.0F);
    crd::f32         ellipse_radius_u      = 2.0F;
    crd::f32         ellipse_radius_v      = 1.0F;
    crd::f32         ellipse_sweep_radians = 6.2831853F;
    bool             ellipse_closed        = true;

    // ---- Helix (CatmullRom-backed) ----
    crd::u32 helix_loops   = 3U;
    crd::f32 helix_radius  = 1.0F;
    crd::f32 helix_pitch   = 0.5F;  // y advance per loop
    crd::u32 helix_samples_per_loop = 12U;
};

// Emit the current curve into `buf` (curve itself + optional frame hairs +
// optional control-point markers). Uses `alloc` for any short-lived scratch
// (sampled polyline, RMF frame array). The function does not retain the
// allocator beyond the call.
void render_curves_showcase(CurvesShowcaseState& state, crd::draw::RenderBuffer& buf,
                             crd::memory::IAllocator& alloc);

// Render the ImGui control panel for the showcase. Mutates `state`.
void draw_curves_showcase_imgui(CurvesShowcaseState& state);

} // namespace crd::sandbox
