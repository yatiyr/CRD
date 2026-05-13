#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-viz — query-result visualisations (Phase 3.1.7 v1j-a).
//
// These wrap query *outputs* (a ray hit, a closest-point pair, a set of
// normals) into recognisable on-screen debug primitives. The geometry side
// returns numbers; the viz side turns those numbers into pictures.
//
// Composes with `primitives.hpp` and `bvh.hpp`: a typical sandbox cell looks
// like "draw the input shape + the query result" — `viz::draw(buf, ray);
// viz::draw_ray_hit(buf, ray, hit->t, normal);`.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/draw/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::viz
{
// Ray-hit visualization: the ray as a coloured segment from origin to
// `origin + dir * t` plus a small 3-axis cross at the hit point. If
// `normal` is set, an arrow of length `normal_length` is emitted from the
// hit point along that normal.
//
// `ray.direction` need not be unit; `t` is the ray-parameter the underlying
// query (raycast / shapecast) returned, matching the v1i convention.
void draw_ray_hit(crd::draw::RenderBuffer& buf, const primitives::Ray3<crd::f32>& ray, crd::f32 t,
                  crd::math::Vec3f normal_dir = crd::math::Vec3f(0.0F, 0.0F, 0.0F), crd::f32 normal_length = 0.5F,
                  crd::draw::Color ray_color = crd::draw::kYellow, crd::draw::Color hit_color = crd::draw::kRed,
                  crd::draw::Color normal_color = crd::draw::kCyan, crd::f32 width_px = 1.0F,
                  crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// Closest-point pair visualization: a segment from `query` to `closest`,
// plus an endpoint marker at each end (small cross at `query`, point at
// `closest`).
void draw_closest_point(crd::draw::RenderBuffer& buf, crd::math::Vec3f query, crd::math::Vec3f closest,
                        crd::draw::Color color = crd::draw::kOrange,
                        crd::f32 endpoint_size = 0.1F, crd::f32 width_px = 1.0F,
                        crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// Per-vertex or per-face normal hairs. `points.size()` must equal
// `normals.size()`; each pair emits an arrow of length `hair_length` from
// `points[i]` along `normals[i]`. Used for visualising mesh normals (v4 +)
// or per-leaf-AABB face-normal indicators.
void draw_normals(crd::draw::RenderBuffer& buf, crd::containers::ConstSpan<crd::math::Vec3f> points,
                  crd::containers::ConstSpan<crd::math::Vec3f> normals, crd::f32 hair_length = 0.25F,
                  crd::draw::Color color = crd::draw::kCyan, crd::f32 width_px = 1.0F,
                  crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F);

} // namespace crd::geometry::viz
