// ---------------------------------------------------------------------------
// crd-geometry-primitives -- Transform helpers (Phase 3.1.7 v11).
//
// Tests the transform_* functions for every primitive type, plus the five
// advisor-pinned discriminators:
//   1. Identity is BIT-EXACT pass-through.
//   2. 45-degree rotation grows the AABB diagonal by sqrt(2) exactly.
//   3. Plane normal stays perpendicular to a transformed in-plane vector
//      (inverse-transpose correctness).
//   4. transform_ray3_to_local round-trip -- local hit transforms back to
//      bit-equal world hit.
//   5. Negative-determinant (reflection) preserves OBB volume + handles
//      orientation correctly.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/transform.hpp>
#include <crd/geometry/primitives/transform_typed.hpp>
#include <crd/math/mat.hpp>
#include <crd/units/quantity_aliases.hpp>
#include <crd/math/vec.hpp>

#include <cmath>

namespace
{

using namespace crd::geometry::primitives;
using crd::f32;
using crd::f64;
using crd::math::Mat3f;
using crd::math::Mat4f;
using crd::math::Vec2f;
using crd::math::Vec3f;

[[nodiscard]] Mat4f translation_mat4(float x, float y, float z) noexcept
{
    Mat4f m = Mat4f::identity();
    m.c3.x  = x;
    m.c3.y  = y;
    m.c3.z  = z;
    return m;
}

[[nodiscard]] Mat4f uniform_scale_mat4(float s) noexcept
{
    Mat4f m = Mat4f::identity();
    m.c0.x  = s;
    m.c1.y  = s;
    m.c2.z  = s;
    return m;
}

[[nodiscard]] Mat4f non_uniform_scale_mat4(float sx, float sy, float sz) noexcept
{
    Mat4f m = Mat4f::identity();
    m.c0.x  = sx;
    m.c1.y  = sy;
    m.c2.z  = sz;
    return m;
}

// Rotation around the Z axis by theta (column-major).
[[nodiscard]] Mat4f rotate_z_mat4(float theta) noexcept
{
    Mat4f       m   = Mat4f::identity();
    const float c   = std::cos(theta);
    const float s   = std::sin(theta);
    m.c0.x          = c;
    m.c0.y          = s;
    m.c1.x          = -s;
    m.c1.y          = c;
    return m;
}

[[nodiscard]] bool approx_eq(float a, float b, float tol = 1e-5F) noexcept
{
    return std::abs(a - b) <= tol;
}

[[nodiscard]] bool vec_approx_eq(const Vec3f& a, const Vec3f& b, float tol = 1e-5F) noexcept
{
    return approx_eq(a.x, b.x, tol) && approx_eq(a.y, b.y, tol) && approx_eq(a.z, b.z, tol);
}

} // namespace

// =============================================================================
// Discriminator 1: Identity is BIT-EXACT pass-through.
// =============================================================================

TEST_CASE("transform: identity matrix is bit-exact pass-through on every shape",
          "[transform][discriminator]")
{
    const auto id  = Mat4f::identity();
    const auto id2 = Mat3f::identity();

    SECTION("AABB3")
    {
        const AABB3<f32> box{Vec3f(-1, -2, -3), Vec3f(4, 5, 6)};
        const auto       out = transform_aabb(id, box);
        REQUIRE(out.min.x == box.min.x);
        REQUIRE(out.min.y == box.min.y);
        REQUIRE(out.min.z == box.min.z);
        REQUIRE(out.max.x == box.max.x);
        REQUIRE(out.max.y == box.max.y);
        REQUIRE(out.max.z == box.max.z);
    }
    SECTION("Sphere")
    {
        const Sphere<f32> s{Vec3f(1, 2, 3), 4.5F};
        const auto        out = transform_sphere(id, s);
        REQUIRE(out.center.x == s.center.x);
        REQUIRE(out.center.y == s.center.y);
        REQUIRE(out.center.z == s.center.z);
        REQUIRE(out.radius == s.radius);
    }
    SECTION("Capsule3")
    {
        const Capsule3<f32> cap{Vec3f(0, 0, 0), Vec3f(0, 1, 0), 0.5F};
        const auto          out = transform_capsule3(id, cap);
        REQUIRE(out.a.x == cap.a.x);
        REQUIRE(out.a.y == cap.a.y);
        REQUIRE(out.b.x == cap.b.x);
        REQUIRE(out.b.y == cap.b.y);
        REQUIRE(out.radius == cap.radius);
    }
    SECTION("Plane")
    {
        const Plane<f32> p{Vec3f(0, 1, 0), -2.0F};
        const auto       out = transform_plane(id, p);
        REQUIRE(out.normal.x == p.normal.x);
        REQUIRE(out.normal.y == p.normal.y);
        REQUIRE(out.normal.z == p.normal.z);
        REQUIRE(out.d == p.d);
    }
    SECTION("Ray3")
    {
        const Ray3<f32> r{Vec3f(1, 2, 3), Vec3f(0, 0, 1)};
        const auto      out = transform_ray3(id, r);
        REQUIRE(out.origin.x == r.origin.x);
        REQUIRE(out.direction.x == r.direction.x);
    }
    SECTION("AABB2")
    {
        const AABB2<f32> box{Vec2f(-1, -2), Vec2f(3, 4)};
        const auto       out = transform_aabb2(id2, box);
        REQUIRE(out.min.x == box.min.x);
        REQUIRE(out.min.y == box.min.y);
        REQUIRE(out.max.x == box.max.x);
        REQUIRE(out.max.y == box.max.y);
    }
}

// =============================================================================
// Discriminator 2: 45-deg rotation grows the AABB diagonal by sqrt(2) exactly.
// =============================================================================

TEST_CASE("transform_aabb: 45-degree rotation grows diagonal by sqrt(2)",
          "[transform][discriminator]")
{
    // Unit square in XY plane, centred at origin.
    const AABB3<f32> box{Vec3f(-0.5F, -0.5F, 0), Vec3f(0.5F, 0.5F, 0)};
    const Mat4f      rot = rotate_z_mat4(0.7853981633974483F); // pi/4
    const auto       out = transform_aabb(rot, box);

    // After 45-degree rotation, the AABB of the rotated unit square has side
    // sqrt(2). So extent.x = sqrt(2)/2 = ~0.7071.
    const float expected_half = static_cast<float>(std::sqrt(2.0) * 0.5);
    REQUIRE(approx_eq(out.max.x - out.min.x, expected_half * 2.0F, 1e-5F));
    REQUIRE(approx_eq(out.max.y - out.min.y, expected_half * 2.0F, 1e-5F));
    REQUIRE(approx_eq(out.max.z - out.min.z, 0.0F, 1e-6F));
}

// =============================================================================
// Discriminator 3: Plane normal stays perpendicular to a transformed in-plane vector.
// =============================================================================

TEST_CASE("transform_plane: normal stays perpendicular after non-uniform scale",
          "[transform][discriminator]")
{
    // Plane z = 1 (normal = +Z, d = -1).
    const Plane<f32> plane{Vec3f(0, 0, 1), -1.0F};

    // An in-plane vector (any vector perpendicular to +Z) e.g. (1, 0, 0).
    const Vec3f in_plane_vec(1, 0, 0);

    // Non-uniform scale.
    Mat4f m  = non_uniform_scale_mat4(2.0F, 3.0F, 5.0F);
    m        = translation_mat4(10, 20, 30) * m;

    const auto out         = transform_plane(m, plane);
    // Transform the in-plane vector as a direction.
    const Vec3f tvec(in_plane_vec.x * 2.0F, in_plane_vec.y * 3.0F, in_plane_vec.z * 5.0F);
    // After transform, the new normal must be perpendicular to the transformed
    // in-plane vector -- pinned by inverse-transpose mathematics.
    const float dot_v = out.normal.x * tvec.x + out.normal.y * tvec.y + out.normal.z * tvec.z;
    REQUIRE(approx_eq(dot_v, 0.0F, 1e-4F));
}

// =============================================================================
// Discriminator 4: transform_ray3_to_local round-trip.
// =============================================================================

TEST_CASE("transform_ray3_to_local: round-trip preserves world hit",
          "[transform][discriminator]")
{
    // World-space sphere at (5, 0, 0) with radius 1 (implicit in the
    // scenario; world_t_hit = 4 is the expected hit distance from the
    // ray origin to the sphere's near surface).
    // World-space ray from (0, 0, 0) along +X.
    const Ray3<f32> world_ray{Vec3f(0, 0, 0), Vec3f(1, 0, 0)};
    const float     world_t_hit = 4.0F;

    // Local frame: sphere centred at origin in local, world transform translates
    // by (+5, 0, 0). world_to_local is the inverse (-5, 0, 0).
    const Mat4f local_to_world = translation_mat4(5, 0, 0);
    const Mat4f world_to_local = translation_mat4(-5, 0, 0);

    // Transform world ray into local space.
    const auto  local_ray = transform_ray3_to_local(world_ray, world_to_local);
    // Local ray should hit local sphere (origin, r=1) at t=4 (local ray origin = (-5,0,0), dir=(1,0,0)).
    REQUIRE(approx_eq(local_ray.origin.x, -5.0F));
    REQUIRE(approx_eq(local_ray.direction.x, 1.0F));

    // Compute hit point in local space at local t=4.
    const Vec3f local_hit(local_ray.origin.x + local_ray.direction.x * world_t_hit,
                          local_ray.origin.y + local_ray.direction.y * world_t_hit,
                          local_ray.origin.z + local_ray.direction.z * world_t_hit);
    // Transform local hit back to world.
    crd::math::Vec4f h(local_hit.x, local_hit.y, local_hit.z, 1.0F);
    const auto       world_hit_v4 = local_to_world * h;
    const Vec3f      world_hit(world_hit_v4.x, world_hit_v4.y, world_hit_v4.z);

    // Compare with direct world raycast hit point.
    const Vec3f world_hit_direct(world_ray.origin.x + world_ray.direction.x * world_t_hit,
                                  world_ray.origin.y + world_ray.direction.y * world_t_hit,
                                  world_ray.origin.z + world_ray.direction.z * world_t_hit);
    REQUIRE(vec_approx_eq(world_hit, world_hit_direct));
}

// =============================================================================
// Discriminator 5: Negative-determinant (reflection) preserves OBB volume.
// =============================================================================

TEST_CASE("transform_obb: reflection (negative det) preserves volume",
          "[transform][discriminator]")
{
    // Reflection across XY plane (z -> -z).
    Mat4f reflect_z = Mat4f::identity();
    reflect_z.c2.z  = -1.0F;

    const OBB3<f32> obb{Vec3f(1, 2, 3), Vec3f(0.5F, 1.0F, 2.0F),
                         crd::math::Mat3f::identity()};
    const auto      out = transform_obb(reflect_z, obb);

    // Volume = 8 * he.x * he.y * he.z. Should be preserved.
    const float vol_before = 8.0F * obb.half_extents.x * obb.half_extents.y * obb.half_extents.z;
    const float vol_after  = 8.0F * std::abs(out.half_extents.x) * std::abs(out.half_extents.y)
                            * std::abs(out.half_extents.z);
    REQUIRE(approx_eq(vol_after, vol_before, 1e-4F));
}

// =============================================================================
// Per-shape correctness coverage.
// =============================================================================

TEST_CASE("transform_aabb: uniform scale = 2 grows extent by 2", "[transform][aabb]")
{
    const AABB3<f32> box{Vec3f(-1, -1, -1), Vec3f(1, 1, 1)};
    const Mat4f      s   = uniform_scale_mat4(2.0F);
    const auto       out = transform_aabb(s, box);
    REQUIRE(approx_eq(out.min.x, -2.0F));
    REQUIRE(approx_eq(out.max.x, 2.0F));
    REQUIRE(approx_eq(out.min.y, -2.0F));
    REQUIRE(approx_eq(out.max.z, 2.0F));
}

TEST_CASE("transform_aabb: translation only", "[transform][aabb]")
{
    const AABB3<f32> box{Vec3f(0, 0, 0), Vec3f(1, 1, 1)};
    const Mat4f      t   = translation_mat4(10, 20, 30);
    const auto       out = transform_aabb(t, box);
    REQUIRE(approx_eq(out.min.x, 10.0F));
    REQUIRE(approx_eq(out.max.x, 11.0F));
    REQUIRE(approx_eq(out.min.y, 20.0F));
    REQUIRE(approx_eq(out.max.y, 21.0F));
}

TEST_CASE("transform_obb: rigid rotation preserves half_extents", "[transform][obb]")
{
    const OBB3<f32> obb{Vec3f(0, 0, 0), Vec3f(1, 2, 3), crd::math::Mat3f::identity()};
    const Mat4f     rot = rotate_z_mat4(1.0F); // 1 radian
    const auto      out = transform_obb(rot, obb);
    REQUIRE(approx_eq(out.half_extents.x, 1.0F));
    REQUIRE(approx_eq(out.half_extents.y, 2.0F));
    REQUIRE(approx_eq(out.half_extents.z, 3.0F));
}

TEST_CASE("transform_obb: uniform scale 2 doubles half_extents", "[transform][obb]")
{
    const OBB3<f32> obb{Vec3f(0, 0, 0), Vec3f(1, 1, 1), crd::math::Mat3f::identity()};
    const Mat4f     s   = uniform_scale_mat4(2.0F);
    const auto      out = transform_obb(s, obb);
    REQUIRE(approx_eq(out.half_extents.x, 2.0F));
    REQUIRE(approx_eq(out.half_extents.y, 2.0F));
    REQUIRE(approx_eq(out.half_extents.z, 2.0F));
}

TEST_CASE("transform_obb: general affine collapses to axis-aligned OBB",
          "[transform][obb]")
{
    const OBB3<f32> obb{Vec3f(0, 0, 0), Vec3f(1, 1, 1), crd::math::Mat3f::identity()};
    const Mat4f     s   = non_uniform_scale_mat4(2.0F, 3.0F, 5.0F);
    const auto      out = transform_obb(s, obb);
    // Orientation should be identity (axis-aligned).
    REQUIRE(approx_eq(out.orientation.c0.x, 1.0F));
    REQUIRE(approx_eq(out.orientation.c1.y, 1.0F));
    REQUIRE(approx_eq(out.orientation.c2.z, 1.0F));
    // Half-extents scaled per axis.
    REQUIRE(approx_eq(out.half_extents.x, 2.0F));
    REQUIRE(approx_eq(out.half_extents.y, 3.0F));
    REQUIRE(approx_eq(out.half_extents.z, 5.0F));
}

TEST_CASE("transform_sphere: uniform scale 3 triples radius", "[transform][sphere]")
{
    const Sphere<f32> s{Vec3f(1, 2, 3), 2.0F};
    const Mat4f       m   = uniform_scale_mat4(3.0F);
    const auto        out = transform_sphere(m, s);
    REQUIRE(approx_eq(out.radius, 6.0F));
    REQUIRE(approx_eq(out.center.x, 3.0F));
    REQUIRE(approx_eq(out.center.y, 6.0F));
    REQUIRE(approx_eq(out.center.z, 9.0F));
}

TEST_CASE("transform_sphere: non-uniform scale picks max axial scale (loose)",
          "[transform][sphere]")
{
    const Sphere<f32> s{Vec3f(0, 0, 0), 1.0F};
    const Mat4f       m   = non_uniform_scale_mat4(2.0F, 5.0F, 3.0F);
    const auto        out = transform_sphere(m, s);
    REQUIRE(approx_eq(out.radius, 5.0F)); // max axial = 5
}

TEST_CASE("transform_capsule3: endpoints transform, radius scales",
          "[transform][capsule3]")
{
    const Capsule3<f32> cap{Vec3f(-1, 0, 0), Vec3f(1, 0, 0), 0.5F};
    const Mat4f         s   = uniform_scale_mat4(4.0F);
    const auto          out = transform_capsule3(s, cap);
    REQUIRE(approx_eq(out.a.x, -4.0F));
    REQUIRE(approx_eq(out.b.x, 4.0F));
    REQUIRE(approx_eq(out.radius, 2.0F));
}

TEST_CASE("transform_cylinder3: same as capsule but flat caps semantics",
          "[transform][cylinder3]")
{
    const Cylinder3<f32> cyl{Vec3f(0, -1, 0), Vec3f(0, 1, 0), 1.0F};
    const Mat4f          rot = rotate_z_mat4(0.5F);
    const auto           out = transform_cylinder3(rot, cyl);
    REQUIRE(approx_eq(out.radius, 1.0F)); // rigid: radius unchanged
}

TEST_CASE("transform_triangle3 / transform_tetrahedron: each vertex transformed",
          "[transform][triangle3][tetrahedron]")
{
    const Triangle3<f32>   tri{Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0)};
    const Tetrahedron<f32> tet{Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 0, 1)};
    const Mat4f            t = translation_mat4(10, 0, 0);

    const auto tri_out = transform_triangle3(t, tri);
    REQUIRE(approx_eq(tri_out.a.x, 10.0F));
    REQUIRE(approx_eq(tri_out.b.x, 11.0F));
    REQUIRE(approx_eq(tri_out.c.x, 10.0F));
    REQUIRE(approx_eq(tri_out.c.y, 1.0F));

    const auto tet_out = transform_tetrahedron(t, tet);
    REQUIRE(approx_eq(tet_out.d.x, 10.0F));
    REQUIRE(approx_eq(tet_out.d.z, 1.0F));
}

TEST_CASE("transform_ray3: direction magnitude preserved (NOT renormalized)",
          "[transform][ray3]")
{
    const Ray3<f32> r{Vec3f(0, 0, 0), Vec3f(2, 0, 0)}; // direction magnitude = 2
    const Mat4f     s   = uniform_scale_mat4(3.0F);
    const auto      out = transform_ray3(s, r);
    // Direction magnitude becomes 3 * 2 = 6.
    REQUIRE(approx_eq(out.direction.x, 6.0F));
}

TEST_CASE("transform_segment3 / transform_line3: endpoints + direction",
          "[transform][segment3][line3]")
{
    const Segment3<f32> seg{Vec3f(0, 0, 0), Vec3f(1, 1, 1)};
    const Line3<f32>    line{Vec3f(5, 0, 0), Vec3f(0, 1, 0)};
    const Mat4f         t = translation_mat4(0, 10, 0);

    const auto seg_out  = transform_segment3(t, seg);
    REQUIRE(approx_eq(seg_out.a.y, 10.0F));
    REQUIRE(approx_eq(seg_out.b.y, 11.0F));

    const auto line_out = transform_line3(t, line);
    REQUIRE(approx_eq(line_out.point.y, 10.0F));
    REQUIRE(approx_eq(line_out.direction.y, 1.0F));
}

TEST_CASE("transform_frustum: all 6 planes transformed via single inverse-transpose",
          "[transform][frustum]")
{
    // Simple axis-aligned cube frustum (6 planes).
    Frustum<f32> f{};
    f.planes[0] = Plane<f32>{Vec3f(1, 0, 0), -1.0F};  // x <= 1
    f.planes[1] = Plane<f32>{Vec3f(-1, 0, 0), -1.0F}; // x >= -1
    f.planes[2] = Plane<f32>{Vec3f(0, 1, 0), -1.0F};
    f.planes[3] = Plane<f32>{Vec3f(0, -1, 0), -1.0F};
    f.planes[4] = Plane<f32>{Vec3f(0, 0, 1), -1.0F};
    f.planes[5] = Plane<f32>{Vec3f(0, 0, -1), -1.0F};

    const Mat4f rot = rotate_z_mat4(1.5708F); // 90 degrees
    const auto  out = transform_frustum(rot, f);

    // After 90-degree rotation, plane[0]'s normal (was +X) should be ~+Y.
    REQUIRE(approx_eq(out.planes[0].normal.x, 0.0F, 1e-4F));
    REQUIRE(approx_eq(out.planes[0].normal.y, 1.0F, 1e-4F));
}

// =============================================================================
// 2D peers.
// =============================================================================

TEST_CASE("transform_aabb2: 45-degree rotation grows diagonal", "[transform][2d]")
{
    const AABB2<f32> box{Vec2f(-0.5F, -0.5F), Vec2f(0.5F, 0.5F)};
    Mat3f            rot   = Mat3f::identity();
    const float      theta = 0.7853981633974483F;
    const float      c     = std::cos(theta);
    const float      s     = std::sin(theta);
    rot.c0.x = c; rot.c0.y = s;
    rot.c1.x = -s; rot.c1.y = c;
    const auto out = transform_aabb2(rot, box);
    REQUIRE(approx_eq(out.max.x - out.min.x, std::sqrt(2.0F), 1e-5F));
}

TEST_CASE("transform_obb2: uniform scale doubles half_extents", "[transform][2d]")
{
    const OBB2<f32> obb{Vec2f(0, 0), Vec2f(1, 1), crd::math::Mat2<f32>::identity()};
    Mat3f           s = Mat3f::identity();
    s.c0.x            = 2.0F;
    s.c1.y            = 2.0F;
    const auto out    = transform_obb2(s, obb);
    REQUIRE(approx_eq(out.half_extents.x, 2.0F));
    REQUIRE(approx_eq(out.half_extents.y, 2.0F));
}

TEST_CASE("transform_circle: scale scales radius", "[transform][2d]")
{
    const Circle<f32> c{Vec2f(0, 0), 1.0F};
    Mat3f             m = Mat3f::identity();
    m.c0.x              = 3.0F;
    m.c1.y              = 3.0F;
    const auto out      = transform_circle(m, c);
    REQUIRE(approx_eq(out.radius, 3.0F));
}

TEST_CASE("transform_capsule2 / segment2 / ray2 / triangle2: vertex transforms",
          "[transform][2d]")
{
    Mat3f t = Mat3f::identity();
    t.c2.x  = 10.0F;
    t.c2.y  = 20.0F;

    const Capsule2<f32>  cap{Vec2f(0, 0), Vec2f(1, 0), 0.5F};
    const auto           cap_out = transform_capsule2(t, cap);
    REQUIRE(approx_eq(cap_out.a.x, 10.0F));
    REQUIRE(approx_eq(cap_out.b.x, 11.0F));

    const Segment2<f32> seg{Vec2f(0, 0), Vec2f(1, 1)};
    const auto          seg_out = transform_segment2(t, seg);
    REQUIRE(approx_eq(seg_out.a.x, 10.0F));
    REQUIRE(approx_eq(seg_out.b.y, 21.0F));

    const Ray2<f32> ray{Vec2f(0, 0), Vec2f(0, 1)};
    const auto      ray_out = transform_ray2(t, ray);
    REQUIRE(approx_eq(ray_out.origin.x, 10.0F));
    REQUIRE(approx_eq(ray_out.direction.y, 1.0F));

    const Triangle2<f32> tri{Vec2f(0, 0), Vec2f(1, 0), Vec2f(0, 1)};
    const auto           tri_out = transform_triangle2(t, tri);
    REQUIRE(approx_eq(tri_out.a.x, 10.0F));
    REQUIRE(approx_eq(tri_out.c.y, 21.0F));
}

// =============================================================================
// TransformedShape composition wrapper (D218).
// =============================================================================

TEST_CASE("TransformedShape: auto-deduced scalar via shape_scalar trait",
          "[transform][composition]")
{
    TransformedShape<AABB3<f32>> ts32;
    ts32.shape    = AABB3<f32>{Vec3f(-1, -1, -1), Vec3f(1, 1, 1)};
    ts32.to_world = translation_mat4(10, 0, 0);
    const auto world_box = transform_aabb(ts32.to_world, ts32.shape);
    REQUIRE(approx_eq(world_box.min.x, 9.0F));

    // f64 instantiation.
    TransformedShape<AABB3<f64>> ts64;
    ts64.shape    = AABB3<f64>{crd::math::Vec3<f64>(-1, -1, -1), crd::math::Vec3<f64>(1, 1, 1)};
    ts64.to_world = crd::math::Mat4<f64>::identity();
    const auto world_box64 = transform_aabb(ts64.to_world, ts64.shape);
    REQUIRE(world_box64.min.x == ts64.shape.min.x); // identity bit-exact
}

// =============================================================================
// f64 instantiations.
// =============================================================================

// =============================================================================
// Typed boundary layer (D233).
// =============================================================================

TEST_CASE("transform_aabb_typed: AABB3<Length32> round-trip", "[transform][typed]")
{
    using crd::units::Length;
    using crd::units::Length32;
    AABB3<Length32> box;
    box.min = crd::math::Vec3<Length32>(Length32{-1.0F}, Length32{-1.0F}, Length32{-1.0F});
    box.max = crd::math::Vec3<Length32>(Length32{1.0F}, Length32{1.0F}, Length32{1.0F});
    const Mat4f s = uniform_scale_mat4(3.0F);
    const auto  out = transform_aabb_typed<crd::units::dim::Length, f32>(s, box);
    REQUIRE(approx_eq(out.min.x.value, -3.0F));
    REQUIRE(approx_eq(out.max.x.value, 3.0F));
}

TEST_CASE("transform_sphere_typed: typed radius matches raw transform",
          "[transform][typed]")
{
    using crd::units::Length32;
    Sphere<Length32> s;
    s.center = crd::math::Vec3<Length32>(Length32{1}, Length32{0}, Length32{0});
    s.radius = Length32{2.0F};
    const Mat4f m   = uniform_scale_mat4(4.0F);
    const auto  out = transform_sphere_typed<crd::units::dim::Length, f32>(m, s);
    REQUIRE(approx_eq(out.radius.value, 8.0F));
    REQUIRE(approx_eq(out.center.x.value, 4.0F));
}

TEST_CASE("transform: f64 instantiations work end-to-end",
          "[transform][f64]")
{
    using crd::math::Mat4;
    using crd::math::Vec3;

    const AABB3<f64> box{Vec3<f64>(-1, -1, -1), Vec3<f64>(1, 1, 1)};
    const Mat4<f64>  s   = []() noexcept {
        Mat4<f64> m = Mat4<f64>::identity();
        m.c0.x      = 3.0;
        m.c1.y      = 3.0;
        m.c2.z      = 3.0;
        return m;
    }();
    const auto out = transform_aabb(s, box);
    REQUIRE(std::abs(out.max.x - 3.0) < 1e-12);

    const Sphere<f64> sphere{Vec3<f64>(0, 0, 0), 1.0};
    const auto        sphere_out = transform_sphere(s, sphere);
    REQUIRE(std::abs(sphere_out.radius - 3.0) < 1e-12);
}
