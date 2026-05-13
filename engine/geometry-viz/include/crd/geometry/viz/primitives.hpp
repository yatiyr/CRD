#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-viz — primitive adapters (Phase 3.1.7 v1j-a).
//
// Overloaded `draw(RenderBuffer&, const Shape&, ...)` for every concrete
// primitive in `crd::geometry::primitives`. Each overload forwards to the
// matching `crd::draw::*_to` helper that already lives in crd-draw; the
// value-add here is the type adaptation (so a caller can write
// `crd::geometry::viz::draw(buf, my_aabb)` instead of unpacking corners
// before each call).
// ---------------------------------------------------------------------------

#include <crd/draw/render_buffer.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/draw/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::viz
{
// ---- AABB3 -----------------------------------------------------------------

inline void draw(crd::draw::RenderBuffer& buf, const primitives::AABB3<crd::f32>& box,
                 crd::draw::Color color = crd::draw::kAabb, crd::f32 width_px = 1.0F,
                 crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    crd::draw::aabb_wire_to(buf, box.min, box.max, color, width_px, flags, lifetime_s);
}

// ---- OBB3 ------------------------------------------------------------------
//
// Compose a world transform from the OBB's center + orientation columns
// (Mat3 -> Mat4 with translation), then forward to `box_wire_to(world,
// half_extents)`. `box_wire_to` treats the box as the unit cube `[-1, +1]^3`
// scaled by half_extents — which matches OBB3's semantics.

inline void draw(crd::draw::RenderBuffer& buf, const primitives::OBB3<crd::f32>& obb,
                 crd::draw::Color color = crd::draw::kAabb, crd::f32 width_px = 1.0F,
                 crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    using crd::math::Vec3f;
    using crd::math::Vec4f;
    crd::math::Mat4f world{};
    world.c0 = Vec4f(obb.orientation.c0.x, obb.orientation.c0.y, obb.orientation.c0.z, 0.0F);
    world.c1 = Vec4f(obb.orientation.c1.x, obb.orientation.c1.y, obb.orientation.c1.z, 0.0F);
    world.c2 = Vec4f(obb.orientation.c2.x, obb.orientation.c2.y, obb.orientation.c2.z, 0.0F);
    world.c3 = Vec4f(obb.center.x, obb.center.y, obb.center.z, 1.0F);
    crd::draw::box_wire_to(buf, world, obb.half_extents, color, width_px, flags, lifetime_s);
}

// ---- Sphere ----------------------------------------------------------------

inline void draw(crd::draw::RenderBuffer& buf, const primitives::Sphere<crd::f32>& sphere,
                 crd::draw::Color color = crd::draw::kWhite, crd::u32 lat = 8U, crd::u32 lon = 12U,
                 crd::f32 width_px = 1.0F, crd::draw::PrimFlags flags = crd::draw::kDefaultFlags,
                 crd::f32 lifetime_s = 0.0F)
{
    crd::draw::sphere_wire_to(buf, sphere.center, sphere.radius, color, lat, lon, width_px, flags, lifetime_s);
}

// ---- Capsule3 / Cylinder3 -------------------------------------------------

inline void draw(crd::draw::RenderBuffer& buf, const primitives::Capsule3<crd::f32>& cap,
                 crd::draw::Color color = crd::draw::kWhite, crd::u32 segments = 16U,
                 crd::f32 width_px = 1.0F, crd::draw::PrimFlags flags = crd::draw::kDefaultFlags,
                 crd::f32 lifetime_s = 0.0F)
{
    crd::draw::capsule_wire_to(buf, cap.a, cap.b, cap.radius, color, segments, width_px, flags, lifetime_s);
}

// A Cylinder3 (flat-cap) drawn as a capsule-like wireframe, omitting the end
// hemispheres — i.e. just the side rings + axial spokes. `capsule_wire_to`
// emits the hemispheres alongside the body; we use it here as a near-enough
// approximation and pin the cylinder-distinct caps as a v9 viz polish item
// (debt: a dedicated `cylinder_wire_to` in crd-draw would be more correct).
inline void draw(crd::draw::RenderBuffer& buf, const primitives::Cylinder3<crd::f32>& cyl,
                 crd::draw::Color color = crd::draw::kWhite, crd::u32 segments = 16U,
                 crd::f32 width_px = 1.0F, crd::draw::PrimFlags flags = crd::draw::kDefaultFlags,
                 crd::f32 lifetime_s = 0.0F)
{
    // Body rings + axial spokes by reusing the capsule helper. The
    // hemispheres at the ends are visually wrong for a flat-cap cylinder,
    // but the body + spokes shape carries.
    crd::draw::capsule_wire_to(buf, cyl.a, cyl.b, cyl.radius, color, segments, width_px, flags, lifetime_s);
}

// ---- Plane (finite patch) --------------------------------------------------
//
// Emits a `size_world`-sized square patch centered at the plane's
// closest-to-origin point and oriented in the plane's tangent frame.
// A plane is infinite by construction; for visualization we sample a finite
// neighbourhood. Caller can pass a custom anchor to centre the patch
// elsewhere.

void draw(crd::draw::RenderBuffer& buf, const primitives::Plane<crd::f32>& plane,
          crd::math::Vec3f anchor = crd::math::Vec3f(0.0F, 0.0F, 0.0F), crd::f32 size_world = 4.0F,
          crd::u32 grid_divisions = 4U, crd::draw::Color color = crd::draw::kGrey,
          crd::f32 width_px = 1.0F, crd::draw::PrimFlags flags = crd::draw::kDefaultFlags,
          crd::f32 lifetime_s = 0.0F);

// ---- Triangle3 / Tetrahedron ----------------------------------------------

inline void draw(crd::draw::RenderBuffer& buf, const primitives::Triangle3<crd::f32>& tri,
                 crd::draw::Color color = crd::draw::kWhite, crd::f32 width_px = 1.0F,
                 crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    crd::draw::add_line_to(buf, tri.a, tri.b, color, width_px, flags, lifetime_s);
    crd::draw::add_line_to(buf, tri.b, tri.c, color, width_px, flags, lifetime_s);
    crd::draw::add_line_to(buf, tri.c, tri.a, color, width_px, flags, lifetime_s);
}

inline void draw(crd::draw::RenderBuffer& buf, const primitives::Tetrahedron<crd::f32>& tet,
                 crd::draw::Color color = crd::draw::kWhite, crd::f32 width_px = 1.0F,
                 crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    // 6 edges of a tetrahedron: ab / ac / ad / bc / bd / cd.
    crd::draw::add_line_to(buf, tet.a, tet.b, color, width_px, flags, lifetime_s);
    crd::draw::add_line_to(buf, tet.a, tet.c, color, width_px, flags, lifetime_s);
    crd::draw::add_line_to(buf, tet.a, tet.d, color, width_px, flags, lifetime_s);
    crd::draw::add_line_to(buf, tet.b, tet.c, color, width_px, flags, lifetime_s);
    crd::draw::add_line_to(buf, tet.b, tet.d, color, width_px, flags, lifetime_s);
    crd::draw::add_line_to(buf, tet.c, tet.d, color, width_px, flags, lifetime_s);
}

// ---- Ray3 / Segment3 / Line3 ----------------------------------------------

inline void draw(crd::draw::RenderBuffer& buf, const primitives::Ray3<crd::f32>& ray, crd::f32 length = 10.0F,
                 crd::draw::Color color = crd::draw::kYellow, crd::f32 width_px = 1.0F,
                 crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    const crd::math::Vec3f end(ray.origin.x + ray.direction.x * length, ray.origin.y + ray.direction.y * length,
                               ray.origin.z + ray.direction.z * length);
    crd::draw::add_line_to(buf, ray.origin, end, color, width_px, flags, lifetime_s);
}

inline void draw(crd::draw::RenderBuffer& buf, const primitives::Segment3<crd::f32>& seg,
                 crd::draw::Color color = crd::draw::kWhite, crd::f32 width_px = 1.0F,
                 crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    crd::draw::add_line_to(buf, seg.a, seg.b, color, width_px, flags, lifetime_s);
}

// A `Line3` is infinite; we emit a `length`-long segment centred at the
// line's anchor `point`. Same convention as the Plane patch above.
inline void draw(crd::draw::RenderBuffer& buf, const primitives::Line3<crd::f32>& line, crd::f32 length = 10.0F,
                 crd::draw::Color color = crd::draw::kGrey, crd::f32 width_px = 1.0F,
                 crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    const crd::f32 half = length * 0.5F;
    const crd::math::Vec3f a(line.point.x - line.direction.x * half, line.point.y - line.direction.y * half,
                             line.point.z - line.direction.z * half);
    const crd::math::Vec3f b(line.point.x + line.direction.x * half, line.point.y + line.direction.y * half,
                             line.point.z + line.direction.z * half);
    crd::draw::add_line_to(buf, a, b, color, width_px, flags, lifetime_s);
}

// ---- Frustum (from 6 planes) ----------------------------------------------
//
// `primitives::Frustum<T>` stores 6 outward-facing planes. The 8 corners are
// recovered by intersecting plane triples — 3-plane intersection via the
// closed form `(d1·(n2×n3) + d2·(n3×n1) + d3·(n1×n2)) / det(n1, n2, n3)`.
// `plane_indices_for_corners` pairs each corner with (near/far, top/bottom,
// left/right) via the canonical ordering.

void draw(crd::draw::RenderBuffer& buf, const primitives::Frustum<crd::f32>& frustum,
          crd::draw::Color color = crd::draw::kYellow, crd::f32 width_px = 1.0F,
          crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F);

} // namespace crd::geometry::viz
