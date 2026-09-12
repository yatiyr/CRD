#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-viz -- Curve adapters. Phase 3.1.7 v10e (2026-05-19).
//
//   draw_polyline(buf, view)         -- emit (n - 1) line segments for open
//                                       polylines / n segments for closed.
//                                       Non-template overloads on f32 / f64.
//   draw_curve(buf, curve, n, alloc) -- sample the curve uniformly + emit
//                                       the resulting polyline. Generic
//                                       over curve kind.
//   draw_tangent_frame(buf, curve, n_samples, axis_len, alloc)
//                                    -- Frenet (T, N, B) at uniform t-samples,
//                                       three colour-coded lines per sample.
//   draw_rmf(buf, frames, points, axis_len)
//                                    -- Wang 2008 RMF frames at precomputed
//                                       sample points, three colour-coded
//                                       lines per frame.
//
// Curve types are templated, so the curve-consuming entry points are header-
// only inline templates. `draw_polyline` + `draw_rmf` are non-template (the
// types are concrete) and live in `curves.cpp`.
//
// Colour convention (matches the existing axis-triad triplet in crd-draw):
//   tangent  = red    (matches +X axis)
//   normal   = green  (matches +Y axis)
//   binormal = blue   (matches +Z axis)
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/draw/types.hpp>
#include <crd/geometry/curves/frames.hpp>
#include <crd/geometry/curves/polyline.hpp>
#include <crd/geometry/curves/sample.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>


namespace crd::geometry::viz
{

// Colour roles for tangent-frame visualisation. Pinned at v10e so every
// downstream consumer (sandbox, future editor) agrees.
inline constexpr crd::draw::Color k_tangent_color  = crd::draw::kRed;
inline constexpr crd::draw::Color k_normal_color   = crd::draw::kGreen;
inline constexpr crd::draw::Color k_binormal_color = crd::draw::kBlue;

// ---------------------------------------------------------------------------
// draw_polyline -- emit segment-by-segment lines for an existing polyline
// view. (n - 1) lines for open polylines + n lines for closed.
// ---------------------------------------------------------------------------

void draw_polyline(crd::draw::RenderBuffer& buf, const curves::Polyline3View<crd::f32>& poly,
                   crd::draw::Color color = crd::draw::kWhite, crd::f32 width_px = 1.0F,
                   crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F);

void draw_polyline(crd::draw::RenderBuffer& buf, const curves::Polyline3View<crd::f64>& poly,
                   crd::draw::Color color = crd::draw::kWhite, crd::f32 width_px = 1.0F,
                   crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// Helper for owning polylines -- forwards to the view overload.
template <crd::math::MathScalar T>
inline void draw_polyline(crd::draw::RenderBuffer& buf, const curves::Polyline3<T>& poly,
                          crd::draw::Color color = crd::draw::kWhite, crd::f32 width_px = 1.0F,
                          crd::draw::PrimFlags flags = crd::draw::kDefaultFlags,
                          crd::f32 lifetime_s = 0.0F)
{
    draw_polyline(buf,
                  curves::Polyline3View<T>{
                      crd::containers::ConstSpan<crd::math::Vec3<T>>{poly.points.data(), poly.points.size()},
                      poly.closed},
                  color, width_px, flags, lifetime_s);
}

// ---------------------------------------------------------------------------
// draw_curve -- sample the curve uniformly + emit the resulting polyline.
// Generic over the curve kind via `Curve::scalar_t`.
// ---------------------------------------------------------------------------

template <typename Curve>
inline void draw_curve(crd::draw::RenderBuffer& buf, const Curve& curve, crd::u32 n_segments,
                       crd::memory::IAllocator* alloc,
                       crd::draw::Color color = crd::draw::kWhite, crd::f32 width_px = 1.0F,
                       crd::draw::PrimFlags flags = crd::draw::kDefaultFlags,
                       crd::f32 lifetime_s = 0.0F)
{
    auto poly = curves::sample_uniform(curve, n_segments, alloc);
    draw_polyline(buf, poly, color, width_px, flags, lifetime_s);
}

// ---------------------------------------------------------------------------
// draw_tangent_frame -- Frenet (T, N, B) at uniform t-samples. Emits three
// lines per sample (one per axis) of length `axis_len`.
// ---------------------------------------------------------------------------

template <typename Curve>
inline void draw_tangent_frame(crd::draw::RenderBuffer&    buf,
                                const Curve&                curve,
                                crd::u32                    n_samples,
                                typename Curve::scalar_t    axis_len,
                                crd::f32                    width_px   = 1.0F,
                                crd::draw::PrimFlags        flags      = crd::draw::kDefaultFlags,
                                crd::f32                    lifetime_s = 0.0F)
{
    using T = typename Curve::scalar_t;
    if (n_samples == 0U) { return; }

    const bool closed = curve.closed;
    const auto count  = closed ? n_samples : (n_samples + 1U);
    for (crd::u32 i = 0U; i < count; ++i)
    {
        const T t   = static_cast<T>(i) / static_cast<T>(n_samples);
        const auto p   = curves::evaluate(curve, t);
        const auto t_h = curves::tangent(curve, t);
        const auto n_h = curves::normal(curve, t);
        const auto b_h = curves::binormal(curve, t);

        // Cast to f32 for the line emitter (crd-draw is f32-only).
        const crd::math::Vec3f p_f32(static_cast<crd::f32>(p.x), static_cast<crd::f32>(p.y),
                                      static_cast<crd::f32>(p.z));
        const crd::f32 ax = static_cast<crd::f32>(axis_len);
        const crd::math::Vec3f t_end(p_f32.x + static_cast<crd::f32>(t_h.x) * ax,
                                      p_f32.y + static_cast<crd::f32>(t_h.y) * ax,
                                      p_f32.z + static_cast<crd::f32>(t_h.z) * ax);
        const crd::math::Vec3f n_end(p_f32.x + static_cast<crd::f32>(n_h.x) * ax,
                                      p_f32.y + static_cast<crd::f32>(n_h.y) * ax,
                                      p_f32.z + static_cast<crd::f32>(n_h.z) * ax);
        const crd::math::Vec3f b_end(p_f32.x + static_cast<crd::f32>(b_h.x) * ax,
                                      p_f32.y + static_cast<crd::f32>(b_h.y) * ax,
                                      p_f32.z + static_cast<crd::f32>(b_h.z) * ax);
        crd::draw::add_line_to(buf, p_f32, t_end, k_tangent_color, width_px, flags, lifetime_s);
        crd::draw::add_line_to(buf, p_f32, n_end, k_normal_color, width_px, flags, lifetime_s);
        crd::draw::add_line_to(buf, p_f32, b_end, k_binormal_color, width_px, flags, lifetime_s);
    }
}

// ---------------------------------------------------------------------------
// draw_rmf -- emit precomputed RMF frames as colour-coded axis hairs at
// matching sample points. `frames.size()` MUST equal `points.size()`.
// ---------------------------------------------------------------------------

void draw_rmf(crd::draw::RenderBuffer&                                                   buf,
              crd::containers::ConstSpan<curves::CurveFrame<crd::f32>>                   frames,
              crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>                      points,
              crd::f32 axis_len   = 0.2F,
              crd::f32 width_px   = 1.0F,
              crd::draw::PrimFlags flags = crd::draw::kDefaultFlags,
              crd::f32 lifetime_s = 0.0F);

} // namespace crd::geometry::viz
