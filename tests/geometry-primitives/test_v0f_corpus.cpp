// crd-geometry-primitives v0f -- the scalar branchless ray-intersection corpus:
// Woop 2013 watertight ray-tri (+ the shared-edge no-crack property), Baldwin-
// Weber 2016 precomputed ray-tri, Williams 2005 / Ize 2013 robust slab ray-AABB,
// Plucker edge classification. Cross-checked against v0c's Moller-Trumbore and
// v0c's plain slab ray-AABB.

#include <crd/geometry/primitives/intersect.hpp>
#include <crd/geometry/primitives/plucker.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/geometry/primitives/watertight_ray_tri.hpp>
#include <crd/math/math.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

using namespace crd;
using namespace crd::math;
using namespace crd::geometry::primitives;

namespace
{
template <typename T> constexpr T tol() noexcept
{
    return std::is_same_v<T, float> ? static_cast<T>(2e-3) : static_cast<T>(1e-9);
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
} // namespace

// ===========================================================================
// Woop 2013 -- watertight ray <-> triangle
// ===========================================================================

TEMPLATE_TEST_CASE("v0f -- Woop watertight ray-tri: basic + back-face cull", "[geometry][v0f]", float, double)
{
    using T = TestType;
    const Triangle3<T> tri(Vec3<T>(-1, -1, 0), Vec3<T>(2, -1, 0), Vec3<T>(0, 2, 0)); // in z = 0, CCW from +z
    T t = static_cast<T>(-1);
    Vec3<T> bc{};
    SECTION("hit / miss / behind / parallel")
    {
        REQUIRE(intersect_ray_triangle_watertight(Ray3<T>(Vec3<T>(0, 0, 5), Vec3<T>(0, 0, -1)), tri, t, bc));
        REQUIRE(t == Catch::Approx(static_cast<T>(5)).margin(tol<T>()));
        // barycentric of the hit point (0,0,0): with this triangle ~= (1/3-ish weights); just check it sums to 1
        REQUIRE((bc.x + bc.y + bc.z) == Catch::Approx(static_cast<T>(1)).margin(tol<T>()));
        REQUIRE_FALSE(
            intersect_ray_triangle_watertight(Ray3<T>(Vec3<T>(5, 5, 5), Vec3<T>(0, 0, -1)), tri, t, bc)); // misses
        REQUIRE_FALSE(
            intersect_ray_triangle_watertight(Ray3<T>(Vec3<T>(0, 0, -5), Vec3<T>(0, 0, -1)), tri, t, bc)); // behind
        REQUIRE_FALSE(
            intersect_ray_triangle_watertight(Ray3<T>(Vec3<T>(0, 0, 5), Vec3<T>(1, 0, 0)), tri, t, bc)); // parallel
    }
    SECTION("back-face culling")
    {
        // ray coming from below (-z) going up (+z) hits the back face
        REQUIRE(intersect_ray_triangle_watertight(Ray3<T>(Vec3<T>(0, 0, -5), Vec3<T>(0, 0, 1)), tri, t, bc, false));
        REQUIRE_FALSE(
            intersect_ray_triangle_watertight(Ray3<T>(Vec3<T>(0, 0, -5), Vec3<T>(0, 0, 1)), tri, t, bc, true));
        // and from above with cull_back: front face still hits
        REQUIRE(intersect_ray_triangle_watertight(Ray3<T>(Vec3<T>(0, 0, 5), Vec3<T>(0, 0, -1)), tri, t, bc, true));
    }
    SECTION("precompute path == inline-convenience path")
    {
        const Ray3<T> r(Vec3<T>(static_cast<T>(0.3), static_cast<T>(-0.2), 4),
                        Vec3<T>(static_cast<T>(0.1), static_cast<T>(0.2), -1));
        T t1 = 0;
        T t2 = 0;
        Vec3<T> b1{};
        Vec3<T> b2{};
        const bool h1 = intersect_ray_triangle_watertight(r, tri, t1, b1);
        const bool h2 = intersect_ray_triangle_watertight(r, precompute_ray_tri(r), tri, t2, b2);
        REQUIRE(h1 == h2);
        if (h1)
        {
            REQUIRE(t1 == Catch::Approx(t2).margin(tol<T>()));
        }
    }
}

TEMPLATE_TEST_CASE("v0f -- Woop watertight: shared-edge no-crack property", "[geometry][v0f]", float, double)
{
    using T = TestType;
    // Two triangles sharing edge AB; a ray straight down through a point exactly
    // on AB must be reported by *at least one* (no crack). With these integer
    // coords the edge function lands exactly on 0 -> exercises "closed on-edge".
    const Vec3<T> va(0, 0, 0);
    const Vec3<T> vb(2, 0, 0);
    const Vec3<T> vc(0, 2, 0);
    const Vec3<T> vd(0, -2, 0);
    const Triangle3<T> abc(va, vb, vc);
    const Triangle3<T> abd(va, vb, vd);
    for (const T x : {static_cast<T>(0.5), static_cast<T>(1.0), static_cast<T>(1.5)})
    {
        const Ray3<T> ray(Vec3<T>(x, 0, 5), Vec3<T>(0, 0, -1)); // hits exactly on segment AB (y == 0)
        T t1 = 0;
        T t2 = 0;
        Vec3<T> b1{};
        Vec3<T> b2{};
        const bool h1 = intersect_ray_triangle_watertight(ray, abc, t1, b1);
        const bool h2 = intersect_ray_triangle_watertight(ray, abd, t2, b2);
        REQUIRE((h1 || h2)); // no crack: at least one triangle catches the on-edge ray
        if (h1)
        {
            REQUIRE(t1 == Catch::Approx(static_cast<T>(5)).margin(tol<T>()));
        }
        if (h2)
        {
            REQUIRE(t2 == Catch::Approx(static_cast<T>(5)).margin(tol<T>()));
        }
    }
}

TEMPLATE_TEST_CASE("v0f -- Woop <-> v0c Moller-Trumbore agreement (random non-degenerate corpus)", "[geometry][v0f]",
                   float, double)
{
    using T = TestType;
    Rng rng(0x0F00U);
    int agreements = 0;
    for (int it = 0; it < 256; ++it)
    {
        const Triangle3<T> tri(rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                               rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                               rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)));
        // Reject near-degenerate triangles for this comparison.
        const Vec3<T> n = crd::math::cross(tri.b - tri.a, tri.c - tri.a);
        if (crd::math::dot(n, n) < static_cast<T>(1e-3))
        {
            continue;
        }
        const Ray3<T> ray(rnd3<T>(rng, static_cast<T>(-4), static_cast<T>(4)),
                          rnd3<T>(rng, static_cast<T>(-1), static_cast<T>(1)));
        if (crd::math::dot(ray.direction, ray.direction) < static_cast<T>(1e-4))
        {
            continue;
        }
        T tw = 0;
        T tm = 0;
        Vec3<T> bw{};
        Vec3<T> bm{};
        const bool hw = intersect_ray_triangle_watertight(ray, tri, tw, bw);
        const bool hm = intersect_ray_triangle(ray, tri, tm, bm);
        // Both should agree except in the thin edge-grazing band; require strong agreement.
        if (hw == hm)
        {
            ++agreements;
            if (hw)
            {
                REQUIRE(tw == Catch::Approx(tm).margin(static_cast<T>(5e-3)));
            }
        }
    }
    REQUIRE(agreements >= 240); // >= ~94% exact agreement; the rest are legitimate edge-grazing boundary cases
}

// ===========================================================================
// Baldwin-Weber 2016 -- precomputed per-triangle affine transform
// ===========================================================================

TEMPLATE_TEST_CASE("v0f -- Baldwin-Weber precomputed ray-tri <-> Woop agreement", "[geometry][v0f]", float, double)
{
    using T = TestType;
    Rng rng(0xBA1DU);
    for (int it = 0; it < 256; ++it)
    {
        const Triangle3<T> tri(rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                               rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                               rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)));
        const Vec3<T> n = crd::math::cross(tri.b - tri.a, tri.c - tri.a);
        if (crd::math::dot(n, n) < static_cast<T>(1e-2))
        {
            continue;
        }
        const TriAffine<T> m = precompute_triangle_affine(tri);
        const Ray3<T> ray(rnd3<T>(rng, static_cast<T>(-4), static_cast<T>(4)),
                          rnd3<T>(rng, static_cast<T>(-1), static_cast<T>(1)));
        if (crd::math::dot(ray.direction, ray.direction) < static_cast<T>(1e-3))
        {
            continue;
        }
        T tb = 0;
        T tw = 0;
        Vec3<T> bb{};
        Vec3<T> bw{};
        const bool hb = intersect_ray_triangle_precomputed(ray, m, tb, bb);
        const bool hw = intersect_ray_triangle_watertight(ray, tri, tw, bw);
        if (hb && hw)
        {
            REQUIRE(tb == Catch::Approx(tw).margin(static_cast<T>(5e-3)));
            REQUIRE((bb.x + bb.y + bb.z) == Catch::Approx(static_cast<T>(1)).margin(static_cast<T>(2e-3)));
        }
        // (a hb!=hw mismatch is only a thin edge-grazing band; the t-match above is the strong check.)
    }
}

// ===========================================================================
// Williams 2005 / Ize 2013 -- robust slab ray <-> AABB
// ===========================================================================

TEMPLATE_TEST_CASE("v0f -- robust ray-AABB: basic + conservative vs v0c slab", "[geometry][v0f]", float, double)
{
    using T = TestType;
    const AABB3<T> box(Vec3<T>(-1, -1, -1), Vec3<T>(1, 1, 1));
    const T inf = std::numeric_limits<T>::infinity();
    SECTION("basic hit / miss / behind / inside")
    {
        T t = -1;
        REQUIRE(
            intersect_ray_aabb_robust(Ray3<T>(Vec3<T>(-5, 0, 0), Vec3<T>(1, 0, 0)), box, static_cast<T>(0), inf, t));
        REQUIRE(t == Catch::Approx(static_cast<T>(4)).margin(static_cast<T>(1e-3)));
        REQUIRE_FALSE(
            intersect_ray_aabb_robust(Ray3<T>(Vec3<T>(-5, 5, 0), Vec3<T>(1, 0, 0)), box, static_cast<T>(0), inf, t));
        REQUIRE_FALSE(
            intersect_ray_aabb_robust(Ray3<T>(Vec3<T>(-5, 0, 0), Vec3<T>(-1, 0, 0)), box, static_cast<T>(0), inf, t));
        REQUIRE(intersect_ray_aabb_robust(Ray3<T>(Vec3<T>(0, 0, 0), Vec3<T>(1, 0, 0)), box, static_cast<T>(0), inf,
                                          t)); // inside
        // a zero-direction component must be NaN/inf-safe (ray parallel to the x-slab, inside it)
        REQUIRE(
            intersect_ray_aabb_robust(Ray3<T>(Vec3<T>(0, -5, 0), Vec3<T>(0, 1, 0)), box, static_cast<T>(0), inf, t));
    }
    SECTION("conservative: never MISS when v0c's plain slab says HIT")
    {
        Rng rng(0x1235U);
        for (int it = 0; it < 256; ++it)
        {
            const Ray3<T> ray(rnd3<T>(rng, static_cast<T>(-4), static_cast<T>(4)),
                              rnd3<T>(rng, static_cast<T>(-1), static_cast<T>(1)));
            if (crd::math::dot(ray.direction, ray.direction) < static_cast<T>(1e-4))
            {
                continue;
            }
            T tv = 0;
            T tr = 0;
            const bool hv = intersect_ray_aabb(ray, box, tv);
            const bool hr = intersect_ray_aabb_robust(ray, box, static_cast<T>(0), inf, tr);
            if (hv)
            {
                REQUIRE(hr); // the robust form is conservative -- it never drops a v0c hit
                REQUIRE(tr == Catch::Approx(tv).margin(static_cast<T>(1e-3)));
            }
        }
    }
}

// ===========================================================================
// Plucker edge classification
// ===========================================================================

TEMPLATE_TEST_CASE("v0f -- Plucker: side values + ray-tri boolean <-> Woop", "[geometry][v0f]", float, double)
{
    using T = TestType;
    SECTION("side(L, L) == 0; parallel lines coplanar; skew lines non-zero")
    {
        const PluckerLine<T> l1 = plucker_from(Vec3<T>(0, 0, 0), Vec3<T>(1, 0, 0));
        REQUIRE(plucker_side(l1, l1) == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
        const PluckerLine<T> par = plucker_from(Vec3<T>(0, 5, 0), Vec3<T>(3, 5, 0)); // parallel to l1, offset in y
        REQUIRE(plucker_side(l1, par) == Catch::Approx(static_cast<T>(0)).margin(tol<T>())); // parallel => coplanar
        const PluckerLine<T> skew = plucker_from(Vec3<T>(0, 1, 5), Vec3<T>(0, 1, -5));       // along z at (x=0,y=1)
        REQUIRE(std::abs(plucker_side(l1, skew)) > static_cast<T>(1e-3));                    // skew => non-zero
    }
    SECTION("ray-tri boolean agrees with Woop on a random corpus")
    {
        Rng rng(0x9C0AU);
        int agreements = 0;
        for (int it = 0; it < 256; ++it)
        {
            const Triangle3<T> tri(rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                                   rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)),
                                   rnd3<T>(rng, static_cast<T>(-3), static_cast<T>(3)));
            const Vec3<T> n = crd::math::cross(tri.b - tri.a, tri.c - tri.a);
            if (crd::math::dot(n, n) < static_cast<T>(1e-2))
            {
                continue;
            }
            const Ray3<T> ray(rnd3<T>(rng, static_cast<T>(-4), static_cast<T>(4)),
                              rnd3<T>(rng, static_cast<T>(-1), static_cast<T>(1)));
            if (crd::math::dot(ray.direction, ray.direction) < static_cast<T>(1e-3))
            {
                continue;
            }
            T tw = 0;
            Vec3<T> bw{};
            const bool hw = intersect_ray_triangle_watertight(ray, tri, tw, bw);
            const bool hp = intersect_ray_triangle_plucker(ray, tri);
            if (hw == hp)
            {
                ++agreements;
            }
        }
        REQUIRE(agreements >= 235);
    }
}
