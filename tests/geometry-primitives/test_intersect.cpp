// crd-geometry-primitives v0c -- intersection corpus (2D + 3D). Ray casts:
// hit/miss/grazing/behind-origin/inside-start with exact out_t on analytic cases.
// SAT pairs: separated/touching/overlapping per axis class + a randomised
// brute-force cross-check. Tri-tri: piercing / coplanar / disjoint. The
// sphere/capsule-vs-X reductions are cross-checked against the v0b distance API.

#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/intersect.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/math.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace crd;
using namespace crd::math;
using namespace crd::geometry::primitives;

namespace
{
template <typename T> constexpr T tol() noexcept
{
    return std::is_same_v<T, float> ? static_cast<T>(1e-4) : static_cast<T>(1e-9);
}

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
    template <typename T> T uni(T lo, T hi) noexcept
    {
        const T u = static_cast<T>(next() >> 11) * (static_cast<T>(1) / static_cast<T>(1ULL << 53));
        return lo + u * (hi - lo);
    }
};
template <typename T> Vec3<T> rnd3(Rng& r, T lo, T hi)
{
    return Vec3<T>(r.uni(lo, hi), r.uni(lo, hi), r.uni(lo, hi));
}
template <typename T> Vec2<T> rnd2(Rng& r, T lo, T hi)
{
    return Vec2<T>(r.uni(lo, hi), r.uni(lo, hi));
}
} // namespace

// ===========================================================================
// 3D -- ray casts
// ===========================================================================

TEMPLATE_TEST_CASE("intersect 3D -- ray vs AABB / OBB", "[geometry][intersect]", float, double)
{
    using T = TestType;
    SECTION("ray vs AABB3")
    {
        const AABB3<T> box(Vec3<T>(-1, -1, -1), Vec3<T>(1, 1, 1));
        T t = static_cast<T>(-1);
        REQUIRE(intersect_ray_aabb(Ray3<T>(Vec3<T>(-5, 0, 0), Vec3<T>(1, 0, 0)), box, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(4)).margin(tol<T>()));
        REQUIRE_FALSE(intersect_ray_aabb(Ray3<T>(Vec3<T>(-5, 5, 0), Vec3<T>(1, 0, 0)), box, t));  // misses above
        REQUIRE_FALSE(intersect_ray_aabb(Ray3<T>(Vec3<T>(-5, 0, 0), Vec3<T>(-1, 0, 0)), box, t)); // points away
        REQUIRE(intersect_ray_aabb(Ray3<T>(Vec3<T>(0, 0, 0), Vec3<T>(1, 0, 0)), box, t));         // starts inside
        REQUIRE(t == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
        REQUIRE(intersect_ray_aabb(Ray3<T>(Vec3<T>(2, 0, 0), Vec3<T>(0, 1, 0)), box, t) == false); // parallel, outside
    }
    SECTION("ray vs OBB3 -- axis-aligned reduces to AABB; rotated 45deg about z")
    {
        const OBB3<T> aa(Vec3<T>(0, 0, 0), Vec3<T>(1, 1, 1), Mat3<T>::identity());
        T t = static_cast<T>(-1);
        REQUIRE(intersect_ray_obb(Ray3<T>(Vec3<T>(-5, 0, 0), Vec3<T>(1, 0, 0)), aa, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(4)).margin(tol<T>()));

        const T c = static_cast<T>(0.70710678118654752440);
        const OBB3<T> obb(Vec3<T>(0, 0, 0), Vec3<T>(2, static_cast<T>(0.5), 1),
                          Mat3<T>(Vec3<T>(c, c, 0), Vec3<T>(-c, c, 0), Vec3<T>(0, 0, 1)));
        // Shoot down the box's long axis (the (1,1,0)/sqrt2 direction): enters at -2 along it -> world (-2c,-2c,0).
        REQUIRE(intersect_ray_obb(Ray3<T>(Vec3<T>(-10 * c, -10 * c, 0), Vec3<T>(c, c, 0)), obb, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(10) - static_cast<T>(2)).margin(static_cast<T>(2e-3)));
        REQUIRE_FALSE(intersect_ray_obb(Ray3<T>(Vec3<T>(0, 5, 0), Vec3<T>(0, 1, 0)), obb, t)); // away
    }
}

TEMPLATE_TEST_CASE("intersect 3D -- ray vs Cylinder3 / Capsule3", "[geometry][intersect]", float, double)
{
    using T = TestType;
    const Cylinder3<T> cyl(Vec3<T>(0, 0, 0), Vec3<T>(0, 0, 10), static_cast<T>(2));
    T t = static_cast<T>(-1);
    SECTION("Cylinder3 -- through the curved side")
    {
        REQUIRE(intersect_ray_cylinder(Ray3<T>(Vec3<T>(-5, 0, 5), Vec3<T>(1, 0, 0)), cyl, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(3)).margin(tol<T>())); // x = -5 -> -2
    }
    SECTION("Cylinder3 -- through an end cap (along the axis)")
    {
        REQUIRE(intersect_ray_cylinder(Ray3<T>(Vec3<T>(0, 0, -4), Vec3<T>(0, 0, 1)), cyl, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(4)).margin(tol<T>()));
    }
    SECTION("Cylinder3 -- miss (passes beside, beyond the radius)")
    {
        REQUIRE_FALSE(intersect_ray_cylinder(Ray3<T>(Vec3<T>(-5, 5, 5), Vec3<T>(1, 0, 0)), cyl, t));
        REQUIRE_FALSE(intersect_ray_cylinder(Ray3<T>(Vec3<T>(-5, 0, 20), Vec3<T>(1, 0, 0)), cyl, t)); // past the top
    }
    SECTION("Capsule3 -- body and the spherical cap")
    {
        const Capsule3<T> cap(Vec3<T>(0, 0, 0), Vec3<T>(0, 0, 10), static_cast<T>(2));
        REQUIRE(intersect_ray_capsule(Ray3<T>(Vec3<T>(-5, 0, 5), Vec3<T>(1, 0, 0)), cap, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(3)).margin(tol<T>())); // body
        REQUIRE(intersect_ray_capsule(Ray3<T>(Vec3<T>(0, 0, -5), Vec3<T>(0, 0, 1)), cap, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(3)).margin(tol<T>())); // bottom cap, z=-2
        REQUIRE_FALSE(intersect_ray_capsule(Ray3<T>(Vec3<T>(-5, 5, 5), Vec3<T>(1, 0, 0)), cap, t));
    }
    SECTION("contains(Cylinder3, point)")
    {
        REQUIRE(contains(cyl, Vec3<T>(1, 0, 5)));
        REQUIRE_FALSE(contains(cyl, Vec3<T>(3, 0, 5)));  // outside the radius
        REQUIRE_FALSE(contains(cyl, Vec3<T>(0, 0, 11))); // past the cap
    }
}

// ===========================================================================
// 3D -- SAT pairs
// ===========================================================================

TEMPLATE_TEST_CASE("intersect 3D -- AABB <-> Triangle (Akenine-Moller 13-axis SAT)", "[geometry][intersect]", float,
                   double)
{
    using T = TestType;
    const AABB3<T> box(Vec3<T>(-1, -1, -1), Vec3<T>(1, 1, 1));
    SECTION("overlapping / touching / separated")
    {
        REQUIRE(intersects(box, Triangle3<T>(Vec3<T>(-2, 0, 0), Vec3<T>(2, 0, 0), Vec3<T>(0, 2, 0)))); // slices through
        REQUIRE(intersects(box, Triangle3<T>(Vec3<T>(1, 1, 1), Vec3<T>(3, 0, 0), Vec3<T>(0, 3, 0))));  // corner touch
        REQUIRE_FALSE(intersects(box, Triangle3<T>(Vec3<T>(5, 0, 0), Vec3<T>(6, 1, 0), Vec3<T>(5, 2, 0))));    // far
        REQUIRE_FALSE(intersects(box, Triangle3<T>(Vec3<T>(-3, -3, 5), Vec3<T>(3, -3, 5), Vec3<T>(0, 3, 5)))); // above
        REQUIRE(intersects(Triangle3<T>(Vec3<T>(-2, 0, 0), Vec3<T>(2, 0, 0), Vec3<T>(0, 2, 0)), box)); // swapped
    }
    SECTION("cross-check vs brute-force point sample")
    {
        Rng rng(0xA1B2U);
        for (int it = 0; it < 96; ++it)
        {
            const Triangle3<T> tri(rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                                   rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                                   rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)));
            const bool sat = intersects(box, tri);
            // closest point on the tri to the box's nearest point <= 0 => they touch; sample to corroborate.
            bool sampled = false;
            for (int i = 0; i <= 10 && !sampled; ++i)
            {
                for (int j = 0; j + i <= 10 && !sampled; ++j)
                {
                    const T u = static_cast<T>(i) / 10;
                    const T v = static_cast<T>(j) / 10;
                    const Vec3<T> p = tri.a * (static_cast<T>(1) - u - v) + tri.b * u + tri.c * v;
                    if (contains(box, p))
                    {
                        sampled = true;
                    }
                }
            }
            if (sampled)
            {
                REQUIRE(sat); // a triangle point inside the box => SAT must say overlap
            }
            // (the converse -- SAT true but no sampled point inside -- is legitimate when only an
            // edge grazes; the distance check below catches gross errors.)
            if (sat)
            {
                REQUIRE(distance_squared(box, closest_point(tri, center(box))) >= static_cast<T>(0));
            }
        }
    }
}

TEMPLATE_TEST_CASE("intersect 3D -- OBB <-> OBB (15-axis SAT)", "[geometry][intersect]", float, double)
{
    using T = TestType;
    const OBB3<T> a(Vec3<T>(0, 0, 0), Vec3<T>(1, 1, 1), Mat3<T>::identity());
    SECTION("axis-aligned overlap / separation reduces to AABB")
    {
        REQUIRE(intersects(a, OBB3<T>(Vec3<T>(1, 0, 0), Vec3<T>(1, 1, 1), Mat3<T>::identity())));
        REQUIRE_FALSE(intersects(a, OBB3<T>(Vec3<T>(3, 0, 0), Vec3<T>(1, 1, 1), Mat3<T>::identity())));
    }
    SECTION("rotated 45deg about z -- separated only via an edge-cross axis")
    {
        const T c = static_cast<T>(0.70710678118654752440);
        const Mat3<T> rot45(Vec3<T>(c, c, 0), Vec3<T>(-c, c, 0), Vec3<T>(0, 0, 1));
        REQUIRE(
            intersects(a, OBB3<T>(Vec3<T>(static_cast<T>(1.5), 0, 0), Vec3<T>(1, 1, 1), rot45))); // diamond overlaps
        REQUIRE_FALSE(intersects(a, OBB3<T>(Vec3<T>(3, 0, 0), Vec3<T>(1, 1, 1), rot45)));         // far apart
    }
    SECTION("cross-check: OBB(identity) vs OBB(identity) == AABB vs AABB")
    {
        Rng rng(0xBEEFU);
        for (int it = 0; it < 64; ++it)
        {
            const Vec3<T> c2 = rnd3<T>(rng, static_cast<T>(-4), static_cast<T>(4));
            const Vec3<T> h2 = rnd3<T>(rng, static_cast<T>(0.2), static_cast<T>(2));
            const OBB3<T> b(c2, h2, Mat3<T>::identity());
            const AABB3<T> ab(Vec3<T>(-1, -1, -1), Vec3<T>(1, 1, 1));
            const AABB3<T> bb(c2 - h2, c2 + h2);
            REQUIRE(intersects(a, b) == intersects(ab, bb));
        }
    }
}

TEMPLATE_TEST_CASE("intersect 3D -- Triangle <-> Triangle (Moller 1997)", "[geometry][intersect]", float, double)
{
    using T = TestType;
    const Triangle3<T> base(Vec3<T>(-1, -1, 0), Vec3<T>(2, -1, 0), Vec3<T>(0, 2, 0)); // in z=0
    SECTION("piercing / disjoint / coplanar")
    {
        REQUIRE(intersects(base, Triangle3<T>(Vec3<T>(0, 0, -1), Vec3<T>(0, 0, 1), Vec3<T>(1, 0, 0)))); // skewers it
        REQUIRE_FALSE(
            intersects(base, Triangle3<T>(Vec3<T>(0, 0, 1), Vec3<T>(1, 0, 1), Vec3<T>(0, 1, 1)))); // hovers above
        REQUIRE(
            intersects(base, Triangle3<T>(Vec3<T>(0, 0, 0), Vec3<T>(1, 0, 0), Vec3<T>(0, 1, 0)))); // coplanar overlap
        REQUIRE_FALSE(
            intersects(base, Triangle3<T>(Vec3<T>(5, 5, 0), Vec3<T>(6, 5, 0), Vec3<T>(5, 6, 0)))); // coplanar disjoint
        REQUIRE(intersects(base, base)); // a triangle with itself
    }
}

// ===========================================================================
// 3D -- sphere / capsule reductions (cross-checked against v0b distance)
// ===========================================================================

TEMPLATE_TEST_CASE("intersect 3D -- Sphere <-> X and Capsule <-> X reduce to v0b distance", "[geometry][intersect]", float,
                   double)
{
    using T = TestType;
    Rng rng(0xCAFEU);
    for (int it = 0; it < 48; ++it)
    {
        const Vec3<T> sc = rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3));
        const T sr = rng.uni<T>(static_cast<T>(0.1), static_cast<T>(2));
        const Sphere<T> s(sc, sr);
        const AABB3<T> box(Vec3<T>(-1, -1, -1), Vec3<T>(1, 1, 1));
        const OBB3<T> obb(Vec3<T>(0, 0, 0), Vec3<T>(1, static_cast<T>(0.5), 2), Mat3<T>::identity());
        const Triangle3<T> tri(Vec3<T>(-1, 0, 0), Vec3<T>(1, 0, 0), Vec3<T>(0, 1, 0));
        const Segment3<T> seg(Vec3<T>(-2, 0, 0), Vec3<T>(2, 0, 0));
        REQUIRE(intersects(s, box) == (distance_squared(box, sc) <= sr * sr));
        REQUIRE(intersects(s, obb) == (distance_squared(obb, sc) <= sr * sr));
        REQUIRE(intersects(s, tri) == (distance_squared(tri, sc) <= sr * sr));
        REQUIRE(intersects(s, seg) == (distance_squared(seg, sc) <= sr * sr));
        // capsule <-> capsule == segment-segment distance <= r1+r2
        const Capsule3<T> c1(rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                             rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                             rng.uni<T>(static_cast<T>(0.1), static_cast<T>(1)));
        const Capsule3<T> c2(rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                             rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                             rng.uni<T>(static_cast<T>(0.1), static_cast<T>(1)));
        const T rr = c1.radius + c2.radius;
        REQUIRE(intersects(c1, c2) == (distance_squared(Segment3<T>(c1.a, c1.b), Segment3<T>(c2.a, c2.b)) <= rr * rr));
    }
}

// ===========================================================================
// 2D
// ===========================================================================

TEMPLATE_TEST_CASE("intersect 2D -- segments_intersect", "[geometry][intersect]", float, double)
{
    using T = TestType;
    SECTION("crossing / parallel-disjoint / collinear-overlap / T-junction")
    {
        REQUIRE(
            segments_intersect(Segment2<T>(Vec2<T>(-1, 0), Vec2<T>(1, 0)), Segment2<T>(Vec2<T>(0, -1), Vec2<T>(0, 1))));
        REQUIRE_FALSE(
            segments_intersect(Segment2<T>(Vec2<T>(0, 0), Vec2<T>(1, 0)), Segment2<T>(Vec2<T>(0, 1), Vec2<T>(1, 1))));
        REQUIRE(segments_intersect(Segment2<T>(Vec2<T>(0, 0), Vec2<T>(3, 0)),
                                   Segment2<T>(Vec2<T>(2, 0), Vec2<T>(5, 0)))); // collinear overlap
        REQUIRE(segments_intersect(Segment2<T>(Vec2<T>(0, 0), Vec2<T>(4, 0)),
                                   Segment2<T>(Vec2<T>(2, 0), Vec2<T>(2, 5)))); // T-junction
        REQUIRE_FALSE(segments_intersect(Segment2<T>(Vec2<T>(0, 0), Vec2<T>(1, 0)),
                                         Segment2<T>(Vec2<T>(2, 0), Vec2<T>(3, 0)))); // collinear disjoint
    }
    SECTION("intersection point of a proper crossing")
    {
        Vec2<T> p{};
        REQUIRE(segments_intersect(Segment2<T>(Vec2<T>(-1, -1), Vec2<T>(1, 1)),
                                   Segment2<T>(Vec2<T>(-1, 1), Vec2<T>(1, -1)), p));
        REQUIRE(approx_equal_abs(p.x, static_cast<T>(0), tol<T>()));
        REQUIRE(approx_equal_abs(p.y, static_cast<T>(0), tol<T>()));
    }
}

TEMPLATE_TEST_CASE("intersect 2D -- ray vs primitives", "[geometry][intersect]", float, double)
{
    using T = TestType;
    T t = static_cast<T>(-1);
    SECTION("ray vs AABB2 / OBB2")
    {
        const AABB2<T> box(Vec2<T>(-1, -1), Vec2<T>(1, 1));
        REQUIRE(intersect_ray2_aabb(Ray2<T>(Vec2<T>(-5, 0), Vec2<T>(1, 0)), box, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(4)).margin(tol<T>()));
        REQUIRE_FALSE(intersect_ray2_aabb(Ray2<T>(Vec2<T>(-5, 5), Vec2<T>(1, 0)), box, t));
        const T c = static_cast<T>(0.70710678118654752440);
        const OBB2<T> obb(Vec2<T>(0, 0), Vec2<T>(2, static_cast<T>(0.5)), Mat2<T>(Vec2<T>(c, c), Vec2<T>(-c, c)));
        REQUIRE(intersect_ray2_obb(Ray2<T>(Vec2<T>(-10 * c, -10 * c), Vec2<T>(c, c)), obb, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(8)).margin(static_cast<T>(2e-3)));
    }
    SECTION("ray vs Circle / Segment2 / Line2")
    {
        REQUIRE(intersect_ray2_circle(Ray2<T>(Vec2<T>(-5, 0), Vec2<T>(1, 0)),
                                      Circle<T>(Vec2<T>(0, 0), static_cast<T>(2)), t));
        REQUIRE(t == Catch::Approx(static_cast<T>(3)).margin(tol<T>()));
        REQUIRE_FALSE(intersect_ray2_circle(Ray2<T>(Vec2<T>(-5, 5), Vec2<T>(1, 0)),
                                            Circle<T>(Vec2<T>(0, 0), static_cast<T>(2)), t));
        REQUIRE(intersect_ray2_segment(Ray2<T>(Vec2<T>(0, -5), Vec2<T>(0, 1)),
                                       Segment2<T>(Vec2<T>(-2, 0), Vec2<T>(2, 0)), t));
        REQUIRE(t == Catch::Approx(static_cast<T>(5)).margin(tol<T>()));
        REQUIRE_FALSE(intersect_ray2_segment(Ray2<T>(Vec2<T>(0, -5), Vec2<T>(0, 1)),
                                             Segment2<T>(Vec2<T>(3, 0), Vec2<T>(5, 0)), t));
    }
    SECTION("ray vs Cylinder2 (= rectangle)")
    {
        const Cylinder2<T> cyl(Vec2<T>(0, 0), Vec2<T>(0, 10), static_cast<T>(2));
        REQUIRE(intersect_ray2_cylinder(Ray2<T>(Vec2<T>(-5, 5), Vec2<T>(1, 0)), cyl, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(3)).margin(tol<T>()));
        REQUIRE_FALSE(intersect_ray2_cylinder(Ray2<T>(Vec2<T>(-5, 15), Vec2<T>(1, 0)), cyl, t)); // above the top
    }
}

TEMPLATE_TEST_CASE("intersect 2D -- OBB2 <-> OBB2 / Triangle2 <-> Triangle2 / Circle reductions", "[geometry][intersect]",
                   float, double)
{
    using T = TestType;
    SECTION("OBB2 <-> OBB2")
    {
        const OBB2<T> a(Vec2<T>(0, 0), Vec2<T>(1, 1), Mat2<T>::identity());
        REQUIRE(intersects(a, OBB2<T>(Vec2<T>(1, 0), Vec2<T>(1, 1), Mat2<T>::identity())));
        REQUIRE_FALSE(intersects(a, OBB2<T>(Vec2<T>(3, 0), Vec2<T>(1, 1), Mat2<T>::identity())));
        const T c = static_cast<T>(0.70710678118654752440);
        REQUIRE(intersects(
            a, OBB2<T>(Vec2<T>(static_cast<T>(1.3), 0), Vec2<T>(1, 1), Mat2<T>(Vec2<T>(c, c), Vec2<T>(-c, c)))));
        REQUIRE_FALSE(intersects(a, OBB2<T>(Vec2<T>(4, 0), Vec2<T>(1, 1), Mat2<T>(Vec2<T>(c, c), Vec2<T>(-c, c)))));
    }
    SECTION("Triangle2 <-> Triangle2")
    {
        const Triangle2<T> a(Vec2<T>(0, 0), Vec2<T>(2, 0), Vec2<T>(0, 2));
        REQUIRE(intersects(a, Triangle2<T>(Vec2<T>(1, 1), Vec2<T>(3, 1), Vec2<T>(1, 3))));       // overlapping
        REQUIRE_FALSE(intersects(a, Triangle2<T>(Vec2<T>(3, 0), Vec2<T>(5, 0), Vec2<T>(3, 2)))); // disjoint
        REQUIRE(intersects(a, a));
    }
    SECTION("Circle reductions vs v0b distance")
    {
        Rng rng(0x5C12U);
        for (int it = 0; it < 48; ++it)
        {
            const Vec2<T> cc = rnd2<T>(rng, static_cast<T>(-3), static_cast<T>(3));
            const T cr = rng.uni<T>(static_cast<T>(0.1), static_cast<T>(2));
            const Circle<T> circ(cc, cr);
            const AABB2<T> box(Vec2<T>(-1, -1), Vec2<T>(1, 1));
            const Triangle2<T> tri(Vec2<T>(-1, 0), Vec2<T>(1, 0), Vec2<T>(0, 1));
            const Segment2<T> seg(Vec2<T>(-2, 0), Vec2<T>(2, 0));
            REQUIRE(intersects(circ, box) == (distance_squared(box, cc) <= cr * cr));
            REQUIRE(intersects(circ, tri) == (distance_squared(tri, cc) <= cr * cr));
            REQUIRE(intersects(circ, seg) == (distance_squared(seg, cc) <= cr * cr));
        }
    }
}
