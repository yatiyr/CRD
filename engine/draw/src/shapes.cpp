// crd-draw -- shape generators (Phase 3.1 v1a-draw, ADR-0066 sec 7).

#include <crd/draw/shapes.hpp>

#include <cmath>

namespace crd::draw
{
namespace
{
// Unit-cube vertices in local space. 8 corners; each component is +/-1.
//
//        7 +-----+ 6
//         /|    /|
//      4 +-----+ 5|
//        | 3 +-+-+ 2
//        |/    |/
//      0 +-----+ 1
//
// Layout chosen so the 12-edge index list below stays small + obvious.
inline constexpr crd::math::Vec3f kUnitBoxCorners[8] = {
    {-1.0F, -1.0F, -1.0F}, // 0
    { 1.0F, -1.0F, -1.0F}, // 1
    { 1.0F, -1.0F,  1.0F}, // 2
    {-1.0F, -1.0F,  1.0F}, // 3
    {-1.0F,  1.0F, -1.0F}, // 4
    { 1.0F,  1.0F, -1.0F}, // 5
    { 1.0F,  1.0F,  1.0F}, // 6
    {-1.0F,  1.0F,  1.0F}, // 7
};

// 12 edges as pairs of corner indices: bottom face x 4, top face x 4,
// vertical pillars x 4. Order doesn't matter functionally; this layout
// matches the "draw bottom, draw top, draw verticals" mental model that
// makes the asserts in the test easy to read.
inline constexpr crd::u8 kUnitBoxEdges[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0}, // bottom (y = -1)
    {4, 5}, {5, 6}, {6, 7}, {7, 4}, // top    (y = +1)
    {0, 4}, {1, 5}, {2, 6}, {3, 7}, // vertical pillars
};

inline crd::math::Vec3f transform_corner(const crd::math::Mat4f& world,
                                         crd::math::Vec3f          half_extents,
                                         const crd::math::Vec3f&   unit_corner) noexcept
{
    // Scale unit corner by half_extents, then transform as a point through
    // the column-major Mat4 (w=1 implies the translation column c3 is applied).
    const crd::math::Vec4f local_h{unit_corner.x * half_extents.x,
                                    unit_corner.y * half_extents.y,
                                    unit_corner.z * half_extents.z,
                                    1.0F};
    const crd::math::Vec4f world_h = world * local_h;
    return crd::math::Vec3f{world_h.x, world_h.y, world_h.z};
}
} // namespace

void box_wire_to(RenderBuffer& buf, const crd::math::Mat4f& world,
                 crd::math::Vec3f half_extents, Color color, crd::f32 width_px,
                 PrimFlags flags, crd::f32 lifetime_s)
{
    crd::math::Vec3f world_corners[8];
    for (crd::usize i = 0; i < 8; ++i)
    {
        world_corners[i] = transform_corner(world, half_extents, kUnitBoxCorners[i]);
    }

    const crd::u32 packed = color.packed_rgba();
    for (const auto& edge : kUnitBoxEdges)
    {
        buf.add_line(DebugLine{world_corners[edge[0]], world_corners[edge[1]],
                               packed, flags, width_px, lifetime_s});
    }
}

void aabb_wire_to(RenderBuffer& buf, crd::math::Vec3f min_corner,
                  crd::math::Vec3f max_corner, Color color, crd::f32 width_px,
                  PrimFlags flags, crd::f32 lifetime_s)
{
    const crd::math::Vec3f mid{(min_corner.x + max_corner.x) * 0.5F,
                                (min_corner.y + max_corner.y) * 0.5F,
                                (min_corner.z + max_corner.z) * 0.5F};
    const crd::math::Vec3f half_extents{(max_corner.x - min_corner.x) * 0.5F,
                                         (max_corner.y - min_corner.y) * 0.5F,
                                         (max_corner.z - min_corner.z) * 0.5F};

    // Translation-only column-major Mat4: identity rotation, midpoint translation.
    crd::math::Mat4f world = crd::math::Mat4f::identity();
    world.c3.x = mid.x;
    world.c3.y = mid.y;
    world.c3.z = mid.z;

    box_wire_to(buf, world, half_extents, color, width_px, flags, lifetime_s);
}

void add_triangle_to(RenderBuffer& buf, crd::math::Vec3f a, crd::math::Vec3f b,
                     crd::math::Vec3f c, Color color, PrimFlags flags, crd::f32 lifetime_s)
{
    buf.add_triangle(DebugTriangle{a, b, c, color.packed_rgba(), flags, lifetime_s});
}

// ---------------------------------------------------------------------------
// Box solid -- 12 triangles forming a closed cube. Two triangles per face,
// 6 faces. Winding chosen so all face outwards (CCW from outside); the
// pipeline disables back-face culling so even reversed winding renders fine.
// ---------------------------------------------------------------------------

namespace
{
// 12 triangles as triplets of corner indices into kUnitBoxCorners.
// Each face = 2 triangles. Faces: -Z, +Z, -Y, +Y, -X, +X.
inline constexpr crd::u8 kUnitBoxTriangles[12][3] = {
    {0, 1, 5}, {0, 5, 4}, // -Z face (back)
    {2, 3, 7}, {2, 7, 6}, // +Z face (front)
    {0, 3, 2}, {0, 2, 1}, // -Y face (bottom)
    {4, 5, 6}, {4, 6, 7}, // +Y face (top)
    {0, 4, 7}, {0, 7, 3}, // -X face (left)
    {1, 2, 6}, {1, 6, 5}, // +X face (right)
};
} // namespace

void box_solid_to(RenderBuffer& buf, const crd::math::Mat4f& world,
                  crd::math::Vec3f half_extents, Color color,
                  PrimFlags flags, crd::f32 lifetime_s)
{
    crd::math::Vec3f world_corners[8];
    for (crd::usize i = 0; i < 8; ++i)
    {
        world_corners[i] = transform_corner(world, half_extents, kUnitBoxCorners[i]);
    }

    const crd::u32 packed = color.packed_rgba();
    for (const auto& tri : kUnitBoxTriangles)
    {
        buf.add_triangle(DebugTriangle{world_corners[tri[0]], world_corners[tri[1]],
                                       world_corners[tri[2]], packed, flags, lifetime_s});
    }
}

// ---------------------------------------------------------------------------
// Sphere wireframe -- UV. Emits `segments_long` great-circle meridians
// (full vertical circles through the poles) + `segments_lat-1` horizontal
// rings. For 16x8 default = 16 meridians (each 16 segs) + 7 horizontal
// rings (each 16 segs) = 16*16 + 7*16 = 368 line segments. Acceptable
// per-instance for typical body counts.
// ---------------------------------------------------------------------------

void sphere_wire_to(RenderBuffer& buf, crd::math::Vec3f center, crd::f32 radius,
                    Color color, crd::u32 segments_long, crd::u32 segments_lat,
                    crd::f32 width_px, PrimFlags flags, crd::f32 lifetime_s)
{
    const crd::f32 two_pi = 6.28318530717958647692F;
    const crd::f32 pi     = 3.14159265358979323846F;
    const crd::u32 nlong  = segments_long;
    const crd::u32 nlat   = segments_lat;
    if (nlong < 3 || nlat < 2 || radius <= 0.0F) return;

    const crd::u32 packed = color.packed_rgba();

    // Meridians (longitude): for each meridian, a vertical great circle
    // composed of (nlat) segments. d2-fix: was nlong segments which made
    // the meridian grid spacing inconsistent with the latitude rings; now
    // uses nlat segments so the wireframe vertices land exactly on the
    // sphere_solid UV grid for perfect alignment.
    for (crd::u32 m = 0; m < nlong; ++m)
    {
        const crd::f32 phi = (static_cast<crd::f32>(m) / static_cast<crd::f32>(nlong)) * two_pi;
        const crd::f32 cosp = std::cos(phi);
        const crd::f32 sinp = std::sin(phi);
        for (crd::u32 t = 0; t < nlat; ++t)
        {
            const crd::f32 t0 = (static_cast<crd::f32>(t)        / static_cast<crd::f32>(nlat)) * pi;
            const crd::f32 t1 = (static_cast<crd::f32>(t + 1)    / static_cast<crd::f32>(nlat)) * pi;
            const crd::math::Vec3f p0{
                center.x + radius * std::sin(t0) * cosp,
                center.y + radius * std::cos(t0),
                center.z + radius * std::sin(t0) * sinp};
            const crd::math::Vec3f p1{
                center.x + radius * std::sin(t1) * cosp,
                center.y + radius * std::cos(t1),
                center.z + radius * std::sin(t1) * sinp};
            buf.add_line(DebugLine{p0, p1, packed, flags, width_px, lifetime_s});
        }
    }

    // Horizontal rings (latitude): nlat-1 rings between the poles.
    for (crd::u32 r = 1; r < nlat; ++r)
    {
        const crd::f32 theta = (static_cast<crd::f32>(r) / static_cast<crd::f32>(nlat)) * pi;
        const crd::f32 y     = center.y + radius * std::cos(theta);
        const crd::f32 ring_r = radius * std::sin(theta);
        for (crd::u32 m = 0; m < nlong; ++m)
        {
            const crd::f32 phi0 = (static_cast<crd::f32>(m)     / static_cast<crd::f32>(nlong)) * two_pi;
            const crd::f32 phi1 = (static_cast<crd::f32>(m + 1) / static_cast<crd::f32>(nlong)) * two_pi;
            const crd::math::Vec3f p0{center.x + ring_r * std::cos(phi0), y, center.z + ring_r * std::sin(phi0)};
            const crd::math::Vec3f p1{center.x + ring_r * std::cos(phi1), y, center.z + ring_r * std::sin(phi1)};
            buf.add_line(DebugLine{p0, p1, packed, flags, width_px, lifetime_s});
        }
    }
}

// ---------------------------------------------------------------------------
// Sphere solid -- UV tessellation matching sphere_wire's grid (16 long x
// 8 lat default = ~224 triangles). d2-fix decision: UV-everywhere for
// perfect alignment between wireframe outlines + translucent fill, since
// we don't do per-triangle lighting (icosphere's vertex-uniformity advantage
// was academic). Replaces the prior icosphere-subdivision-1 path.
// ---------------------------------------------------------------------------

namespace
{
inline crd::math::Vec3f sphere_uv_vertex(crd::math::Vec3f center, crd::f32 radius,
                                          crd::f32 phi, crd::f32 theta) noexcept
{
    const crd::f32 sin_t = std::sin(theta);
    return {center.x + radius * sin_t * std::cos(phi),
            center.y + radius * std::cos(theta),
            center.z + radius * sin_t * std::sin(phi)};
}
} // namespace

void sphere_solid_to(RenderBuffer& buf, crd::math::Vec3f center, crd::f32 radius,
                     Color color, PrimFlags flags, crd::f32 lifetime_s)
{
    if (radius <= 0.0F) return;
    constexpr crd::u32 kNlong = 16; // matches sphere_wire's `segments_long` default
    constexpr crd::u32 kNlat  = 8;  // matches sphere_wire's `segments_lat`  default
    const crd::f32 two_pi = 6.28318530717958647692F;
    const crd::f32 pi     = 3.14159265358979323846F;
    const crd::u32 packed = color.packed_rgba();

    const crd::math::Vec3f south_pole = sphere_uv_vertex(center, radius, 0.0F, 0.0F);
    const crd::math::Vec3f north_pole = sphere_uv_vertex(center, radius, 0.0F, pi);

    for (crd::u32 r = 0; r < kNlat; ++r)
    {
        const crd::f32 theta_lo = (static_cast<crd::f32>(r)     / static_cast<crd::f32>(kNlat)) * pi;
        const crd::f32 theta_hi = (static_cast<crd::f32>(r + 1) / static_cast<crd::f32>(kNlat)) * pi;
        for (crd::u32 m = 0; m < kNlong; ++m)
        {
            const crd::f32 phi_lo = (static_cast<crd::f32>(m)     / static_cast<crd::f32>(kNlong)) * two_pi;
            const crd::f32 phi_hi = (static_cast<crd::f32>(m + 1) / static_cast<crd::f32>(kNlong)) * two_pi;
            const crd::math::Vec3f p_ll = sphere_uv_vertex(center, radius, phi_lo, theta_lo);
            const crd::math::Vec3f p_rl = sphere_uv_vertex(center, radius, phi_hi, theta_lo);
            const crd::math::Vec3f p_lh = sphere_uv_vertex(center, radius, phi_lo, theta_hi);
            const crd::math::Vec3f p_rh = sphere_uv_vertex(center, radius, phi_hi, theta_hi);

            if (r == 0)
            {
                // South-cap fan (theta=0 collapses to south pole).
                buf.add_triangle(DebugTriangle{south_pole, p_lh, p_rh, packed, flags, lifetime_s});
            }
            else if (r == kNlat - 1)
            {
                // North-cap fan (theta=pi collapses to north pole).
                buf.add_triangle(DebugTriangle{p_ll, north_pole, p_rl, packed, flags, lifetime_s});
            }
            else
            {
                // Middle band: 2 triangles per cell, CCW from outside.
                buf.add_triangle(DebugTriangle{p_ll, p_lh, p_rh, packed, flags, lifetime_s});
                buf.add_triangle(DebugTriangle{p_ll, p_rh, p_rl, packed, flags, lifetime_s});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Capsule helpers -- shared axis math.
// ---------------------------------------------------------------------------

namespace
{
struct CapsuleFrame
{
    crd::math::Vec3f axis;     // unit vector from a to b
    crd::math::Vec3f right;    // unit vector perpendicular to axis
    crd::math::Vec3f forward;  // unit vector perpendicular to axis + right
    crd::f32         length;   // distance(a, b)
    bool             valid;    // false if a == b (degenerate)
};

CapsuleFrame compute_capsule_frame(crd::math::Vec3f a, crd::math::Vec3f b) noexcept
{
    CapsuleFrame f{};
    const crd::math::Vec3f d{b.x - a.x, b.y - a.y, b.z - a.z};
    const crd::f32 len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    if (len2 <= 0.0F)
    {
        f.axis = {0.0F, 1.0F, 0.0F};
        f.length = 0.0F;
        f.valid  = false;
    }
    else
    {
        f.length = std::sqrt(len2);
        f.axis   = {d.x / f.length, d.y / f.length, d.z / f.length};
        f.valid  = true;
    }
    // Pick a stable perpendicular: cross with whichever world axis is
    // most non-parallel to f.axis.
    const crd::math::Vec3f world_up{0.0F, 1.0F, 0.0F};
    const crd::math::Vec3f world_x {1.0F, 0.0F, 0.0F};
    const crd::math::Vec3f& seed = (std::abs(f.axis.y) > 0.9F) ? world_x : world_up;
    crd::math::Vec3f r{
        seed.y * f.axis.z - seed.z * f.axis.y,
        seed.z * f.axis.x - seed.x * f.axis.z,
        seed.x * f.axis.y - seed.y * f.axis.x};
    const crd::f32 r_len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    if (r_len > 0.0F) { r = {r.x / r_len, r.y / r_len, r.z / r_len}; }
    else              { r = {1.0F, 0.0F, 0.0F}; }
    f.right = r;
    f.forward = {
        f.axis.y * r.z - f.axis.z * r.y,
        f.axis.z * r.x - f.axis.x * r.z,
        f.axis.x * r.y - f.axis.y * r.x};
    return f;
}
} // namespace

void capsule_wire_to(RenderBuffer& buf, crd::math::Vec3f a, crd::math::Vec3f b,
                     crd::f32 radius, Color color, crd::u32 segments,
                     crd::f32 width_px, PrimFlags flags, crd::f32 lifetime_s)
{
    if (radius <= 0.0F || segments < 6) return;
    const auto f = compute_capsule_frame(a, b);
    const crd::u32 packed = color.packed_rgba();
    const crd::f32 two_pi = 6.28318530717958647692F;

    auto offset_along = [&](const crd::math::Vec3f& base, crd::f32 r_scale,
                            crd::f32 cos_a, crd::f32 sin_a, crd::f32 along) {
        return crd::math::Vec3f{
            base.x + (f.right.x * cos_a + f.forward.x * sin_a) * r_scale + f.axis.x * along,
            base.y + (f.right.y * cos_a + f.forward.y * sin_a) * r_scale + f.axis.y * along,
            base.z + (f.right.z * cos_a + f.forward.z * sin_a) * r_scale + f.axis.z * along};
    };

    // Equatorial rings at both caps (a + b).
    for (crd::u32 i = 0; i < segments; ++i)
    {
        const crd::f32 t0 = (static_cast<crd::f32>(i)     / static_cast<crd::f32>(segments)) * two_pi;
        const crd::f32 t1 = (static_cast<crd::f32>(i + 1) / static_cast<crd::f32>(segments)) * two_pi;
        const crd::math::Vec3f a0 = offset_along(a, radius, std::cos(t0), std::sin(t0), 0.0F);
        const crd::math::Vec3f a1 = offset_along(a, radius, std::cos(t1), std::sin(t1), 0.0F);
        const crd::math::Vec3f b0 = offset_along(b, radius, std::cos(t0), std::sin(t0), 0.0F);
        const crd::math::Vec3f b1 = offset_along(b, radius, std::cos(t1), std::sin(t1), 0.0F);
        buf.add_line(DebugLine{a0, a1, packed, flags, width_px, lifetime_s});
        buf.add_line(DebugLine{b0, b1, packed, flags, width_px, lifetime_s});
    }

    // Connecting "side" lines from a-equator to b-equator at 4 cardinal angles.
    for (crd::u32 i = 0; i < 4; ++i)
    {
        const crd::f32 t = (static_cast<crd::f32>(i) / 4.0F) * two_pi;
        const crd::math::Vec3f a_pt = offset_along(a, radius, std::cos(t), std::sin(t), 0.0F);
        const crd::math::Vec3f b_pt = offset_along(b, radius, std::cos(t), std::sin(t), 0.0F);
        buf.add_line(DebugLine{a_pt, b_pt, packed, flags, width_px, lifetime_s});
    }

    // Hemispherical caps -- two perpendicular semicircles at each end,
    // each spanning angle alpha in [0, pi]:
    //   alpha=0   : equator at +tangent (in-plane = +r,  axial = 0)
    //   alpha=pi/2: pole       (in-plane = 0,  axial = +-r away from body)
    //   alpha=pi  : equator at -tangent (in-plane = -r,  axial = 0)
    // (The earlier [-pi/2, +pi/2] parameterization put the endpoints inside
    // the cylinder body instead of on the equator -- d2-fix.)
    const crd::f32 pi    = 3.14159265358979323846F;
    const crd::u32 hemi_segs = (segments < 4) ? 4 : segments;
    for (crd::u32 plane = 0; plane < 2; ++plane)
    {
        const crd::math::Vec3f& tangent = (plane == 0) ? f.right : f.forward;
        for (crd::u32 i = 0; i < hemi_segs; ++i)
        {
            const crd::f32 t0 = (static_cast<crd::f32>(i)     / static_cast<crd::f32>(hemi_segs)) * pi;
            const crd::f32 t1 = (static_cast<crd::f32>(i + 1) / static_cast<crd::f32>(hemi_segs)) * pi;
            const crd::f32 c0 = std::cos(t0); const crd::f32 s0 = std::sin(t0);
            const crd::f32 c1 = std::cos(t1); const crd::f32 s1 = std::sin(t1);
            // a-cap extends in -axis direction (away from b).
            const crd::math::Vec3f ap0{a.x + tangent.x * c0 * radius - f.axis.x * s0 * radius,
                                       a.y + tangent.y * c0 * radius - f.axis.y * s0 * radius,
                                       a.z + tangent.z * c0 * radius - f.axis.z * s0 * radius};
            const crd::math::Vec3f ap1{a.x + tangent.x * c1 * radius - f.axis.x * s1 * radius,
                                       a.y + tangent.y * c1 * radius - f.axis.y * s1 * radius,
                                       a.z + tangent.z * c1 * radius - f.axis.z * s1 * radius};
            buf.add_line(DebugLine{ap0, ap1, packed, flags, width_px, lifetime_s});
            // b-cap extends in +axis direction (away from a).
            const crd::math::Vec3f bp0{b.x + tangent.x * c0 * radius + f.axis.x * s0 * radius,
                                       b.y + tangent.y * c0 * radius + f.axis.y * s0 * radius,
                                       b.z + tangent.z * c0 * radius + f.axis.z * s0 * radius};
            const crd::math::Vec3f bp1{b.x + tangent.x * c1 * radius + f.axis.x * s1 * radius,
                                       b.y + tangent.y * c1 * radius + f.axis.y * s1 * radius,
                                       b.z + tangent.z * c1 * radius + f.axis.z * s1 * radius};
            buf.add_line(DebugLine{bp0, bp1, packed, flags, width_px, lifetime_s});
        }
    }
}

void capsule_solid_to(RenderBuffer& buf, crd::math::Vec3f a, crd::math::Vec3f b,
                      crd::f32 radius, Color color, crd::u32 segments,
                      PrimFlags flags, crd::f32 lifetime_s)
{
    if (radius <= 0.0F || segments < 6) return;
    const auto f = compute_capsule_frame(a, b);
    const crd::u32 packed = color.packed_rgba();
    const crd::f32 two_pi = 6.28318530717958647692F;
    const crd::f32 pi_2   = 1.57079632679489661923F;
    const crd::u32 stacks = 4; // hemisphere stacks (pole to equator)

    auto cap_vert = [&](crd::math::Vec3f base, crd::f32 phi, crd::f32 theta,
                        crd::f32 axial_sign) {
        // theta in [0, pi/2]: 0 = pole on axis_sign side, pi/2 = equator.
        const crd::f32 s = std::sin(theta);
        const crd::f32 c = std::cos(theta);
        const crd::math::Vec3f tangential{
            (f.right.x * std::cos(phi) + f.forward.x * std::sin(phi)) * radius * s,
            (f.right.y * std::cos(phi) + f.forward.y * std::sin(phi)) * radius * s,
            (f.right.z * std::cos(phi) + f.forward.z * std::sin(phi)) * radius * s};
        const crd::math::Vec3f axial{f.axis.x * radius * c * axial_sign,
                                     f.axis.y * radius * c * axial_sign,
                                     f.axis.z * radius * c * axial_sign};
        return crd::math::Vec3f{base.x + tangential.x + axial.x,
                                base.y + tangential.y + axial.y,
                                base.z + tangential.z + axial.z};
    };

    // -- Cylinder body: `segments` quads between a-equator and b-equator. --
    for (crd::u32 i = 0; i < segments; ++i)
    {
        const crd::f32 phi0 = (static_cast<crd::f32>(i)     / static_cast<crd::f32>(segments)) * two_pi;
        const crd::f32 phi1 = (static_cast<crd::f32>(i + 1) / static_cast<crd::f32>(segments)) * two_pi;
        const crd::math::Vec3f a0 = cap_vert(a, phi0, pi_2, -1.0F);
        const crd::math::Vec3f a1 = cap_vert(a, phi1, pi_2, -1.0F);
        const crd::math::Vec3f b0 = cap_vert(b, phi0, pi_2, +1.0F);
        const crd::math::Vec3f b1 = cap_vert(b, phi1, pi_2, +1.0F);
        buf.add_triangle(DebugTriangle{a0, b0, b1, packed, flags, lifetime_s});
        buf.add_triangle(DebugTriangle{a0, b1, a1, packed, flags, lifetime_s});
    }

    // -- Hemispherical caps: triangulate stack-by-stack. --
    for (crd::u32 cap = 0; cap < 2; ++cap)
    {
        const crd::math::Vec3f base       = (cap == 0) ? a : b;
        const crd::f32         axial_sign = (cap == 0) ? -1.0F : +1.0F;
        for (crd::u32 s = 0; s < stacks; ++s)
        {
            const crd::f32 t0 = (static_cast<crd::f32>(s)     / static_cast<crd::f32>(stacks)) * pi_2;
            const crd::f32 t1 = (static_cast<crd::f32>(s + 1) / static_cast<crd::f32>(stacks)) * pi_2;
            for (crd::u32 i = 0; i < segments; ++i)
            {
                const crd::f32 phi0 = (static_cast<crd::f32>(i)     / static_cast<crd::f32>(segments)) * two_pi;
                const crd::f32 phi1 = (static_cast<crd::f32>(i + 1) / static_cast<crd::f32>(segments)) * two_pi;
                const crd::math::Vec3f p00 = cap_vert(base, phi0, t0, axial_sign);
                const crd::math::Vec3f p10 = cap_vert(base, phi1, t0, axial_sign);
                const crd::math::Vec3f p01 = cap_vert(base, phi0, t1, axial_sign);
                const crd::math::Vec3f p11 = cap_vert(base, phi1, t1, axial_sign);
                buf.add_triangle(DebugTriangle{p00, p10, p11, packed, flags, lifetime_s});
                buf.add_triangle(DebugTriangle{p00, p11, p01, packed, flags, lifetime_s});
            }
        }
    }
}

// =========================================================================
// d2 -- gizmo-ready immediate-mode primitives.
// =========================================================================

namespace
{
// Compute an orthonormal frame around an arbitrary direction. `dir` is
// assumed already normalized (callers normalize at entry). Returns
// (right, up) such that {dir, right, up} is a right-handed orthonormal basis.
struct DirFrame { crd::math::Vec3f right; crd::math::Vec3f up; };

DirFrame frame_from_direction(crd::math::Vec3f dir) noexcept
{
    const crd::math::Vec3f world_up{0.0F, 1.0F, 0.0F};
    const crd::math::Vec3f world_x {1.0F, 0.0F, 0.0F};
    const crd::math::Vec3f& seed = (std::abs(dir.y) > 0.9F) ? world_x : world_up;
    crd::math::Vec3f right{
        seed.y * dir.z - seed.z * dir.y,
        seed.z * dir.x - seed.x * dir.z,
        seed.x * dir.y - seed.y * dir.x};
    const crd::f32 r_len = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
    if (r_len > 0.0F) { right = {right.x / r_len, right.y / r_len, right.z / r_len}; }
    else              { right = {1.0F, 0.0F, 0.0F}; }
    const crd::math::Vec3f up{
        dir.y * right.z - dir.z * right.y,
        dir.z * right.x - dir.x * right.z,
        dir.x * right.y - dir.y * right.x};
    return {right, up};
}
} // namespace

void arrow_to(RenderBuffer& buf, crd::math::Vec3f origin, crd::math::Vec3f dir,
              crd::f32 length, Color color, crd::f32 head_size_ratio,
              crd::f32 head_radius_ratio, crd::f32 width_px,
              PrimFlags flags, crd::f32 lifetime_s)
{
    if (length <= 0.0F) return;
    const crd::f32 dlen = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dlen <= 0.0F) return;
    const crd::math::Vec3f udir{dir.x / dlen, dir.y / dlen, dir.z / dlen};

    const crd::f32 head_len    = length * head_size_ratio;
    const crd::f32 head_radius = head_len * head_radius_ratio;
    const crd::f32 stem_len    = length - head_len;

    const crd::math::Vec3f tip {origin.x + udir.x * length,
                                 origin.y + udir.y * length,
                                 origin.z + udir.z * length};
    const crd::math::Vec3f base{origin.x + udir.x * stem_len,
                                 origin.y + udir.y * stem_len,
                                 origin.z + udir.z * stem_len};

    // Stem -- single line from origin to head base.
    add_line_to(buf, origin, base, color, width_px, flags, lifetime_s);

    // Head -- 4-triangle cone. Base ring of 4 corners, all triangles meet at
    // the tip. 4 segments is the gizmo convention; rounder cones at higher
    // segment counts can land later if needed.
    const auto frame = frame_from_direction(udir);
    crd::math::Vec3f corners[4];
    for (int i = 0; i < 4; ++i)
    {
        const crd::f32 angle = static_cast<crd::f32>(i) * 1.57079632679489661923F; // pi/2
        const crd::f32 c = std::cos(angle);
        const crd::f32 s = std::sin(angle);
        corners[i] = {base.x + (frame.right.x * c + frame.up.x * s) * head_radius,
                      base.y + (frame.right.y * c + frame.up.y * s) * head_radius,
                      base.z + (frame.right.z * c + frame.up.z * s) * head_radius};
    }
    const crd::u32 packed = color.packed_rgba();
    for (int i = 0; i < 4; ++i)
    {
        const crd::math::Vec3f& a = corners[i];
        const crd::math::Vec3f& b = corners[(i + 1) % 4];
        // Side triangle -- corner i -> corner i+1 -> tip.
        buf.add_triangle(DebugTriangle{a, b, tip, packed, flags, lifetime_s});
    }
    // Base cap -- 2 triangles closing the cone bottom (so it doesn't look
    // hollow when rendered with translucent color from underneath).
    buf.add_triangle(DebugTriangle{corners[0], corners[2], corners[1], packed, flags, lifetime_s});
    buf.add_triangle(DebugTriangle{corners[0], corners[3], corners[2], packed, flags, lifetime_s});
}

void axis_triad_to(RenderBuffer& buf, const crd::math::Mat4f& transform,
                   crd::f32 length, crd::f32 width_px, PrimFlags flags,
                   crd::f32 lifetime_s)
{
    // Origin = transform * (0, 0, 0, 1) = transform.c3 column.
    const crd::math::Vec3f origin{transform.c3.x, transform.c3.y, transform.c3.z};
    // Local-axis directions = first 3 columns (rotation+scale applied).
    const crd::math::Vec3f x_dir{transform.c0.x, transform.c0.y, transform.c0.z};
    const crd::math::Vec3f y_dir{transform.c1.x, transform.c1.y, transform.c1.z};
    const crd::math::Vec3f z_dir{transform.c2.x, transform.c2.y, transform.c2.z};
    arrow_to(buf, origin, x_dir, length, kAxisX, 0.2F, 0.4F, width_px, flags, lifetime_s);
    arrow_to(buf, origin, y_dir, length, kAxisY, 0.2F, 0.4F, width_px, flags, lifetime_s);
    arrow_to(buf, origin, z_dir, length, kAxisZ, 0.2F, 0.4F, width_px, flags, lifetime_s);
}

void arc_to(RenderBuffer& buf, crd::math::Vec3f center, crd::math::Vec3f axis,
            crd::math::Vec3f zero_dir, crd::f32 radius, crd::f32 angle_min,
            crd::f32 angle_max, Color color, crd::u32 segments,
            crd::f32 width_px, PrimFlags flags, crd::f32 lifetime_s)
{
    if (radius <= 0.0F || segments < 1) return;
    // Normalize axis + zero_dir; compute perpendicular = axis x zero_dir.
    const crd::f32 ax_len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (ax_len <= 0.0F) return;
    const crd::math::Vec3f a{axis.x / ax_len, axis.y / ax_len, axis.z / ax_len};
    // Project zero_dir onto the plane perpendicular to a, then normalize.
    const crd::f32 zdota = zero_dir.x * a.x + zero_dir.y * a.y + zero_dir.z * a.z;
    crd::math::Vec3f r0{zero_dir.x - a.x * zdota, zero_dir.y - a.y * zdota, zero_dir.z - a.z * zdota};
    const crd::f32 r0_len = std::sqrt(r0.x * r0.x + r0.y * r0.y + r0.z * r0.z);
    if (r0_len <= 0.0F) return;
    r0 = {r0.x / r0_len, r0.y / r0_len, r0.z / r0_len};
    // perp = a x r0 (right-handed).
    const crd::math::Vec3f r1{a.y * r0.z - a.z * r0.y,
                               a.z * r0.x - a.x * r0.z,
                               a.x * r0.y - a.y * r0.x};

    const crd::f32 packed_w = width_px;
    const crd::u32 packed_c = color.packed_rgba();
    const crd::f32 sweep = angle_max - angle_min;
    crd::math::Vec3f prev{center.x + r0.x * radius * std::cos(angle_min) + r1.x * radius * std::sin(angle_min),
                          center.y + r0.y * radius * std::cos(angle_min) + r1.y * radius * std::sin(angle_min),
                          center.z + r0.z * radius * std::cos(angle_min) + r1.z * radius * std::sin(angle_min)};
    for (crd::u32 i = 1; i <= segments; ++i)
    {
        const crd::f32 t = angle_min + sweep * (static_cast<crd::f32>(i) / static_cast<crd::f32>(segments));
        const crd::f32 ct = std::cos(t);
        const crd::f32 st = std::sin(t);
        const crd::math::Vec3f next{center.x + r0.x * radius * ct + r1.x * radius * st,
                                     center.y + r0.y * radius * ct + r1.y * radius * st,
                                     center.z + r0.z * radius * ct + r1.z * radius * st};
        buf.add_line(DebugLine{prev, next, packed_c, flags, packed_w, lifetime_s});
        prev = next;
    }
}

void cross_3d_to(RenderBuffer& buf, crd::math::Vec3f center, crd::f32 size,
                 Color color, crd::f32 width_px, PrimFlags flags,
                 crd::f32 lifetime_s)
{
    const crd::f32 h = size * 0.5F;
    add_line_to(buf, {center.x - h, center.y, center.z}, {center.x + h, center.y, center.z},
                color, width_px, flags, lifetime_s);
    add_line_to(buf, {center.x, center.y - h, center.z}, {center.x, center.y + h, center.z},
                color, width_px, flags, lifetime_s);
    add_line_to(buf, {center.x, center.y, center.z - h}, {center.x, center.y, center.z + h},
                color, width_px, flags, lifetime_s);
}

void grid_to(RenderBuffer& buf, crd::math::Vec3f origin, crd::math::Vec3f right,
             crd::math::Vec3f forward, crd::u32 cells_x, crd::u32 cells_z,
             crd::f32 cell_size, Color color, crd::f32 width_px,
             PrimFlags flags, crd::f32 lifetime_s)
{
    if (cell_size <= 0.0F || cells_x == 0 || cells_z == 0) return;
    const crd::f32 half_x = static_cast<crd::f32>(cells_x) * 0.5F * cell_size;
    const crd::f32 half_z = static_cast<crd::f32>(cells_z) * 0.5F * cell_size;
    const crd::math::Vec3f r_step{right.x * cell_size, right.y * cell_size, right.z * cell_size};
    const crd::math::Vec3f f_step{forward.x * cell_size, forward.y * cell_size, forward.z * cell_size};
    const crd::math::Vec3f base{origin.x - right.x * half_x - forward.x * half_z,
                                 origin.y - right.y * half_x - forward.y * half_z,
                                 origin.z - right.z * half_x - forward.z * half_z};
    const crd::math::Vec3f r_full{r_step.x * static_cast<crd::f32>(cells_x),
                                   r_step.y * static_cast<crd::f32>(cells_x),
                                   r_step.z * static_cast<crd::f32>(cells_x)};
    const crd::math::Vec3f f_full{f_step.x * static_cast<crd::f32>(cells_z),
                                   f_step.y * static_cast<crd::f32>(cells_z),
                                   f_step.z * static_cast<crd::f32>(cells_z)};

    // Lines parallel to `forward` (one per x division).
    for (crd::u32 i = 0; i <= cells_x; ++i)
    {
        const crd::math::Vec3f a{base.x + r_step.x * static_cast<crd::f32>(i),
                                 base.y + r_step.y * static_cast<crd::f32>(i),
                                 base.z + r_step.z * static_cast<crd::f32>(i)};
        const crd::math::Vec3f b{a.x + f_full.x, a.y + f_full.y, a.z + f_full.z};
        add_line_to(buf, a, b, color, width_px, flags, lifetime_s);
    }
    // Lines parallel to `right` (one per z division).
    for (crd::u32 i = 0; i <= cells_z; ++i)
    {
        const crd::math::Vec3f a{base.x + f_step.x * static_cast<crd::f32>(i),
                                 base.y + f_step.y * static_cast<crd::f32>(i),
                                 base.z + f_step.z * static_cast<crd::f32>(i)};
        const crd::math::Vec3f b{a.x + r_full.x, a.y + r_full.y, a.z + r_full.z};
        add_line_to(buf, a, b, color, width_px, flags, lifetime_s);
    }
}

void frustum_to(RenderBuffer& buf, const crd::math::Mat4f& view_proj,
                Color color, crd::f32 clip_z_min, crd::f32 width_px,
                PrimFlags flags, crd::f32 lifetime_s)
{
    // Unproject the 8 NDC corners through inverse(view_proj). Caller passes
    // clip_z_min = 0.0 for reverse-Z (Cerid default) or -1.0 for OpenGL-style.
    const crd::math::Mat4f inv = crd::math::inverse(view_proj);
    const crd::f32 z0 = clip_z_min;
    const crd::f32 z1 = 1.0F;
    const crd::math::Vec4f ndc[8] = {
        {-1.0F, -1.0F, z0, 1.0F}, // 0: near bottom-left
        { 1.0F, -1.0F, z0, 1.0F}, // 1: near bottom-right
        { 1.0F,  1.0F, z0, 1.0F}, // 2: near top-right
        {-1.0F,  1.0F, z0, 1.0F}, // 3: near top-left
        {-1.0F, -1.0F, z1, 1.0F}, // 4: far  bottom-left
        { 1.0F, -1.0F, z1, 1.0F}, // 5: far  bottom-right
        { 1.0F,  1.0F, z1, 1.0F}, // 6: far  top-right
        {-1.0F,  1.0F, z1, 1.0F}, // 7: far  top-left
    };
    crd::math::Vec3f world[8];
    for (int i = 0; i < 8; ++i)
    {
        const crd::math::Vec4f h = inv * ndc[i];
        const crd::f32 inv_w = (std::abs(h.w) > 1e-6F) ? (1.0F / h.w) : 1.0F;
        world[i] = {h.x * inv_w, h.y * inv_w, h.z * inv_w};
    }
    // 12 edges: 4 near, 4 far, 4 connectors.
    constexpr int kEdges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0}, // near rectangle
        {4, 5}, {5, 6}, {6, 7}, {7, 4}, // far  rectangle
        {0, 4}, {1, 5}, {2, 6}, {3, 7}, // connectors
    };
    for (const auto& e : kEdges)
    {
        add_line_to(buf, world[e[0]], world[e[1]], color, width_px, flags, lifetime_s);
    }
}

} // namespace crd::draw
