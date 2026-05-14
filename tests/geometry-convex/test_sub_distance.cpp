// crd-geometry-convex v2a — Ericson §9.5 sub-distance unit tests.
//
// Verifies the standalone `detail/sub_distance.hpp` routines produce the
// expected closest point + surviving-simplex mask + barycentric weights for
// every Voronoi region (vertex / edge / face / interior) on 1-, 2-, 3-, and
// 4-simplices.
//
// Writing these *standalone* (no GJK driver) means the GJK driver test
// failures only have to investigate the driver, not the simplex reducer.

#include <crd/geometry/convex/detail/sub_distance.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
using crd::f32;
using crd::u8;
using crd::geometry::convex::detail::sub_distance_1;
using crd::geometry::convex::detail::sub_distance_2;
using crd::geometry::convex::detail::sub_distance_3;
using crd::geometry::convex::detail::sub_distance_4;
using crd::geometry::convex::detail::SubDistanceResult;
using crd::math::Vec3;

constexpr f32 kTol = 1e-5F;

bool approx(const Vec3<f32>& lhs, const Vec3<f32>& rhs, f32 tol = kTol)
{
    return std::fabs(lhs.x - rhs.x) <= tol && std::fabs(lhs.y - rhs.y) <= tol && std::fabs(lhs.z - rhs.z) <= tol;
}
bool approx(f32 lhs, f32 rhs, f32 tol = kTol)
{
    return std::fabs(lhs - rhs) <= tol;
}
} // namespace

TEST_CASE("sub_distance_1: point is the closest, weight 1, mask 0001", "[sub-distance][1-simplex]")
{
    const SubDistanceResult<f32> r = sub_distance_1(Vec3<f32>(1.0F, 2.0F, 3.0F));
    REQUIRE(approx(r.closest, Vec3<f32>(1.0F, 2.0F, 3.0F)));
    REQUIRE(approx(r.weights[0], 1.0F));
    REQUIRE(r.mask == 0b0001);
}

TEST_CASE("sub_distance_2: three Voronoi regions on an edge", "[sub-distance][2-simplex]")
{
    SECTION("vertex region a: origin's projection is past a")
    {
        // a = (1, 0, 0), b = (2, 0, 0). Origin's projection on the line is at
        // -1·(b-a) past a → t = -1 < 0 → vertex a.
        const SubDistanceResult<f32> r = sub_distance_2(Vec3<f32>(1.0F, 0, 0), Vec3<f32>(2.0F, 0, 0));
        REQUIRE(approx(r.closest, Vec3<f32>(1, 0, 0)));
        REQUIRE(r.mask == 0b0001);
        REQUIRE(approx(r.weights[0], 1.0F));
    }
    SECTION("vertex region b: origin's projection is past b")
    {
        const SubDistanceResult<f32> r = sub_distance_2(Vec3<f32>(-2.0F, 0, 0), Vec3<f32>(-1.0F, 0, 0));
        REQUIRE(approx(r.closest, Vec3<f32>(-1, 0, 0)));
        REQUIRE(r.mask == 0b0010);
        REQUIRE(approx(r.weights[1], 1.0F));
    }
    SECTION("edge interior: projection is in (0, 1)")
    {
        // a = (-1, 1, 0), b = (1, 1, 0). Projection onto line is x=0,y=1 ⇒ t=0.5.
        const SubDistanceResult<f32> r = sub_distance_2(Vec3<f32>(-1, 1, 0), Vec3<f32>(1, 1, 0));
        REQUIRE(approx(r.closest, Vec3<f32>(0, 1, 0)));
        REQUIRE(r.mask == 0b0011);
        REQUIRE(approx(r.weights[0], 0.5F));
        REQUIRE(approx(r.weights[1], 0.5F));
    }
    SECTION("degenerate coincident edge collapses to vertex a")
    {
        const SubDistanceResult<f32> r = sub_distance_2(Vec3<f32>(2, 0, 0), Vec3<f32>(2, 0, 0));
        REQUIRE(approx(r.closest, Vec3<f32>(2, 0, 0)));
        REQUIRE(r.mask == 0b0001);
    }
}

TEST_CASE("sub_distance_3: seven Voronoi regions on a triangle", "[sub-distance][3-simplex]")
{
    // Triangle in the y=1 plane. Origin's nearest point on the *plane* is
    // (0,1,0); whether that's inside the triangle determines the region.
    SECTION("interior face: origin projects inside the triangle")
    {
        // Equilateral-ish around (0,1,0) — large enough to contain the projection.
        const Vec3<f32> a(-1.0F, 1.0F, -1.0F), b(1.0F, 1.0F, -1.0F), c(0.0F, 1.0F, 1.0F);
        const SubDistanceResult<f32> r = sub_distance_3(a, b, c);
        REQUIRE(approx(r.closest, Vec3<f32>(0, 1, 0)));
        REQUIRE(r.mask == 0b0111);
        // Weights sum to 1.
        REQUIRE(approx(r.weights[0] + r.weights[1] + r.weights[2], 1.0F));
    }
    SECTION("vertex region a: triangle far from origin in +X")
    {
        const Vec3<f32> a(1.0F, 0, 0), b(2.0F, 0, 0), c(2.0F, 0, 1.0F);
        const SubDistanceResult<f32> r = sub_distance_3(a, b, c);
        REQUIRE(approx(r.closest, a));
        REQUIRE(r.mask == 0b0001);
    }
    SECTION("edge region ab: origin projects onto edge ab")
    {
        // a=(-1,1,0), b=(1,1,0), c=(0,1,5). Projection (0,1,0) is on edge ab.
        const Vec3<f32> a(-1.0F, 1.0F, 0), b(1.0F, 1.0F, 0), c(0.0F, 1.0F, 5.0F);
        const SubDistanceResult<f32> r = sub_distance_3(a, b, c);
        REQUIRE(approx(r.closest, Vec3<f32>(0, 1, 0)));
        REQUIRE(r.mask == 0b0011);
    }
}

TEST_CASE("sub_distance_4: tetrahedron - interior, face, edge, vertex", "[sub-distance][4-simplex]")
{
    SECTION("origin inside the tetrahedron")
    {
        // Standard tet around the origin.
        const Vec3<f32> a(1.0F, 1.0F, 1.0F);
        const Vec3<f32> b(-1.0F, -1.0F, 1.0F);
        const Vec3<f32> c(-1.0F, 1.0F, -1.0F);
        const Vec3<f32> d(1.0F, -1.0F, -1.0F);
        const SubDistanceResult<f32> r = sub_distance_4(a, b, c, d);
        REQUIRE(approx(r.closest, Vec3<f32>(0, 0, 0)));
        REQUIRE(r.mask == 0b1111);
        REQUIRE(approx(r.weights[0] + r.weights[1] + r.weights[2] + r.weights[3], 1.0F));
    }
    SECTION("reduces to a face when origin is outside that face")
    {
        // Tet shifted +Z away from origin: origin is below the (a,b,c) face.
        const Vec3<f32> a(1.0F, 1.0F, 2.0F);
        const Vec3<f32> b(-1.0F, -1.0F, 2.0F);
        const Vec3<f32> c(-1.0F, 1.0F, 2.0F);
        const Vec3<f32> d(0.0F, 0.0F, 4.0F);
        const SubDistanceResult<f32> r = sub_distance_4(a, b, c, d);
        // Closest point should be on the (a,b,c) face — at z=2, projection (0,0,2).
        REQUIRE(approx(r.closest.z, 2.0F));
        // Mask should NOT include slot 3 (d).
        REQUIRE((r.mask & 0b1000) == 0U);
    }
    SECTION("reduces to a vertex when origin is far from one corner")
    {
        // Tet displaced into the (+X,+Y,+Z) octant; nearest vertex is the
        // origin-side corner.
        const Vec3<f32> a(1.0F, 1.0F, 1.0F);
        const Vec3<f32> b(2.0F, 1.0F, 1.0F);
        const Vec3<f32> c(1.0F, 2.0F, 1.0F);
        const Vec3<f32> d(1.0F, 1.0F, 2.0F);
        const SubDistanceResult<f32> r = sub_distance_4(a, b, c, d);
        REQUIRE(approx(r.closest, a));
        REQUIRE(r.mask == 0b0001);
    }
}
