#pragma once

// crd-draw -- shape generators (Phase 3.1 v1a-draw, ADR-0066 sec 7).
//
// Pure CPU helpers that decompose high-level shapes into lower-level
// `DebugLine` / `DebugTriangle` records and append them to a `RenderBuffer`.
//
// d0 ships line + box wireframe. d1 adds solid box / sphere / capsule
// (UV sphere wireframe + icosphere solid per ADR-0066 sec 7). d2 adds the
// rest of the immediate-mode API surface.

#include <crd/draw/render_buffer.hpp>
#include <crd/draw/types.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>

namespace crd::draw
{
// ---------------------------------------------------------------------------
// Primitive constructors -- lowest level. The public immediate-mode API
// (line / box_wire / etc.) is just sugar over these.
// ---------------------------------------------------------------------------

inline void add_line_to(RenderBuffer& buf, crd::math::Vec3f a, crd::math::Vec3f b,
                        Color color = kWhite, crd::f32 width_px = 1.0F,
                        PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    buf.add_line(DebugLine{a, b, color.packed_rgba(), flags, width_px, lifetime_s});
}

inline void add_point_to(RenderBuffer& buf, crd::math::Vec3f p,
                         Color color = kWhite, crd::f32 size_px = 4.0F,
                         PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    buf.add_point(DebugPoint{p, color.packed_rgba(), flags, size_px, lifetime_s});
}

// ---------------------------------------------------------------------------
// Box wireframe -- 12 edges of a unit cube, transformed by `world` and
// scaled by `half_extents`. The cube is [-1, +1]^3 in local space; with
// half_extents = (hx, hy, hz) the resulting box covers [-hx, +hx] etc.
//
// `world` is a column-major Mat4 (per crd::math convention). For a body's
// world transform, pass the model matrix directly. For a body with a
// non-identity collider local pose, pre-multiply: `world * local`.
// ---------------------------------------------------------------------------

void box_wire_to(RenderBuffer& buf, const crd::math::Mat4f& world,
                 crd::math::Vec3f half_extents, Color color = kWhite,
                 crd::f32 width_px = 1.0F, PrimFlags flags = kDefaultFlags,
                 crd::f32 lifetime_s = 0.0F);

// AABB convenience wrapper -- axis-aligned box from min/max corner pair.
// Equivalent to `box_wire_to` with an identity transform centered at the
// midpoint and half_extents = (max - min) / 2.
void aabb_wire_to(RenderBuffer& buf, crd::math::Vec3f min_corner,
                  crd::math::Vec3f max_corner, Color color = kAabb,
                  crd::f32 width_px = 1.0F, PrimFlags flags = kDefaultFlags,
                  crd::f32 lifetime_s = 0.0F);

// Add a single triangle (3 vertices in any winding; back-face culling is OFF
// in the overlay pipeline). Color uses the triangle's alpha for translucent
// fills (typical: 0.3 - 0.5 for non-occluding shape highlights).
void add_triangle_to(RenderBuffer& buf, crd::math::Vec3f a, crd::math::Vec3f b,
                     crd::math::Vec3f c, Color color = kWhite,
                     PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// ---------------------------------------------------------------------------
// Solid box (12 triangles forming the closed hull) -- transformed by `world`
// and scaled by `half_extents`. Default alpha is 0.3 so the wireframe outline
// stays legible when overlaid (typical "wire over translucent fill" look).
// ---------------------------------------------------------------------------

void box_solid_to(RenderBuffer& buf, const crd::math::Mat4f& world,
                  crd::math::Vec3f half_extents, Color color = kWhite,
                  PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// ---------------------------------------------------------------------------
// Sphere wireframe -- UV tessellation (recognisable equator + axis lines per
// ADR-0066 sec 7). Default 16 longitude meridians at 8-segment resolution.
// ---------------------------------------------------------------------------

void sphere_wire_to(RenderBuffer& buf, crd::math::Vec3f center, crd::f32 radius,
                    Color color = kWhite, crd::u32 segments_long = 16,
                    crd::u32 segments_lat = 8, crd::f32 width_px = 1.0F,
                    PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// ---------------------------------------------------------------------------
// Sphere solid -- icosphere subdivision 1 (80 triangles). Gives uniform
// triangle areas, better than UV for fills (per ADR-0066 sec 7).
// ---------------------------------------------------------------------------

void sphere_solid_to(RenderBuffer& buf, crd::math::Vec3f center, crd::f32 radius,
                     Color color = kWhite, PrimFlags flags = kDefaultFlags,
                     crd::f32 lifetime_s = 0.0F);

// ---------------------------------------------------------------------------
// Capsule wireframe -- two hemispheres + connecting line segments + an
// equatorial ring per hemisphere. `a` and `b` are the centers of the two
// hemispherical caps; total length = distance(a,b) + 2*radius.
// ---------------------------------------------------------------------------

void capsule_wire_to(RenderBuffer& buf, crd::math::Vec3f a, crd::math::Vec3f b,
                     crd::f32 radius, Color color = kWhite, crd::u32 segments = 16,
                     crd::f32 width_px = 1.0F, PrimFlags flags = kDefaultFlags,
                     crd::f32 lifetime_s = 0.0F);

// ---------------------------------------------------------------------------
// Capsule solid -- cylinder body + 2 hemispheres, all triangulated.
// Default segments=16 around the axis (~600 triangles total).
// ---------------------------------------------------------------------------

void capsule_solid_to(RenderBuffer& buf, crd::math::Vec3f a, crd::math::Vec3f b,
                      crd::f32 radius, Color color = kWhite, crd::u32 segments = 16,
                      PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// =========================================================================
// Phase 3.1 v1a-draw d2 -- gizmo-ready immediate-mode API.
//
// These primitives are designed for both debug visualization AND Phase 7
// editor manipulator gizmos / brush previews. The same shape that shows
// "joint limit arc" for physics debug also shows "rotation handle arc" for
// editor manipulators. Per ADR-0066 sec 8.
// =========================================================================

// Arrow -- line stem + 4-triangle solid cone head pointing along `dir`.
// `length` is the total arrow length from `origin` to tip; `head_size_ratio`
// is the head length as a fraction of total length (default 0.2 = 20%).
// `head_radius_ratio` is head base radius / head length (default 0.4).
//
// Common use: velocity arrows, force vectors, normal vectors, manipulator
// translation handles.
void arrow_to(RenderBuffer& buf, crd::math::Vec3f origin, crd::math::Vec3f dir,
              crd::f32 length, Color color = kWhite,
              crd::f32 head_size_ratio = 0.2F, crd::f32 head_radius_ratio = 0.4F,
              crd::f32 width_px = 2.0F, PrimFlags flags = kDefaultFlags,
              crd::f32 lifetime_s = 0.0F);

// Axis triad -- 3 arrows from `transform`'s origin along its local X / Y / Z
// axes. Colors are kAxisX / kAxisY / kAxisZ (RViz convention per ADR-0066).
// Pass `Mat4f::identity()` for world-axis triad at origin.
//
// Common use: world frame, body frame, camera frame, articulation joints.
void axis_triad_to(RenderBuffer& buf, const crd::math::Mat4f& transform,
                   crd::f32 length = 1.0F, crd::f32 width_px = 2.0F,
                   PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// Arc -- circular arc in the plane perpendicular to `axis`, traced from
// `angle_min` to `angle_max` radians, with `zero_dir` as the 0-angle
// reference. `segments` = subdivision count across the full sweep.
//
// Common use: joint limit visualization (revolute angular range), rotation
// gizmo handles, dial controls, FOV cones.
void arc_to(RenderBuffer& buf, crd::math::Vec3f center, crd::math::Vec3f axis,
            crd::math::Vec3f zero_dir, crd::f32 radius,
            crd::f32 angle_min, crd::f32 angle_max,
            Color color = kWhite, crd::u32 segments = 24,
            crd::f32 width_px = 1.0F, PrimFlags flags = kDefaultFlags,
            crd::f32 lifetime_s = 0.0F);

// 3D cross -- three perpendicular line segments centered at `center`,
// each of total length `size` (so each leg extends size/2 from center).
//
// Common use: contact point markers, IK target pin markers, picking-debug
// click-position indicators.
void cross_3d_to(RenderBuffer& buf, crd::math::Vec3f center, crd::f32 size,
                 Color color = kWhite, crd::f32 width_px = 1.0F,
                 PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// Grid -- regular grid in the plane spanned by `right` x `forward` axes,
// centered at `origin`. (cells_x+1) lines parallel to `forward` and
// (cells_z+1) lines parallel to `right`. `cell_size` is the spacing.
//
// Common use: floor reference, level editor grid, voxel-space alignment.
void grid_to(RenderBuffer& buf, crd::math::Vec3f origin, crd::math::Vec3f right,
             crd::math::Vec3f forward, crd::u32 cells_x, crd::u32 cells_z,
             crd::f32 cell_size, Color color = kGrey, crd::f32 width_px = 1.0F,
             PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// Frustum -- 8 corner unprojection of a view-projection matrix, rendered as
// 12 edges (4 near + 4 far + 4 connectors). Right-handed clip space:
// near plane at z = -1 in OpenGL convention OR z = 0 in Vulkan/D3D
// reverse-Z. The `clip_z_min` argument selects the near-plane clip-space z;
// pass 0.0F for reverse-Z (Cerid default), -1.0F for OpenGL-style.
//
// Common use: light frustum visualization, shadow cascade splits, camera
// preview, culling debug.
void frustum_to(RenderBuffer& buf, const crd::math::Mat4f& view_proj,
                Color color = kYellow, crd::f32 clip_z_min = 0.0F,
                crd::f32 width_px = 1.0F, PrimFlags flags = kDefaultFlags,
                crd::f32 lifetime_s = 0.0F);

} // namespace crd::draw
