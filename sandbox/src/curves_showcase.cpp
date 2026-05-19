// Sandbox curves showcase -- Phase 3.1.7 v10e.
//
// Visualises every `crd-geometry-curves` curve kind interactively. See
// curves_showcase.hpp for the design contract.

#include "curves_showcase.hpp"

#include <crd/containers/span.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/draw/types.hpp>
#include <crd/geometry/viz/curves.hpp>

#include <imgui.h>

#include <cmath>

namespace crd::sandbox
{
namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::math::Vec3f;
namespace curves = crd::geometry::curves;
namespace viz    = crd::geometry::viz;

// Compact RGBA red for control-point markers.
constexpr crd::draw::Color kCpColor    = {255U, 200U, 60U, 255U};
constexpr crd::draw::Color kCurveColor = crd::draw::kWhite;

// Emit one 3D cross at `p` of half-extent `size`. Used to mark every
// control point so the user can SEE the handles even before the gizmo
// cluster lands (filed in docs/debt.md).
void cross_at(crd::draw::RenderBuffer& buf, const Vec3f& p, f32 size, f32 width_px) noexcept
{
    crd::draw::add_line_to(buf, Vec3f(p.x - size, p.y, p.z), Vec3f(p.x + size, p.y, p.z),
                            kCpColor, width_px);
    crd::draw::add_line_to(buf, Vec3f(p.x, p.y - size, p.z), Vec3f(p.x, p.y + size, p.z),
                            kCpColor, width_px);
    crd::draw::add_line_to(buf, Vec3f(p.x, p.y, p.z - size), Vec3f(p.x, p.y, p.z + size),
                            kCpColor, width_px);
}

// Sample n_samples + 1 points along the curve into a stack array (max 256
// samples in the showcase to keep the per-frame line budget bounded).
template <typename Curve>
crd::u32 sample_points(const Curve& curve, u32 n_samples, crd::math::Vec3f* out_buf,
                        crd::u32 buf_cap) noexcept
{
    using T = typename Curve::scalar_t;
    const bool closed = curve.closed;
    const u32  count  = closed ? n_samples : (n_samples + 1U);
    const u32  n      = (count <= buf_cap) ? count : buf_cap;
    for (u32 i = 0U; i < n; ++i)
    {
        const T t = static_cast<T>(i) / static_cast<T>(n_samples);
        const auto p = curves::evaluate(curve, t);
        out_buf[i].x = static_cast<f32>(p.x);
        out_buf[i].y = static_cast<f32>(p.y);
        out_buf[i].z = static_cast<f32>(p.z);
    }
    return n;
}

// Emit RMF frames if requested + the per-frame axis hairs.
template <typename Curve>
void emit_frames(crd::draw::RenderBuffer& buf, const Curve& curve, u32 n_samples,
                  f32 axis_len, f32 width_px, ShowcaseFrameMode mode,
                  crd::memory::IAllocator& alloc) noexcept
{
    if (mode == ShowcaseFrameMode::Off || n_samples == 0U) { return; }

    if (mode == ShowcaseFrameMode::Frenet)
    {
        viz::draw_tangent_frame(buf, curve, n_samples, static_cast<typename Curve::scalar_t>(axis_len),
                                  width_px);
        return;
    }

    // RMF path: precompute frames + their sample points, then forward to
    // viz::draw_rmf.
    auto frames = curves::compute_rmf(curve, n_samples, &alloc);
    const auto n_frames = static_cast<u32>(frames.size());
    Vec3f pts[257];
    const u32 n_pts = sample_points(curve, n_samples, pts, 257U);
    const u32 n     = n_pts < n_frames ? n_pts : n_frames;
    // Cast Vec3<T> -> Vec3f at the boundary (T may be f64).
    crd::geometry::curves::CurveFrame<f32> frames_f32[257];
    for (u32 i = 0U; i < n; ++i)
    {
        frames_f32[i].tangent  = Vec3f(static_cast<f32>(frames[i].tangent.x),
                                        static_cast<f32>(frames[i].tangent.y),
                                        static_cast<f32>(frames[i].tangent.z));
        frames_f32[i].normal   = Vec3f(static_cast<f32>(frames[i].normal.x),
                                        static_cast<f32>(frames[i].normal.y),
                                        static_cast<f32>(frames[i].normal.z));
        frames_f32[i].binormal = Vec3f(static_cast<f32>(frames[i].binormal.x),
                                        static_cast<f32>(frames[i].binormal.y),
                                        static_cast<f32>(frames[i].binormal.z));
    }
    viz::draw_rmf(buf,
                  crd::containers::ConstSpan<crd::geometry::curves::CurveFrame<f32>>(frames_f32, n),
                  crd::containers::ConstSpan<Vec3f>(pts, n), axis_len, width_px);
}

// Emit a single control-point cluster: cross markers + dashed connector
// segments between consecutive points (the "control polygon" hint). Helps
// the user see WHICH point they're dragging when there are 6 of them.
void emit_control_polygon(crd::draw::RenderBuffer& buf, const Vec3f* pts, u32 n_pts,
                          bool draw_polygon, f32 cp_size, f32 width_px) noexcept
{
    for (u32 i = 0U; i < n_pts; ++i)
    {
        cross_at(buf, pts[i], cp_size, width_px);
    }
    if (draw_polygon)
    {
        for (u32 i = 0U; i + 1U < n_pts; ++i)
        {
            crd::draw::add_line_to(buf, pts[i], pts[i + 1U], crd::draw::kGrey,
                                    width_px * 0.5F);
        }
    }
}

} // namespace

void render_curves_showcase(CurvesShowcaseState& state, crd::draw::RenderBuffer& buf,
                             crd::memory::IAllocator& alloc)
{
    const f32 wp = state.line_width;

    switch (state.kind)
    {
    case ShowcaseCurveKind::Polyline:
    {
        curves::Polyline3View<f32> p{
            crd::containers::ConstSpan<Vec3f>(state.polyline_pts, 6U),
            state.polyline_closed};
        viz::draw_polyline(buf, p, kCurveColor, wp);
        emit_frames(buf, p, state.n_samples, state.frame_axis_len, wp, state.frame_mode, alloc);
        if (state.show_control_points)
        {
            emit_control_polygon(buf, state.polyline_pts, 6U, false, state.control_point_size, wp);
        }
        break;
    }
    case ShowcaseCurveKind::QuadBezier:
    {
        curves::QuadBezier3<f32> q{state.quad_p0, state.quad_p1, state.quad_p2};
        viz::draw_curve(buf, q, state.n_samples, &alloc, kCurveColor, wp);
        emit_frames(buf, q, state.n_samples, state.frame_axis_len, wp, state.frame_mode, alloc);
        if (state.show_control_points)
        {
            const Vec3f cps[] = {state.quad_p0, state.quad_p1, state.quad_p2};
            emit_control_polygon(buf, cps, 3U, true, state.control_point_size, wp);
        }
        break;
    }
    case ShowcaseCurveKind::CubicBezier:
    {
        curves::CubicBezier3<f32> c{state.cubic_p0, state.cubic_p1, state.cubic_p2, state.cubic_p3};
        viz::draw_curve(buf, c, state.n_samples, &alloc, kCurveColor, wp);
        emit_frames(buf, c, state.n_samples, state.frame_axis_len, wp, state.frame_mode, alloc);
        if (state.show_control_points)
        {
            const Vec3f cps[] = {state.cubic_p0, state.cubic_p1, state.cubic_p2, state.cubic_p3};
            emit_control_polygon(buf, cps, 4U, true, state.control_point_size, wp);
        }
        break;
    }
    case ShowcaseCurveKind::CubicHermite:
    {
        curves::CubicHermite3<f32> h{state.hermite_p0, state.hermite_p1, state.hermite_t0,
                                      state.hermite_t1};
        viz::draw_curve(buf, h, state.n_samples, &alloc, kCurveColor, wp);
        emit_frames(buf, h, state.n_samples, state.frame_axis_len, wp, state.frame_mode, alloc);
        if (state.show_control_points)
        {
            cross_at(buf, state.hermite_p0, state.control_point_size, wp);
            cross_at(buf, state.hermite_p1, state.control_point_size, wp);
            const Vec3f t0_end(state.hermite_p0.x + state.hermite_t0.x,
                                state.hermite_p0.y + state.hermite_t0.y,
                                state.hermite_p0.z + state.hermite_t0.z);
            const Vec3f t1_end(state.hermite_p1.x + state.hermite_t1.x,
                                state.hermite_p1.y + state.hermite_t1.y,
                                state.hermite_p1.z + state.hermite_t1.z);
            crd::draw::add_line_to(buf, state.hermite_p0, t0_end, crd::draw::kGrey, wp * 0.5F);
            crd::draw::add_line_to(buf, state.hermite_p1, t1_end, crd::draw::kGrey, wp * 0.5F);
        }
        break;
    }
    case ShowcaseCurveKind::CatmullRom:
    {
        curves::CatmullRom3<f32> c(&alloc,
                                    crd::containers::ConstSpan<Vec3f>(state.catmull_pts, 6U),
                                    state.catmull_param, state.catmull_closed);
        viz::draw_curve(buf, c, state.n_samples, &alloc, kCurveColor, wp);
        emit_frames(buf, c, state.n_samples, state.frame_axis_len, wp, state.frame_mode, alloc);
        if (state.show_control_points)
        {
            emit_control_polygon(buf, state.catmull_pts, 6U, true, state.control_point_size, wp);
        }
        break;
    }
    case ShowcaseCurveKind::BSpline:
    {
        auto b = curves::BSpline3<f32>::make_uniform_open(
            &alloc, crd::containers::ConstSpan<Vec3f>(state.bspline_pts, 6U));
        viz::draw_curve(buf, b, state.n_samples, &alloc, kCurveColor, wp);
        emit_frames(buf, b, state.n_samples, state.frame_axis_len, wp, state.frame_mode, alloc);
        if (state.show_control_points)
        {
            emit_control_polygon(buf, state.bspline_pts, 6U, true, state.control_point_size, wp);
        }
        break;
    }
    case ShowcaseCurveKind::CircularArc:
    {
        curves::CircularArc3<f32> a{state.arc_center, state.arc_axis_u, state.arc_axis_v,
                                     state.arc_radius, state.arc_sweep_radians, state.arc_closed};
        viz::draw_curve(buf, a, state.n_samples, &alloc, kCurveColor, wp);
        emit_frames(buf, a, state.n_samples, state.frame_axis_len, wp, state.frame_mode, alloc);
        if (state.show_control_points)
        {
            cross_at(buf, state.arc_center, state.control_point_size, wp);
        }
        break;
    }
    case ShowcaseCurveKind::EllipseArc:
    {
        curves::EllipseArc3<f32> e{state.ellipse_center, state.ellipse_axis_u,
                                    state.ellipse_axis_v, state.ellipse_radius_u,
                                    state.ellipse_radius_v, state.ellipse_sweep_radians,
                                    state.ellipse_closed};
        viz::draw_curve(buf, e, state.n_samples, &alloc, kCurveColor, wp);
        emit_frames(buf, e, state.n_samples, state.frame_axis_len, wp, state.frame_mode, alloc);
        if (state.show_control_points)
        {
            cross_at(buf, state.ellipse_center, state.control_point_size, wp);
        }
        break;
    }
    case ShowcaseCurveKind::Helix:
    {
        // Generate helix control points on the fly + sample via Catmull-Rom
        // centripetal. The cinematic-camera demo curve.
        const u32 n_loops    = state.helix_loops == 0U ? 1U : state.helix_loops;
        const u32 per_loop   = state.helix_samples_per_loop == 0U ? 8U : state.helix_samples_per_loop;
        const u32 n_pts      = n_loops * per_loop + 1U;
        const u32 cap        = 257U;
        const u32 use_pts    = n_pts > cap ? cap : n_pts;
        Vec3f pts[257];
        for (u32 i = 0U; i < use_pts; ++i)
        {
            const f32 theta = static_cast<f32>(i) * (6.2831853F / static_cast<f32>(per_loop));
            pts[i] = Vec3f(state.helix_radius * std::cos(theta),
                           state.helix_pitch * static_cast<f32>(i)
                               / static_cast<f32>(per_loop),
                           state.helix_radius * std::sin(theta));
        }
        curves::CatmullRom3<f32> c(&alloc, crd::containers::ConstSpan<Vec3f>(pts, use_pts),
                                    curves::CatmullRomParam::Centripetal, false);
        viz::draw_curve(buf, c, state.n_samples, &alloc, kCurveColor, wp);
        emit_frames(buf, c, state.n_samples, state.frame_axis_len, wp, state.frame_mode, alloc);
        if (state.show_control_points)
        {
            emit_control_polygon(buf, pts, use_pts, true, state.control_point_size * 0.5F,
                                  wp * 0.5F);
        }
        break;
    }
    }
}

void draw_curves_showcase_imgui(CurvesShowcaseState& state)
{
    static const char* k_kind_names[] = {"Polyline", "QuadBezier", "CubicBezier",
                                          "CubicHermite", "CatmullRom", "BSpline",
                                          "CircularArc", "EllipseArc", "Helix"};
    int kind = static_cast<int>(state.kind);
    if (ImGui::Combo("Curve kind", &kind, k_kind_names, IM_ARRAYSIZE(k_kind_names)))
    {
        state.kind = static_cast<ShowcaseCurveKind>(kind);
    }
    int n = static_cast<int>(state.n_samples);
    if (ImGui::SliderInt("n_samples", &n, 4, 256))
    {
        state.n_samples = static_cast<crd::u32>(n);
    }
    static const char* k_frame_modes[] = {"Off", "Frenet", "RMF (Wang 2008)"};
    int fm = static_cast<int>(state.frame_mode);
    if (ImGui::Combo("frame mode", &fm, k_frame_modes, IM_ARRAYSIZE(k_frame_modes)))
    {
        state.frame_mode = static_cast<ShowcaseFrameMode>(fm);
    }
    ImGui::SliderFloat("frame axis len", &state.frame_axis_len, 0.05F, 1.0F);
    ImGui::SliderFloat("control point size", &state.control_point_size, 0.01F, 0.4F);
    ImGui::Checkbox("show control points", &state.show_control_points);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ImGui DragFloat3 control-point editing is the temporary surface;\n"
                          "3D-viewport drag gizmos ship with the future UI cluster (see\n"
                          "docs/debt.md -> Future cluster -- direct-manipulation UX).");
    }

    ImGui::Separator();
    switch (state.kind)
    {
    case ShowcaseCurveKind::Polyline:
        for (crd::u32 i = 0U; i < 6U; ++i)
        {
            char label[16];
            std::snprintf(label, sizeof(label), "pt[%u]", i);
            ImGui::DragFloat3(label, &state.polyline_pts[i].x, 0.05F);
        }
        ImGui::Checkbox("closed", &state.polyline_closed);
        break;
    case ShowcaseCurveKind::QuadBezier:
        ImGui::DragFloat3("p0", &state.quad_p0.x, 0.05F);
        ImGui::DragFloat3("p1", &state.quad_p1.x, 0.05F);
        ImGui::DragFloat3("p2", &state.quad_p2.x, 0.05F);
        break;
    case ShowcaseCurveKind::CubicBezier:
        ImGui::DragFloat3("p0", &state.cubic_p0.x, 0.05F);
        ImGui::DragFloat3("p1", &state.cubic_p1.x, 0.05F);
        ImGui::DragFloat3("p2", &state.cubic_p2.x, 0.05F);
        ImGui::DragFloat3("p3", &state.cubic_p3.x, 0.05F);
        break;
    case ShowcaseCurveKind::CubicHermite:
        ImGui::DragFloat3("p0", &state.hermite_p0.x, 0.05F);
        ImGui::DragFloat3("p1", &state.hermite_p1.x, 0.05F);
        ImGui::DragFloat3("t0 (tangent)", &state.hermite_t0.x, 0.05F);
        ImGui::DragFloat3("t1 (tangent)", &state.hermite_t1.x, 0.05F);
        break;
    case ShowcaseCurveKind::CatmullRom:
        for (crd::u32 i = 0U; i < 6U; ++i)
        {
            char label[16];
            std::snprintf(label, sizeof(label), "pt[%u]", i);
            ImGui::DragFloat3(label, &state.catmull_pts[i].x, 0.05F);
        }
        ImGui::Checkbox("closed", &state.catmull_closed);
        {
            static const char* k_param[] = {"Uniform", "Centripetal (Yuksel 2011)"};
            int p = static_cast<int>(state.catmull_param);
            if (ImGui::Combo("parameterisation", &p, k_param, IM_ARRAYSIZE(k_param)))
            {
                state.catmull_param =
                    static_cast<crd::geometry::curves::CatmullRomParam>(p);
            }
        }
        break;
    case ShowcaseCurveKind::BSpline:
        for (crd::u32 i = 0U; i < 6U; ++i)
        {
            char label[16];
            std::snprintf(label, sizeof(label), "pt[%u]", i);
            ImGui::DragFloat3(label, &state.bspline_pts[i].x, 0.05F);
        }
        break;
    case ShowcaseCurveKind::CircularArc:
        ImGui::DragFloat3("center", &state.arc_center.x, 0.05F);
        ImGui::DragFloat3("axis_u", &state.arc_axis_u.x, 0.05F);
        ImGui::DragFloat3("axis_v", &state.arc_axis_v.x, 0.05F);
        ImGui::SliderFloat("radius", &state.arc_radius, 0.1F, 5.0F);
        ImGui::SliderFloat("sweep (rad)", &state.arc_sweep_radians, 0.0F, 6.2831853F);
        ImGui::Checkbox("closed", &state.arc_closed);
        break;
    case ShowcaseCurveKind::EllipseArc:
        ImGui::DragFloat3("center", &state.ellipse_center.x, 0.05F);
        ImGui::DragFloat3("axis_u", &state.ellipse_axis_u.x, 0.05F);
        ImGui::DragFloat3("axis_v", &state.ellipse_axis_v.x, 0.05F);
        ImGui::SliderFloat("radius_u", &state.ellipse_radius_u, 0.1F, 5.0F);
        ImGui::SliderFloat("radius_v", &state.ellipse_radius_v, 0.1F, 5.0F);
        ImGui::SliderFloat("sweep (rad)", &state.ellipse_sweep_radians, 0.0F, 6.2831853F);
        ImGui::Checkbox("closed", &state.ellipse_closed);
        break;
    case ShowcaseCurveKind::Helix:
    {
        int loops = static_cast<int>(state.helix_loops);
        if (ImGui::SliderInt("loops", &loops, 1, 8))
        {
            state.helix_loops = static_cast<crd::u32>(loops);
        }
        ImGui::SliderFloat("radius", &state.helix_radius, 0.1F, 3.0F);
        ImGui::SliderFloat("pitch (y per loop)", &state.helix_pitch, 0.05F, 2.0F);
        int spl = static_cast<int>(state.helix_samples_per_loop);
        if (ImGui::SliderInt("control pts per loop", &spl, 4, 24))
        {
            state.helix_samples_per_loop = static_cast<crd::u32>(spl);
        }
        break;
    }
    }
}

} // namespace crd::sandbox
