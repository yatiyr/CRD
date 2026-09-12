#include <crd/geometry/viz/curves.hpp>

#include <crd/core/assert.hpp>

namespace crd::geometry::viz
{

namespace
{

template <crd::math::MathScalar T>
void draw_polyline_impl(crd::draw::RenderBuffer& buf, const curves::Polyline3View<T>& poly,
                        crd::draw::Color color, crd::f32 width_px,
                        crd::draw::PrimFlags flags, crd::f32 lifetime_s) noexcept
{
    const auto n = poly.points.size();
    if (n < 2U) { return; }

    const auto n_segs = poly.closed ? n : (n - 1U);
    for (crd::usize i = 0U; i < n_segs; ++i)
    {
        const auto& a_v = poly.points[i];
        const auto& b_v = poly.points[(i + 1U) % n];
        const crd::math::Vec3f a(static_cast<crd::f32>(a_v.x), static_cast<crd::f32>(a_v.y),
                                  static_cast<crd::f32>(a_v.z));
        const crd::math::Vec3f b(static_cast<crd::f32>(b_v.x), static_cast<crd::f32>(b_v.y),
                                  static_cast<crd::f32>(b_v.z));
        crd::draw::add_line_to(buf, a, b, color, width_px, flags, lifetime_s);
    }
}

} // namespace

void draw_polyline(crd::draw::RenderBuffer& buf, const curves::Polyline3View<crd::f32>& poly,
                   crd::draw::Color color, crd::f32 width_px,
                   crd::draw::PrimFlags flags, crd::f32 lifetime_s)
{
    draw_polyline_impl<crd::f32>(buf, poly, color, width_px, flags, lifetime_s);
}

void draw_polyline(crd::draw::RenderBuffer& buf, const curves::Polyline3View<crd::f64>& poly,
                   crd::draw::Color color, crd::f32 width_px,
                   crd::draw::PrimFlags flags, crd::f32 lifetime_s)
{
    draw_polyline_impl<crd::f64>(buf, poly, color, width_px, flags, lifetime_s);
}

void draw_rmf(crd::draw::RenderBuffer&                                                   buf,
              crd::containers::ConstSpan<curves::CurveFrame<crd::f32>>                   frames,
              crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>                      points,
              crd::f32 axis_len, crd::f32 width_px, crd::draw::PrimFlags flags,
              crd::f32 lifetime_s)
{
    CRD_ASSERT(frames.size() == points.size());
    const auto n = frames.size();
    for (crd::usize i = 0U; i < n; ++i)
    {
        const auto& p = points[i];
        const auto& f = frames[i];
        const crd::math::Vec3f t_end(p.x + f.tangent.x * axis_len,
                                      p.y + f.tangent.y * axis_len,
                                      p.z + f.tangent.z * axis_len);
        const crd::math::Vec3f n_end(p.x + f.normal.x * axis_len,
                                      p.y + f.normal.y * axis_len,
                                      p.z + f.normal.z * axis_len);
        const crd::math::Vec3f b_end(p.x + f.binormal.x * axis_len,
                                      p.y + f.binormal.y * axis_len,
                                      p.z + f.binormal.z * axis_len);
        crd::draw::add_line_to(buf, p, t_end, k_tangent_color, width_px, flags, lifetime_s);
        crd::draw::add_line_to(buf, p, n_end, k_normal_color, width_px, flags, lifetime_s);
        crd::draw::add_line_to(buf, p, b_end, k_binormal_color, width_px, flags, lifetime_s);
    }
}

} // namespace crd::geometry::viz
