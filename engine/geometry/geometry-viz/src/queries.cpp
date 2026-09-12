#include <crd/core/assert.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/geometry/viz/queries.hpp>

#include <cmath>

namespace crd::geometry::viz
{
namespace
{
using crd::f32;
using crd::math::Vec3f;
}

void draw_ray_hit(crd::draw::RenderBuffer& buf, const primitives::Ray3<f32>& ray, f32 t, Vec3f normal_dir,
                  f32 normal_length, crd::draw::Color ray_color, crd::draw::Color hit_color,
                  crd::draw::Color normal_color, f32 width_px, crd::draw::PrimFlags flags, f32 lifetime_s)
{
    const Vec3f hit_point(ray.origin.x + ray.direction.x * t, ray.origin.y + ray.direction.y * t,
                          ray.origin.z + ray.direction.z * t);
    // Ray segment from origin to hit.
    crd::draw::add_line_to(buf, ray.origin, hit_point, ray_color, width_px, flags, lifetime_s);
    // Small 3-axis cross at the hit.
    const f32 cross_size = normal_length * 0.4F;
    crd::draw::cross_3d_to(buf, hit_point, cross_size, hit_color, width_px, flags, lifetime_s);
    // Optional normal arrow.
    if (normal_dir.x != 0.0F || normal_dir.y != 0.0F || normal_dir.z != 0.0F)
    {
        crd::draw::arrow_to(buf, hit_point, normal_dir, normal_length, normal_color, 0.2F, 0.4F, width_px, flags,
                            lifetime_s);
    }
}

void draw_closest_point(crd::draw::RenderBuffer& buf, Vec3f query, Vec3f closest, crd::draw::Color color,
                        f32 endpoint_size, f32 width_px, crd::draw::PrimFlags flags, f32 lifetime_s)
{
    crd::draw::add_line_to(buf, query, closest, color, width_px, flags, lifetime_s);
    crd::draw::cross_3d_to(buf, query, endpoint_size, color, width_px, flags, lifetime_s);
    crd::draw::add_point_to(buf, closest, color, 6.0F, flags, lifetime_s);
}

void draw_normals(crd::draw::RenderBuffer& buf, crd::containers::ConstSpan<Vec3f> points,
                  crd::containers::ConstSpan<Vec3f> normals, f32 hair_length, crd::draw::Color color, f32 width_px,
                  crd::draw::PrimFlags flags, f32 lifetime_s)
{
    CRD_ASSERT(points.size() == normals.size());
    for (crd::usize i = 0; i < points.size(); ++i)
    {
        crd::draw::arrow_to(buf, points[i], normals[i], hair_length, color, 0.2F, 0.4F, width_px, flags, lifetime_s);
    }
}

} // namespace crd::geometry::viz
