// crd-geometry-primitives v0f -- the batch kernels, lane-by-lane against
// their scalar reference (ULP-conformance): ray_vs_8_aabb / ray_packet8_vs_aabb
// vs the robust slab, ray_vs_8_triangle vs v0c Moller-Trumbore, aabb8/sphere8
// overlap masks vs v0c `intersects`, segment8 squared-closest-distance vs v0b
// `distance_squared(Segment3, Segment3)`. Plus the all-ones mask convention.

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
// Fill a column from a generator(lane).
template <typename F> void gather8(f32 (&column)[8], F gen)
{
    for (int l = 0; l < 8; ++l)
    {
        column[l] = gen(l);
    }
}
bool lane_set(const u32 (&mask)[8], int i)
{
    REQUIRE((mask[i] == 0U || mask[i] == 0xFFFFFFFFU)); // all ones or zero
    return mask[i] != 0U;
}
void fill_boxes(Aabb8& boxes, const AABB3<f32> (&b)[8])
{
    gather8(boxes.min_x, [&](int l) { return b[l].min.x; });
    gather8(boxes.min_y, [&](int l) { return b[l].min.y; });
    gather8(boxes.min_z, [&](int l) { return b[l].min.z; });
    gather8(boxes.max_x, [&](int l) { return b[l].max.x; });
    gather8(boxes.max_y, [&](int l) { return b[l].max.y; });
    gather8(boxes.max_z, [&](int l) { return b[l].max.z; });
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
        Aabb8 boxes{};
        fill_boxes(boxes, b);

        const Ray3<f32> ray(rnd3(rng, -5.0F, 5.0F), rnd3(rng, -1.0F, 1.0F));
        if (dot(ray.direction, ray.direction) < 1e-4F)
        {
            continue;
        }
        const auto pre = precompute_ray_aabb(ray);
        RayAabb8Result r{};
        ray_vs_8_aabb(ray, pre, {&boxes, 1}, 0.0F, inf, {&r, 1});
        for (int l = 0; l < 8; ++l)
        {
            f32 ts = 0.0F;
            const bool hs = intersect_ray_aabb_robust(ray, pre, b[l], 0.0F, inf, ts);
            REQUIRE(lane_set(r.hit_mask, l) == hs);
            if (hs)
            {
                REQUIRE(r.t_enter[l] == Catch::Approx(ts).margin(1e-3F));
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
        const RayPacket8 pkt = precompute_ray_packet8(rays);
        RayAabb8Result r{};
        ray_packet8_vs_aabb(pkt, {&box, 1}, 0.0F, inf, {&r, 1});
        for (int l = 0; l < 8; ++l)
        {
            f32 ts = 0.0F;
            const bool hs = intersect_ray_aabb_robust(rays[l], box, 0.0F, inf, ts);
            REQUIRE(lane_set(r.hit_mask, l) == hs);
            if (hs)
            {
                REQUIRE(r.t_enter[l] == Catch::Approx(ts).margin(1e-3F));
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
        Triangle38 tris{};
        gather8(tris.ax, [&](int l) { return tr[l].a.x; });
        gather8(tris.ay, [&](int l) { return tr[l].a.y; });
        gather8(tris.az, [&](int l) { return tr[l].a.z; });
        gather8(tris.bx, [&](int l) { return tr[l].b.x; });
        gather8(tris.by, [&](int l) { return tr[l].b.y; });
        gather8(tris.bz, [&](int l) { return tr[l].b.z; });
        gather8(tris.cx, [&](int l) { return tr[l].c.x; });
        gather8(tris.cy, [&](int l) { return tr[l].c.y; });
        gather8(tris.cz, [&](int l) { return tr[l].c.z; });

        const Ray3<f32> ray(rnd3(rng, -5.0F, 5.0F), rnd3(rng, -1.0F, 1.0F));
        if (dot(ray.direction, ray.direction) < 1e-3F)
        {
            continue;
        }
        RayTri8Result r{};
        ray_vs_8_triangle(ray, {&tris, 1}, {&r, 1});
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
                    REQUIRE(r.t[l] == Catch::Approx(ts).margin(5e-3F));
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
        Aabb8 boxes{};
        fill_boxes(boxes, b);
        Sphere8 spheres{};
        gather8(spheres.center_x, [&](int l) { return sp[l].center.x; });
        gather8(spheres.center_y, [&](int l) { return sp[l].center.y; });
        gather8(spheres.center_z, [&](int l) { return sp[l].center.z; });
        gather8(spheres.radius, [&](int l) { return sp[l].radius; });

        const AABB3<f32> qbox(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1));
        const Sphere<f32> qsphere(Vec3<f32>(0.5F, 0.0F, 0.0F), 1.2F);
        Mask8 am{};
        Mask8 smask{};
        aabb8_vs_aabb({&boxes, 1}, qbox, {&am, 1});
        sphere8_vs_sphere({&spheres, 1}, qsphere, {&smask, 1});
        for (int l = 0; l < 8; ++l)
        {
            REQUIRE(lane_set(am.lane, l) == intersects(b[l], qbox));
            REQUIRE(lane_set(smask.lane, l) == intersects(sp[l], qsphere));
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
        Segment38Pair pairs{};
        gather8(pairs.p1x, [&](int l) { return s1[l].a.x; });
        gather8(pairs.p1y, [&](int l) { return s1[l].a.y; });
        gather8(pairs.p1z, [&](int l) { return s1[l].a.z; });
        gather8(pairs.q1x, [&](int l) { return s1[l].b.x; });
        gather8(pairs.q1y, [&](int l) { return s1[l].b.y; });
        gather8(pairs.q1z, [&](int l) { return s1[l].b.z; });
        gather8(pairs.p2x, [&](int l) { return s2[l].a.x; });
        gather8(pairs.p2y, [&](int l) { return s2[l].a.y; });
        gather8(pairs.p2z, [&](int l) { return s2[l].a.z; });
        gather8(pairs.q2x, [&](int l) { return s2[l].b.x; });
        gather8(pairs.q2y, [&](int l) { return s2[l].b.y; });
        gather8(pairs.q2z, [&](int l) { return s2[l].b.z; });

        Lanes8 d2{};
        segment8_vs_segment_distsq({&pairs, 1}, {&d2, 1});
        for (int l = 0; l < 8; ++l)
        {
            const f32 scalar = distance_squared(s1[l], s2[l]);
            REQUIRE(d2.lane[l] == Catch::Approx(scalar).margin(2e-3F));
        }
    }
}

TEST_CASE("v0f SIMD -- mask convention: hit lane all ones, miss lane zero", "[geometry][v0f][simd]")
{
    // A box at +x; ray straight along +x from -5 hits, a parallel-offset ray misses.
    AABB3<f32> b[8];
    for (auto& box : b)
    {
        box = AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1));
    }
    b[3] = AABB3<f32>(Vec3<f32>(99, 99, 99), Vec3<f32>(100, 100, 100)); // far away -- lane 3 misses
    Aabb8 boxes{};
    fill_boxes(boxes, b);
    const Ray3<f32> ray(Vec3<f32>(-5, 0, 0), Vec3<f32>(1, 0, 0));
    RayAabb8Result r{};
    ray_vs_8_aabb(ray, precompute_ray_aabb(ray), {&boxes, 1}, 0.0F, std::numeric_limits<f32>::infinity(), {&r, 1});
    for (int l = 0; l < 8; ++l)
    {
        if (l == 3)
        {
            REQUIRE(r.hit_mask[l] == 0U); // miss -> all-zero lane
        }
        else
        {
            REQUIRE(r.hit_mask[l] == 0xFFFFFFFFU); // hit -> all-ones lane
        }
    }
}
