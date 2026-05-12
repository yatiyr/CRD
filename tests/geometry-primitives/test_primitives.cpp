// crd-geometry-primitives v0a -- primitive types + the helpers migrated from the
// deleted crd/math/geometry.hpp (ADR-0076 sec13). These tests are the verbatim
// behavioural contract that was previously in tests/math/test_math.cpp, plus a
// smoke of the new v0 bare types (Line3/Segment3/OBB3/Capsule3). v0b-v0f extend.

#include <crd/geometry/primitives/format.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/math.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <format>

using namespace crd;
using namespace crd::math; // Vec3f/Vec3d/Mat4f, approx helpers
using namespace crd::geometry::primitives;

namespace
{
template <typename T> void require_vec3_close(const Vec3<T>& actual, const Vec3<T>& expected, T epsilon)
{
    REQUIRE(approx_equal_abs(actual.x, expected.x, epsilon));
    REQUIRE(approx_equal_abs(actual.y, expected.y, epsilon));
    REQUIRE(approx_equal_abs(actual.z, expected.z, epsilon));
}

template <typename T> void require_plane_close(const Plane<T>& actual, const Plane<T>& expected, T epsilon)
{
    require_vec3_close(actual.normal, expected.normal, epsilon);
    REQUIRE(approx_equal_abs(actual.d, expected.d, epsilon));
}
} // namespace

TEST_CASE("primitives: new v0 bare types construct and compare", "[geometry][primitives]")
{
    // These four land their algorithms in v0b/v0c; v0a defines the types.
    const Line3f line(Vec3f(0, 0, 0), Vec3f(1, 0, 0));
    REQUIRE(line == Line3f(Vec3f(0, 0, 0), Vec3f(1, 0, 0)));
    REQUIRE_FALSE(line == Line3f(Vec3f(0, 0, 0), Vec3f(0, 1, 0)));

    const Segment3f seg(Vec3f(-1, 0, 0), Vec3f(1, 0, 0));
    REQUIRE(seg == Segment3f(Vec3f(-1, 0, 0), Vec3f(1, 0, 0)));

    const Capsule3f cap(Vec3f(0, -1, 0), Vec3f(0, 1, 0), 0.5F);
    REQUIRE(cap == Capsule3f(Vec3f(0, -1, 0), Vec3f(0, 1, 0), 0.5F));
    REQUIRE_FALSE(cap == Capsule3f(Vec3f(0, -1, 0), Vec3f(0, 1, 0), 0.6F));

    const OBB3f obb(Vec3f(1, 2, 3), Vec3f(0.5F, 1.0F, 1.5F), Mat3f::identity());
    REQUIRE(obb == OBB3f(Vec3f(1, 2, 3), Vec3f(0.5F, 1.0F, 1.5F), Mat3f::identity()));
    REQUIRE(obb.orientation == Mat3f::identity());

    // Triangle3 -- renamed from the old `Triangle` in the move-and-delete.
    const Triangle3f tri(Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0));
    REQUIRE(tri == Triangle3f(Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0)));
}

TEST_CASE("primitives: utility constructors and relationships behave as expected", "[geometry][primitives]")
{
    const Planed plane = plane_from_point_normal(Vec3d(0.0, 5.0, 0.0), Vec3d(0.0, 2.0, 0.0));
    require_plane_close(plane, Planed(Vec3d(0.0, 1.0, 0.0), -5.0), 1.0e-12);
    REQUIRE(signed_distance(plane, Vec3d(0.0, 8.0, 0.0)) == Catch::Approx(3.0));
    require_vec3_close(closest_point(plane, Vec3d(7.0, 8.0, -2.0)), Vec3d(7.0, 5.0, -2.0), 1.0e-12);

    const AABB3d bounds(Vec3d(-1.0, -2.0, -3.0), Vec3d(3.0, 2.0, 1.0));
    require_vec3_close(center(bounds), Vec3d(1.0, 0.0, -1.0), 1.0e-12);
    require_vec3_close(extents(bounds), Vec3d(2.0, 2.0, 2.0), 1.0e-12);
    require_vec3_close(positive_vertex(bounds, Vec3d(1.0, -1.0, 1.0)), Vec3d(3.0, -2.0, 1.0), 1.0e-12);

    const Triangle3d tri(Vec3d(0.0, 0.0, 0.0), Vec3d(2.0, 0.0, 0.0), Vec3d(0.0, 2.0, 0.0));
    require_vec3_close(centroid(tri), Vec3d(2.0 / 3.0, 2.0 / 3.0, 0.0), 1.0e-12);
    require_vec3_close(normal(tri), Vec3d(0.0, 0.0, 1.0), 1.0e-12);
}

TEST_CASE("primitives: AABB3 / sphere / triangle inside-outside and overlap", "[geometry][primitives]")
{
    const AABB3f bounds(Vec3f(-1.0F, -1.0F, -1.0F), Vec3f(1.0F, 1.0F, 1.0F));
    REQUIRE(contains(bounds, Vec3f(0.25F, 0.5F, -0.5F)));
    REQUIRE_FALSE(contains(bounds, Vec3f(2.0F, 0.0F, 0.0F)));
    require_vec3_close(closest_point(bounds, Vec3f(2.0F, -0.5F, -3.0F)), Vec3f(1.0F, -0.5F, -1.0F), 1.0e-6F);
    REQUIRE(intersects(bounds, AABB3f(Vec3f(0.5F, 0.5F, 0.5F), Vec3f(2.0F, 2.0F, 2.0F))));
    REQUIRE_FALSE(intersects(bounds, AABB3f(Vec3f(2.0F, 2.0F, 2.0F), Vec3f(3.0F, 3.0F, 3.0F))));

    const Spheref s0(Vec3f(0.0F, 0.0F, 0.0F), 1.0F);
    const Spheref s1(Vec3f(1.5F, 0.0F, 0.0F), 1.0F);
    REQUIRE(contains(s0, Vec3f(0.5F, 0.0F, 0.0F)));
    REQUIRE_FALSE(contains(s0, Vec3f(2.0F, 0.0F, 0.0F)));
    REQUIRE(intersects(s0, s1));
    REQUIRE(intersects(bounds, s0));

    const Triangle3f tri(Vec3f(0.0F, 0.0F, 0.0F), Vec3f(2.0F, 0.0F, 0.0F), Vec3f(0.0F, 2.0F, 0.0F));
    const Vec3f bc = barycentric(tri, Vec3f(0.5F, 0.5F, 0.0F));
    REQUIRE(bc.x == Catch::Approx(0.5F));
    REQUIRE(bc.y == Catch::Approx(0.25F));
    REQUIRE(bc.z == Catch::Approx(0.25F));
    REQUIRE(contains(tri, Vec3f(0.5F, 0.5F, 0.0F)));
    REQUIRE_FALSE(contains(tri, Vec3f(2.0F, 2.0F, 0.0F)));
}

TEST_CASE("primitives: ray intersections cover plane / sphere / triangle", "[geometry][primitives][ray]")
{
    const Ray3d ray(Vec3d(0.0, 0.0, -5.0), Vec3d(0.0, 0.0, 1.0));
    const Planed plane = plane_from_point_normal(Vec3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    double t = 0.0;
    REQUIRE(intersect_ray_plane(ray, plane, t));
    REQUIRE(t == Catch::Approx(5.0));
    require_vec3_close(point_at(ray, t), Vec3d(0.0, 0.0, 0.0), 1.0e-12);

    const Sphered sphere(Vec3d(0.0, 0.0, 0.0), 1.0);
    REQUIRE(intersect_ray_sphere(ray, sphere, t));
    REQUIRE(t == Catch::Approx(4.0));

    const Triangle3d tri(Vec3d(-1.0, -1.0, 0.0), Vec3d(1.0, -1.0, 0.0), Vec3d(0.0, 1.0, 0.0));
    Vec3d bary{};
    REQUIRE(intersect_ray_triangle(ray, tri, t, bary));
    REQUIRE(t == Catch::Approx(5.0));
    REQUIRE(bary.x + bary.y + bary.z == Catch::Approx(1.0));

    const Ray3d parallel(Vec3d(0.0, 0.0, -5.0), Vec3d(1.0, 0.0, 0.0));
    REQUIRE_FALSE(intersect_ray_plane(parallel, plane, t));
    REQUIRE_FALSE(intersect_ray_triangle(parallel, tri, t, bary));
}

TEST_CASE("primitives: frustum extraction and containment for the canonical clip volume",
          "[geometry][primitives][frustum]")
{
    const Frustumf frustum = frustum_from_view_projection(Mat4f::identity());
    REQUIRE(contains(frustum, Vec3f(0.0F, 0.0F, 0.0F)));
    REQUIRE_FALSE(contains(frustum, Vec3f(2.0F, 0.0F, 0.0F)));
    REQUIRE(intersects(frustum, Spheref(Vec3f(0.0F, 0.0F, 0.0F), 0.5F)));
    REQUIRE_FALSE(intersects(frustum, Spheref(Vec3f(4.0F, 0.0F, 0.0F), 0.5F)));
    REQUIRE(intersects(frustum, AABB3f(Vec3f(-0.5F, -0.5F, -0.5F), Vec3f(0.5F, 0.5F, 0.5F))));
    REQUIRE_FALSE(intersects(frustum, AABB3f(Vec3f(2.0F, 2.0F, 2.0F), Vec3f(3.0F, 3.0F, 3.0F))));
}

TEST_CASE("primitives: std::format output", "[geometry][primitives][format]")
{
    const Ray3f ray(Vec3f(0.0F, 1.0F, 2.0F), Vec3f(0.0F, 0.0F, -1.0F));
    const Planef plane(Vec3f(0.0F, 1.0F, 0.0F), -5.0F);
    const Spheref sphere(Vec3f(1.0F, 2.0F, 3.0F), 4.0F);
    const AABB3f bounds(Vec3f(-1.0F, -2.0F, -3.0F), Vec3f(4.0F, 5.0F, 6.0F));
    const Triangle3f tri(Vec3f(0.0F, 0.0F, 0.0F), Vec3f(1.0F, 0.0F, 0.0F), Vec3f(0.0F, 1.0F, 0.0F));

    REQUIRE(std::format("{}", ray) == "Ray3(o=Vec3(0, 1, 2), d=Vec3(0, 0, -1))");
    REQUIRE(std::format("{}", plane) == "Plane(n=Vec3(0, 1, 0), d=-5)");
    REQUIRE(std::format("{}", sphere) == "Sphere(c=Vec3(1, 2, 3), r=4)");
    REQUIRE(std::format("{}", bounds) == "AABB3(min=Vec3(-1, -2, -3), max=Vec3(4, 5, 6))");
    REQUIRE(std::format("{}", tri) == "Triangle3(a=Vec3(0, 0, 0), b=Vec3(1, 0, 0), c=Vec3(0, 1, 0))");
}
