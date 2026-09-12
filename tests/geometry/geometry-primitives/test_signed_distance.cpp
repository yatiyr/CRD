// crd-geometry-primitives v1h -- the iq analytic signed-distance functions.
// Three test families:
//   * STRONG cross-checks where a matching primitive + its distance() exists:
//     sd_sphere/box/circle/box_2d/segment_2d/capsule/plane/triangle agree with
//     closest_point.hpp's distance() (and the sign agrees with contains()).
//   * SPOT checks of closed-form values at axis points for the rest
//     (round_box / box_frame / cone / torus / ellipsoid / octahedron / polygons).
//   * 1-LIPSCHITZ property |sd(p)-sd(q)| <= |p-q| on random pairs (a true SDF
//     never changes faster than distance; iq's ellipsoid bound is only
//     approximately Lipschitz so it is exempted).

#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/signed_distance.hpp>
#include <crd/math/math.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

using namespace crd;
using namespace crd::math;
using namespace crd::geometry::primitives;

namespace
{
constexpr f32 kTol = 1.0e-4F;
constexpr f32 kSqrt3 = std::numbers::sqrt3_v<f32>;

// splitmix64 -> f32 in [-range, range]
struct Rng
{
    u64 s;
    explicit Rng(u64 seed) noexcept : s(seed) {}
    u64 next() noexcept
    {
        u64 z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    f32 uniform(f32 lo, f32 hi) noexcept
    {
        const f32 u = static_cast<f32>(next() >> 40) / static_cast<f32>(1ULL << 24);
        return lo + (hi - lo) * u;
    }
    Vec3<f32> p3(f32 r) noexcept { return Vec3<f32>(uniform(-r, r), uniform(-r, r), uniform(-r, r)); }
    Vec2<f32> p2(f32 r) noexcept { return Vec2<f32>(uniform(-r, r), uniform(-r, r)); }
};

void check_lipschitz_3d(auto sd, Rng& rng, int n = 400)
{
    for (int i = 0; i < n; ++i)
    {
        const Vec3<f32> p = rng.p3(5.0F);
        const Vec3<f32> q = rng.p3(5.0F);
        const f32 lhs = std::abs(sd(p) - sd(q));
        const f32 rhs = length(p - q);
        REQUIRE(lhs <= rhs + 1.0e-3F * rhs + 1.0e-4F);
    }
}
void check_lipschitz_2d(auto sd, Rng& rng, int n = 400)
{
    for (int i = 0; i < n; ++i)
    {
        const Vec2<f32> p = rng.p2(5.0F);
        const Vec2<f32> q = rng.p2(5.0F);
        const f32 lhs = std::abs(sd(p) - sd(q));
        const f32 rhs = length(p - q);
        REQUIRE(lhs <= rhs + 1.0e-3F * rhs + 1.0e-4F);
    }
}
} // namespace

TEST_CASE("sd_sphere: matches distance(Sphere) magnitude + sign matches contains", "[geometry][sdf]")
{
    Rng rng(1);
    const Sphere<f32> s(Vec3<f32>(0, 0, 0), 1.3F);
    for (int i = 0; i < 500; ++i)
    {
        const Vec3<f32> p = rng.p3(4.0F);
        const f32 d = sd_sphere(p, 1.3F);
        REQUIRE(approx_equal_abs(std::abs(d), distance(s, p), kTol));
        REQUIRE((d <= 0.0F) == contains(s, p));
    }
    REQUIRE(approx_equal_abs(sd_sphere(Vec3<f32>(1.3F, 0, 0), 1.3F), 0.0F, kTol));
    check_lipschitz_3d([](const Vec3<f32>& p) { return sd_sphere(p, 1.3F); }, rng);
}

TEST_CASE("sd_box: outside == distance(AABB), sign == contains, surface == 0", "[geometry][sdf]")
{
    Rng rng(2);
    const Vec3<f32> b(0.7F, 1.1F, 0.4F);
    const AABB3<f32> box(-b, b);
    for (int i = 0; i < 600; ++i)
    {
        const Vec3<f32> p = rng.p3(3.0F);
        const f32 d = sd_box(p, b);
        REQUIRE((d <= kTol) == (contains(box, p) || std::abs(d) <= kTol));
        if (!contains(box, p))
        {
            REQUIRE(approx_equal_abs(d, distance(box, p), kTol));
            REQUIRE(d > -kTol);
        }
        else
        {
            REQUIRE(d <= kTol);
        }
    }
    REQUIRE(approx_equal_abs(sd_box(Vec3<f32>(0.7F, 0, 0), b), 0.0F, kTol));
    REQUIRE(approx_equal_abs(sd_box(Vec3<f32>(0, 0, 0), b), -0.4F, kTol)); // -min half-extent
    check_lipschitz_3d([&](const Vec3<f32>& p) { return sd_box(p, b); }, rng);
}

TEST_CASE("sd_round_box: closed-form value at origin + on a face; Lipschitz", "[geometry][sdf]")
{
    Rng rng(3);
    const Vec3<f32> b(1.0F, 1.2F, 0.9F);
    const f32 r = 0.2F;
    REQUIRE(approx_equal_abs(sd_round_box(Vec3<f32>(0, 0, 0), b, r), -0.9F, kTol));   // -min(b)
    REQUIRE(approx_equal_abs(sd_round_box(Vec3<f32>(1.0F, 0, 0), b, r), 0.0F, kTol)); // on the +x face
    REQUIRE(sd_round_box(Vec3<f32>(3, 0, 0), b, r) > 0.0F);
    check_lipschitz_3d([&](const Vec3<f32>& p) { return sd_round_box(p, b, r); }, rng);
}

TEST_CASE("sd_box_frame: hollow centre is outside, a corner is on the frame; Lipschitz", "[geometry][sdf]")
{
    Rng rng(4);
    const Vec3<f32> b(1.0F, 1.0F, 1.0F);
    const f32 e = 0.1F;
    REQUIRE(sd_box_frame(Vec3<f32>(0, 0, 0), b, e) > 0.0F);                         // the frame is hollow
    REQUIRE(std::abs(sd_box_frame(Vec3<f32>(1.0F, 1.0F, 1.0F), b, e)) <= e + kTol); // a corner strut
    REQUIRE(std::abs(sd_box_frame(Vec3<f32>(1.0F, 1.0F, 0.0F), b, e)) <= kTol);     // mid-edge: on the strut
    check_lipschitz_3d([&](const Vec3<f32>& p) { return sd_box_frame(p, b, e); }, rng);
}

TEST_CASE("sd_plane: equals signed_distance(Plane)", "[geometry][sdf]")
{
    Rng rng(5);
    const Vec3<f32> n = normalized(Vec3<f32>(1.0F, 2.0F, -1.0F));
    const f32 h = -0.4F;
    const Plane<f32> pl(n, h);
    for (int i = 0; i < 300; ++i)
    {
        const Vec3<f32> p = rng.p3(4.0F);
        REQUIRE(approx_equal_abs(sd_plane(p, n, h), signed_distance(pl, p), kTol));
    }
}

TEST_CASE("sd_capsule: equals distance(Segment3) - r; Lipschitz", "[geometry][sdf]")
{
    Rng rng(6);
    const Vec3<f32> a(-0.5F, 0.2F, 0.1F);
    const Vec3<f32> b(0.8F, -0.3F, 0.4F);
    const f32 r = 0.35F;
    const Segment3<f32> seg(a, b);
    for (int i = 0; i < 500; ++i)
    {
        const Vec3<f32> p = rng.p3(3.0F);
        REQUIRE(approx_equal_abs(sd_capsule(p, a, b, r), distance(seg, p) - r, kTol));
    }
    REQUIRE(approx_equal_abs(sd_capsule(a, a, b, r), -r, kTol)); // at an endpoint
    check_lipschitz_3d([&](const Vec3<f32>& p) { return sd_capsule(p, a, b, r); }, rng);
}

TEST_CASE("sd_cylinder: inside negative, far positive, on the round side ~0; Lipschitz", "[geometry][sdf]")
{
    Rng rng(7);
    const Vec3<f32> a(0, -1.0F, 0);
    const Vec3<f32> b(0, 1.0F, 0);
    const f32 r = 0.5F;
    REQUIRE(sd_cylinder(Vec3<f32>(0, 0, 0), a, b, r) < 0.0F);               // dead centre
    REQUIRE(std::abs(sd_cylinder(Vec3<f32>(0.5F, 0, 0), a, b, r)) <= kTol); // on the curved surface
    REQUIRE(std::abs(sd_cylinder(Vec3<f32>(0, 1.0F, 0), a, b, r)) <= kTol); // on the top cap rim/centre
    REQUIRE(sd_cylinder(Vec3<f32>(3, 0, 0), a, b, r) > 0.0F);
    check_lipschitz_3d([&](const Vec3<f32>& p) { return sd_cylinder(p, a, b, r); }, rng);
}

TEST_CASE("sd_cone: apex ~0, inside negative, far positive; Lipschitz", "[geometry][sdf]")
{
    Rng rng(8);
    const Vec2<f32> c(0.5F, kSqrt3 * 0.5F); // half-angle 30 deg
    const f32 h = 2.0F;
    REQUIRE(std::abs(sd_cone(Vec3<f32>(0, 0, 0), c, h)) <= kTol); // apex
    REQUIRE(sd_cone(Vec3<f32>(0, -1.0F, 0), c, h) < 0.0F);        // down the axis, inside
    REQUIRE(sd_cone(Vec3<f32>(5.0F, 0, 0), c, h) > 0.0F);
    check_lipschitz_3d([&](const Vec3<f32>& p) { return sd_cone(p, c, h); }, rng);
}

TEST_CASE("sd_torus: closed-form values on/around the tube; Lipschitz", "[geometry][sdf]")
{
    Rng rng(9);
    const Vec2<f32> t(1.5F, 0.4F);                                                 // major 1.5, minor 0.4
    REQUIRE(approx_equal_abs(sd_torus(Vec3<f32>(1.5F, 0, 0), t), -0.4F, kTol));    // on the major circle
    REQUIRE(approx_equal_abs(sd_torus(Vec3<f32>(1.9F, 0, 0), t), 0.0F, kTol));     // outer equator
    REQUIRE(approx_equal_abs(sd_torus(Vec3<f32>(1.5F, 0.4F, 0), t), 0.0F, kTol));  // top of the tube
    REQUIRE(approx_equal_abs(sd_torus(Vec3<f32>(0, 0, 0), t), 1.5F - 0.4F, kTol)); // hole centre
    check_lipschitz_3d([&](const Vec3<f32>& p) { return sd_torus(p, t); }, rng);
}

TEST_CASE("sd_triangle (3D, unsigned): equals distance(Triangle3)", "[geometry][sdf]")
{
    Rng rng(10);
    const Vec3<f32> a(0.0F, 0.0F, 0.0F);
    const Vec3<f32> b(1.2F, 0.0F, 0.0F);
    const Vec3<f32> c(0.3F, 0.9F, 0.0F);
    const Triangle3<f32> tri(a, b, c);
    for (int i = 0; i < 500; ++i)
    {
        const Vec3<f32> p = rng.p3(3.0F);
        REQUIRE(approx_equal_abs(sd_triangle(p, a, b, c), distance(tri, p), kTol));
    }
    REQUIRE(approx_equal_abs(sd_triangle(a, a, b, c), 0.0F, kTol)); // a vertex
}

TEST_CASE("sd_ellipsoid: ~0 on an axis vertex, negative inside, exact r.x at 2*r.x", "[geometry][sdf]")
{
    const Vec3<f32> r(1.0F, 1.6F, 0.7F);
    REQUIRE(std::abs(sd_ellipsoid(Vec3<f32>(1.0F, 0, 0), r)) <= kTol);             // +x vertex
    REQUIRE(std::abs(sd_ellipsoid(Vec3<f32>(0, 1.6F, 0), r)) <= kTol);             // +y vertex
    REQUIRE(sd_ellipsoid(Vec3<f32>(0, 0, 0), r) < 0.0F);                           // centre
    REQUIRE(approx_equal_abs(sd_ellipsoid(Vec3<f32>(2.0F, 0, 0), r), 1.0F, kTol)); // k0=2 => sd=r.x
}

TEST_CASE("sd_octahedron: closed-form at centre + vertex, far positive; Lipschitz", "[geometry][sdf]")
{
    Rng rng(11);
    const f32 s = 1.0F;
    REQUIRE(approx_equal_abs(sd_octahedron(Vec3<f32>(0, 0, 0), s), -s / kSqrt3, kTol));
    REQUIRE(approx_equal_abs(sd_octahedron(Vec3<f32>(s, 0, 0), s), 0.0F, kTol)); // a vertex
    REQUIRE(approx_equal_abs(sd_octahedron(Vec3<f32>(0, 0, s), s), 0.0F, kTol)); // a vertex
    REQUIRE(sd_octahedron(Vec3<f32>(3, 0, 0), s) > 0.0F);
    check_lipschitz_3d([&](const Vec3<f32>& p) { return sd_octahedron(p, s); }, rng);
}

// ---- 2D --------------------------------------------------------------------

TEST_CASE("sd_circle: matches distance(Circle) magnitude + sign matches contains", "[geometry][sdf]")
{
    Rng rng(20);
    const Circle<f32> ci(Vec2<f32>(0, 0), 1.1F);
    for (int i = 0; i < 400; ++i)
    {
        const Vec2<f32> p = rng.p2(3.0F);
        const f32 d = sd_circle(p, 1.1F);
        REQUIRE(approx_equal_abs(std::abs(d), distance(ci, p), kTol));
        REQUIRE((d <= 0.0F) == contains(ci, p));
    }
}

TEST_CASE("sd_box_2d: outside == distance(AABB2), sign == contains", "[geometry][sdf]")
{
    Rng rng(21);
    const Vec2<f32> b(0.8F, 0.5F);
    const AABB2<f32> box(-b, b);
    for (int i = 0; i < 400; ++i)
    {
        const Vec2<f32> p = rng.p2(3.0F);
        const f32 d = sd_box_2d(p, b);
        if (!contains(box, p))
        {
            REQUIRE(approx_equal_abs(d, distance(box, p), kTol));
        }
        else
        {
            REQUIRE(d <= kTol);
        }
    }
    REQUIRE(approx_equal_abs(sd_box_2d(Vec2<f32>(0, 0), b), -0.5F, kTol)); // -min(b)
}

TEST_CASE("sd_round_box_2d: value at origin + on a face; Lipschitz", "[geometry][sdf]")
{
    Rng rng(22);
    const Vec2<f32> b(1.0F, 0.7F);
    const f32 r = 0.15F;
    REQUIRE(approx_equal_abs(sd_round_box_2d(Vec2<f32>(0, 0), b, r), -0.7F, kTol));   // -min(b)
    REQUIRE(approx_equal_abs(sd_round_box_2d(Vec2<f32>(1.0F, 0), b, r), 0.0F, kTol)); // +x face
    check_lipschitz_2d([&](const Vec2<f32>& p) { return sd_round_box_2d(p, b, r); }, rng);
}

TEST_CASE("sd_segment_2d: equals distance(Segment2)", "[geometry][sdf]")
{
    Rng rng(23);
    const Vec2<f32> a(-0.4F, 0.1F);
    const Vec2<f32> b(0.9F, 0.6F);
    const Segment2<f32> seg(a, b);
    for (int i = 0; i < 400; ++i)
    {
        const Vec2<f32> p = rng.p2(3.0F);
        REQUIRE(approx_equal_abs(sd_segment_2d(p, a, b), distance(seg, p), kTol));
    }
}

TEST_CASE("sd_triangle_2d: signed -- negative inside, sign matches contains, ~0 at vertices", "[geometry][sdf]")
{
    Rng rng(24);
    const Vec2<f32> p0(-0.6F, -0.4F);
    const Vec2<f32> p1(0.8F, -0.5F);
    const Vec2<f32> p2(0.1F, 0.9F);
    const Triangle2<f32> tri(p0, p1, p2);
    for (int i = 0; i < 500; ++i)
    {
        const Vec2<f32> p = rng.p2(2.5F);
        const f32 d = sd_triangle_2d(p, p0, p1, p2);
        REQUIRE((d <= kTol) == (contains(tri, p) || std::abs(d) <= kTol));
        if (!contains(tri, p))
        {
            REQUIRE(approx_equal_abs(std::abs(d), distance(tri, p), kTol));
        }
    }
    REQUIRE(approx_equal_abs(sd_triangle_2d(p0, p0, p1, p2), 0.0F, kTol));
    REQUIRE(approx_equal_abs(sd_triangle_2d(p1, p0, p1, p2), 0.0F, kTol));
    check_lipschitz_2d([&](const Vec2<f32>& p) { return sd_triangle_2d(p, p0, p1, p2); }, rng);
}

TEST_CASE("sd_equilateral_triangle_2d / pentagon / hexagon: inside negative, far positive; Lipschitz",
          "[geometry][sdf]")
{
    Rng rng(25);
    REQUIRE(sd_equilateral_triangle_2d(Vec2<f32>(0, 0), 1.0F) < 0.0F);
    REQUIRE(sd_equilateral_triangle_2d(Vec2<f32>(4, 0), 1.0F) > 0.0F);
    REQUIRE(sd_pentagon_2d(Vec2<f32>(0, 0), 1.0F) < 0.0F);
    REQUIRE(sd_pentagon_2d(Vec2<f32>(4, 0), 1.0F) > 0.0F);
    REQUIRE(sd_hexagon_2d(Vec2<f32>(0, 0), 1.0F) < 0.0F);
    REQUIRE(sd_hexagon_2d(Vec2<f32>(0, 4), 1.0F) > 0.0F);
    check_lipschitz_2d([](const Vec2<f32>& p) { return sd_equilateral_triangle_2d(p, 1.0F); }, rng);
    check_lipschitz_2d([](const Vec2<f32>& p) { return sd_pentagon_2d(p, 1.0F); }, rng);
    check_lipschitz_2d([](const Vec2<f32>& p) { return sd_hexagon_2d(p, 1.0F); }, rng);
}
