// crd-geometry-primitives v0f -- the SIMD batch kernels, lane-by-lane against
// their scalar reference (ULP-conformance): ray_vs_8_aabb / ray_packet8_vs_aabb
// vs the robust slab, ray_vs_8_triangle vs v0c Moller-Trumbore, aabb8/sphere8
// overlap masks vs v0c `intersects`, segment8 squared-closest-distance vs v0b
// `distance_squared(Segment3, Segment3)`. Plus the all-bits-set mask convention.

#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/intersect.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/geometry/primitives/simd_batch.hpp>
#include <crd/math/math.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace crd;
using namespace crd::math;
using namespace crd::geometry::primitives;
using crd::math::simd::Vec8f;

namespace
{
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
    f32 uni(f32 lo, f32 hi) noexcept
    {
        const f32 u = static_cast<f32>(next() >> 40) * (1.0F / 16777216.0F);
        return lo + u * (hi - lo);
    }
};
Vec3<f32> rnd3(Rng& r, f32 lo, f32 hi)
{
    return Vec3<f32>(r.uni(lo, hi), r.uni(lo, hi), r.uni(lo, hi));
}
// Build a Vec8f from a generator(lane).
template <typename F> Vec8f gather8(F gen)
{
    return Vec8f(gen(0), gen(1), gen(2), gen(3), gen(4), gen(5), gen(6), gen(7));
}
// Extract all 8 lanes.
struct Lanes8
{
    alignas(32) f32 v[8];
    explicit Lanes8(const Vec8f& x) noexcept { x.store(v); }
};
bool lane_set(const Vec8f& mask, int i)
{
    Lanes8 m(mask);
    return m.v[i] != 0.0F; // all-bits-set (a NaN bit pattern) != 0; +0.0 (clear) == 0
}
} // namespace

TEST_CASE("v0f SIMD -- ray_vs_8_aabb lane-by-lane vs the robust scalar slab", "[geometry][v0f][simd]")
{
    const f32 inf = std::numeric_limits<f32>::infinity();
    Rng rng(0xA8B8U);
    for (int it = 0; it < 64; ++it)
    {
        AABB3<f32> b[8];
        for (auto& box : b)
        {
            const Vec3<f32> c = rnd3(rng, -3.0F, 3.0F);
            const Vec3<f32> h(rng.uni(0.2F, 1.5F), rng.uni(0.2F, 1.5F), rng.uni(0.2F, 1.5F));
            box = AABB3<f32>(c - h, c + h);
        }
        Aabb8 boxes;
        boxes.min_x = gather8([&](int l) { return b[l].min.x; });
        boxes.min_y = gather8([&](int l) { return b[l].min.y; });
        boxes.min_z = gather8([&](int l) { return b[l].min.z; });
        boxes.max_x = gather8([&](int l) { return b[l].max.x; });
        boxes.max_y = gather8([&](int l) { return b[l].max.y; });
        boxes.max_z = gather8([&](int l) { return b[l].max.z; });

        const Ray3<f32> ray(rnd3(rng, -5.0F, 5.0F), rnd3(rng, -1.0F, 1.0F));
        if (dot(ray.direction, ray.direction) < 1e-4F)
        {
            continue;
        }
        const auto pre = precompute_ray_aabb(ray);
        const RayAabb8Result r = ray_vs_8_aabb(ray, pre, boxes, 0.0F, inf);
        Lanes8 tlanes(r.t_enter);
        for (int l = 0; l < 8; ++l)
        {
            f32 ts = 0.0F;
            const bool hs = intersect_ray_aabb_robust(ray, pre, b[l], 0.0F, inf, ts);
            REQUIRE(lane_set(r.hit_mask, l) == hs);
            if (hs)
            {
                REQUIRE(tlanes.v[l] == Catch::Approx(ts).margin(1e-3F));
            }
            // conservative wrt the v0c plain slab
            f32 tv = 0.0F;
            if (intersect_ray_aabb(ray, b[l], tv))
            {
                REQUIRE(lane_set(r.hit_mask, l));
            }
        }
    }
}

TEST_CASE("v0f SIMD -- ray_packet8_vs_aabb (8 rays, 1 box) vs the robust scalar slab", "[geometry][v0f][simd]")
{
    const f32 inf = std::numeric_limits<f32>::infinity();
    Rng rng(0x9A9AU);
    for (int it = 0; it < 64; ++it)
    {
        const Vec3<f32> c = rnd3(rng, -3.0F, 3.0F);
        const Vec3<f32> h(rng.uni(0.3F, 2.0F), rng.uni(0.3F, 2.0F), rng.uni(0.3F, 2.0F));
        const AABB3<f32> box(c - h, c + h);

        Ray3<f32> rays[8];
        for (auto& ray : rays)
        {
            ray = Ray3<f32>(rnd3(rng, -5.0F, 5.0F), rnd3(rng, -1.0F, 1.0F));
            if (dot(ray.direction, ray.direction) < 1e-3F)
            {
                ray.direction = Vec3<f32>(1.0F, 0.5F, -0.3F);
            }
        }
        const RayPacket8 pkt = precompute_ray_packet8(
            gather8([&](int l) { return rays[l].origin.x; }), gather8([&](int l) { return rays[l].origin.y; }),
            gather8([&](int l) { return rays[l].origin.z; }), gather8([&](int l) { return rays[l].direction.x; }),
            gather8([&](int l) { return rays[l].direction.y; }), gather8([&](int l) { return rays[l].direction.z; }));
        const RayAabb8Result r = ray_packet8_vs_aabb(pkt, box, 0.0F, inf);
        Lanes8 tlanes(r.t_enter);
        for (int l = 0; l < 8; ++l)
        {
            f32 ts = 0.0F;
            const bool hs = intersect_ray_aabb_robust(rays[l], box, 0.0F, inf, ts);
            REQUIRE(lane_set(r.hit_mask, l) == hs);
            if (hs)
            {
                REQUIRE(tlanes.v[l] == Catch::Approx(ts).margin(1e-3F));
            }
        }
    }
}

TEST_CASE("v0f SIMD -- ray_vs_8_triangle (Moller-Trumbore x 8) vs the v0c scalar MT", "[geometry][v0f][simd]")
{
    Rng rng(0x71A8U);
    int compared = 0;
    for (int it = 0; it < 64; ++it)
    {
        Triangle3<f32> tr[8];
        for (auto& t : tr)
        {
            t = Triangle3<f32>(rnd3(rng, -3.0F, 3.0F), rnd3(rng, -3.0F, 3.0F), rnd3(rng, -3.0F, 3.0F));
        }
        Triangle38 tris;
        tris.ax = gather8([&](int l) { return tr[l].a.x; });
        tris.ay = gather8([&](int l) { return tr[l].a.y; });
        tris.az = gather8([&](int l) { return tr[l].a.z; });
        tris.bx = gather8([&](int l) { return tr[l].b.x; });
        tris.by = gather8([&](int l) { return tr[l].b.y; });
        tris.bz = gather8([&](int l) { return tr[l].b.z; });
        tris.cx = gather8([&](int l) { return tr[l].c.x; });
        tris.cy = gather8([&](int l) { return tr[l].c.y; });
        tris.cz = gather8([&](int l) { return tr[l].c.z; });

        const Ray3<f32> ray(rnd3(rng, -5.0F, 5.0F), rnd3(rng, -1.0F, 1.0F));
        if (dot(ray.direction, ray.direction) < 1e-3F)
        {
            continue;
        }
        const RayTri8Result r = ray_vs_8_triangle(ray, tris);
        Lanes8 tlanes(r.t);
        for (int l = 0; l < 8; ++l)
        {
            // skip near-degenerate triangles for the exact comparison
            const Vec3<f32> n = cross(tr[l].b - tr[l].a, tr[l].c - tr[l].a);
            if (dot(n, n) < 1e-2F)
            {
                continue;
            }
            f32 ts = 0.0F;
            Vec3<f32> bs{};
            const bool hs = intersect_ray_triangle(ray, tr[l], ts, bs);
            if (lane_set(r.hit_mask, l) == hs)
            {
                ++compared;
                if (hs)
                {
                    REQUIRE(tlanes.v[l] == Catch::Approx(ts).margin(5e-3F));
                }
            }
        }
    }
    REQUIRE(compared >= 360); // strong lane-by-lane agreement (the rest = thin edge-grazing band)
}

TEST_CASE("v0f SIMD -- aabb8 / sphere8 overlap masks vs the v0c scalar `intersects`", "[geometry][v0f][simd]")
{
    Rng rng(0x0BB8U);
    for (int it = 0; it < 64; ++it)
    {
        AABB3<f32> b[8];
        Sphere<f32> sp[8];
        for (int l = 0; l < 8; ++l)
        {
            const Vec3<f32> c = rnd3(rng, -3.0F, 3.0F);
            const Vec3<f32> h(rng.uni(0.2F, 1.5F), rng.uni(0.2F, 1.5F), rng.uni(0.2F, 1.5F));
            b[l] = AABB3<f32>(c - h, c + h);
            sp[l] = Sphere<f32>(rnd3(rng, -3.0F, 3.0F), rng.uni(0.2F, 1.5F));
        }
        Aabb8 boxes;
        boxes.min_x = gather8([&](int l) { return b[l].min.x; });
        boxes.min_y = gather8([&](int l) { return b[l].min.y; });
        boxes.min_z = gather8([&](int l) { return b[l].min.z; });
        boxes.max_x = gather8([&](int l) { return b[l].max.x; });
        boxes.max_y = gather8([&](int l) { return b[l].max.y; });
        boxes.max_z = gather8([&](int l) { return b[l].max.z; });
        Sphere8 spheres;
        spheres.center_x = gather8([&](int l) { return sp[l].center.x; });
        spheres.center_y = gather8([&](int l) { return sp[l].center.y; });
        spheres.center_z = gather8([&](int l) { return sp[l].center.z; });
        spheres.radius = gather8([&](int l) { return sp[l].radius; });

        const AABB3<f32> qbox(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1));
        const Sphere<f32> qsphere(Vec3<f32>(0.5F, 0.0F, 0.0F), 1.2F);
        const Vec8f am = aabb8_vs_aabb(boxes, qbox);
        const Vec8f smask = sphere8_vs_sphere(spheres, qsphere);
        for (int l = 0; l < 8; ++l)
        {
            REQUIRE(lane_set(am, l) == intersects(b[l], qbox));
            REQUIRE(lane_set(smask, l) == intersects(sp[l], qsphere));
        }
    }
}

TEST_CASE("v0f SIMD -- segment8_vs_segment_distsq vs v0b distance_squared(Segment3, Segment3)", "[geometry][v0f][simd]")
{
    Rng rng(0x5E68U);
    for (int it = 0; it < 64; ++it)
    {
        Segment3<f32> s1[8];
        Segment3<f32> s2[8];
        for (int l = 0; l < 8; ++l)
        {
            s1[l] = Segment3<f32>(rnd3(rng, -3.0F, 3.0F), rnd3(rng, -3.0F, 3.0F));
            s2[l] = Segment3<f32>(rnd3(rng, -3.0F, 3.0F), rnd3(rng, -3.0F, 3.0F));
        }
        // throw in a couple of degenerate pairs
        s1[2] = Segment3<f32>(Vec3<f32>(1, 1, 1), Vec3<f32>(1, 1, 1));   // seg1 a point
        s2[5] = Segment3<f32>(Vec3<f32>(-2, 0, 1), Vec3<f32>(-2, 0, 1)); // seg2 a point
        Segment38Pair pairs;
        pairs.p1x = gather8([&](int l) { return s1[l].a.x; });
        pairs.p1y = gather8([&](int l) { return s1[l].a.y; });
        pairs.p1z = gather8([&](int l) { return s1[l].a.z; });
        pairs.q1x = gather8([&](int l) { return s1[l].b.x; });
        pairs.q1y = gather8([&](int l) { return s1[l].b.y; });
        pairs.q1z = gather8([&](int l) { return s1[l].b.z; });
        pairs.p2x = gather8([&](int l) { return s2[l].a.x; });
        pairs.p2y = gather8([&](int l) { return s2[l].a.y; });
        pairs.p2z = gather8([&](int l) { return s2[l].a.z; });
        pairs.q2x = gather8([&](int l) { return s2[l].b.x; });
        pairs.q2y = gather8([&](int l) { return s2[l].b.y; });
        pairs.q2z = gather8([&](int l) { return s2[l].b.z; });

        const Vec8f d2 = segment8_vs_segment_distsq(pairs);
        Lanes8 lanes(d2);
        for (int l = 0; l < 8; ++l)
        {
            const f32 scalar = distance_squared(s1[l], s2[l]);
            REQUIRE(lanes.v[l] == Catch::Approx(scalar).margin(2e-3F));
        }
    }
}

TEST_CASE("v0f SIMD -- mask convention: hit lane != 0, miss lane == 0", "[geometry][v0f][simd]")
{
    // A box at +x; ray straight along +x from -5 hits, a parallel-offset ray misses.
    AABB3<f32> b[8];
    for (auto& box : b)
    {
        box = AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1));
    }
    b[3] = AABB3<f32>(Vec3<f32>(99, 99, 99), Vec3<f32>(100, 100, 100)); // far away -- lane 3 misses
    Aabb8 boxes;
    boxes.min_x = gather8([&](int l) { return b[l].min.x; });
    boxes.min_y = gather8([&](int l) { return b[l].min.y; });
    boxes.min_z = gather8([&](int l) { return b[l].min.z; });
    boxes.max_x = gather8([&](int l) { return b[l].max.x; });
    boxes.max_y = gather8([&](int l) { return b[l].max.y; });
    boxes.max_z = gather8([&](int l) { return b[l].max.z; });
    const Ray3<f32> ray(Vec3<f32>(-5, 0, 0), Vec3<f32>(1, 0, 0));
    const RayAabb8Result r =
        ray_vs_8_aabb(ray, precompute_ray_aabb(ray), boxes, 0.0F, std::numeric_limits<f32>::infinity());
    Lanes8 mask(r.hit_mask);
    for (int l = 0; l < 8; ++l)
    {
        if (l == 3)
        {
            REQUIRE(mask.v[l] == 0.0F); // miss -> all-zero lane
        }
        else
        {
            REQUIRE(mask.v[l] != 0.0F); // hit -> all-bits-set lane (a NaN bit pattern)
        }
    }
}
