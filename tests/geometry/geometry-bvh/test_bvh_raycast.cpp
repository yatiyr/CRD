// crd-geometry-bvh v1a — query tests: nearest-hit raycast vs a brute-force
// reference, AABB-overlap vs brute force, and the degenerate cases.

#include <crd/geometry/bvh/bvh.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::bvh::bvh_build;
using crd::geometry::bvh::bvh_overlap;
using crd::geometry::bvh::bvh_raycast;
using crd::geometry::bvh::BvhBuildOptions;
using crd::geometry::bvh::BvhRayHit;
using crd::geometry::bvh::BvhTree;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::intersect_ray_aabb_robust;
using crd::geometry::primitives::intersects;
using crd::geometry::primitives::Ray3;
using crd::math::Vec3;

struct Rng
{
    crd::u64 state;
    explicit Rng(crd::u64 seed) : state(seed) {}
    crd::u64 next()
    {
        crd::u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    f32 unit() { return static_cast<f32>(next() >> 40) / static_cast<f32>(1U << 24); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
};

AABB3<f32> random_box(Rng& rng, f32 world, f32 max_size)
{
    const Vec3<f32> c(rng.range(-world, world), rng.range(-world, world), rng.range(-world, world));
    const Vec3<f32> h(rng.range(0.05F, max_size), rng.range(0.05F, max_size), rng.range(0.05F, max_size));
    return AABB3<f32>(Vec3<f32>(c.x - h.x, c.y - h.y, c.z - h.z), Vec3<f32>(c.x + h.x, c.y + h.y, c.z + h.z));
}

Vec3<f32> normalized(const Vec3<f32>& v)
{
    const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return Vec3<f32>(v.x / len, v.y / len, v.z / len);
}

// Brute-force nearest hit using the exact same per-box test the BVH uses.
std::optional<BvhRayHit> brute_raycast(const std::vector<AABB3<f32>>& prims, const Ray3<f32>& ray, f32 tmax)
{
    f32 best_t = tmax;
    u32 best_p = 0;
    bool hit = false;
    for (usize i = 0; i < prims.size(); ++i)
    {
        f32 t = 0.0F;
        if (intersect_ray_aabb_robust(ray, prims[i], 0.0F, best_t, t) && t < best_t)
        {
            best_t = t;
            best_p = static_cast<u32>(i);
            hit = true;
        }
    }
    if (!hit)
    {
        return std::nullopt;
    }
    return BvhRayHit{best_t, best_p};
}

constexpr f32 kInf = std::numeric_limits<f32>::infinity();

} // namespace

TEST_CASE("BVH raycast: empty tree returns nullopt", "[geometry][bvh][query]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(), &alloc);
    const Ray3<f32> ray{Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0)};
    REQUIRE_FALSE(bvh_raycast(tree, crd::containers::ConstSpan<AABB3<f32>>(), ray).has_value());
}

TEST_CASE("BVH raycast: matches brute force on a random corpus", "[geometry][bvh][query]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0xCA57);
    for (usize trial = 0; trial < 4; ++trial)
    {
        const usize n = 50U + (rng.next() % 600U);
        std::vector<AABB3<f32>> prims;
        for (usize i = 0; i < n; ++i)
        {
            prims.push_back(random_box(rng, 80.0F, 4.0F));
        }
        BvhBuildOptions opts;
        opts.max_leaf_prims = static_cast<crd::u16>(1U + (rng.next() % 6U));
        const BvhTree tree =
            bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc, opts);
        const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());

        for (usize r = 0; r < 400; ++r)
        {
            const Ray3<f32> ray{Vec3<f32>(rng.range(-120, 120), rng.range(-120, 120), rng.range(-120, 120)),
                                normalized(Vec3<f32>(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)))};
            const f32 tmax = (r % 5U == 0U) ? rng.range(10.0F, 60.0F) : kInf;
            const std::optional<BvhRayHit> got = bvh_raycast(tree, pspan, ray, tmax);
            const std::optional<BvhRayHit> ref = brute_raycast(prims, ray, tmax);
            REQUIRE(got.has_value() == ref.has_value());
            if (got)
            {
                // The parameter is exact (entry t is not subject to the Ize tmax pad), so it must
                // bit-match the brute-force value; on a t-tie between two boxes the BVH and the
                // brute force may name different prims, but each named prim is genuinely hit at t.
                REQUIRE(got->t == ref->t);
                f32 check = 0.0F;
                REQUIRE(intersect_ray_aabb_robust(ray, prims[got->payload], 0.0F, kInf, check));
                REQUIRE(check == got->t);
            }
        }
    }
}

TEST_CASE("BVH raycast: ray origin inside a box hits at t = 0", "[geometry][bvh][query]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");
    std::vector<AABB3<f32>> prims = {AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1)),
                                     AABB3<f32>(Vec3<f32>(5, 5, 5), Vec3<f32>(7, 7, 7))};
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc);
    const Ray3<f32> ray{Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0)};
    const std::optional<BvhRayHit> got =
        bvh_raycast(tree, crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), ray);
    REQUIRE(got.has_value());
    REQUIRE(got->payload == 0U);
    REQUIRE(got->t == 0.0F);
}

TEST_CASE("BVH raycast: tmax clamps out a far hit", "[geometry][bvh][query]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");
    std::vector<AABB3<f32>> prims = {AABB3<f32>(Vec3<f32>(9, -1, -1), Vec3<f32>(11, 1, 1))};
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc);
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const Ray3<f32> ray{Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0)};
    REQUIRE(bvh_raycast(tree, pspan, ray, 12.0F).has_value());      // box entered at t = 9
    REQUIRE_FALSE(bvh_raycast(tree, pspan, ray, 5.0F).has_value()); // clamped before t = 9
}

TEST_CASE("BVH raycast: axis-aligned grazing ray along an edge", "[geometry][bvh][query]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");
    std::vector<AABB3<f32>> prims = {AABB3<f32>(Vec3<f32>(2, 0, 0), Vec3<f32>(4, 2, 2))};
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc);
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    // Ray grazing the y=0,z=0 edge of the box — the robust slab keeps boundary contact.
    const Ray3<f32> ray{Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0)};
    const std::optional<BvhRayHit> got = bvh_raycast(tree, pspan, ray);
    REQUIRE(got.has_value());
    REQUIRE(got->payload == 0U);
    REQUIRE(got->t == 2.0F);
}

TEST_CASE("BVH overlap: matches brute force, callback and Array forms agree", "[geometry][bvh][query]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0x07E11A);
    for (usize trial = 0; trial < 4; ++trial)
    {
        const usize n = 30U + (rng.next() % 500U);
        std::vector<AABB3<f32>> prims;
        for (usize i = 0; i < n; ++i)
        {
            prims.push_back(random_box(rng, 60.0F, 3.0F));
        }
        BvhBuildOptions opts;
        opts.max_leaf_prims = static_cast<crd::u16>(1U + (rng.next() % 6U));
        const BvhTree tree =
            bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc, opts);
        const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());

        for (usize q = 0; q < 200; ++q)
        {
            const AABB3<f32> box = random_box(rng, 60.0F, 8.0F);
            std::vector<u32> ref;
            for (usize i = 0; i < n; ++i)
            {
                if (intersects(prims[i], box))
                {
                    ref.push_back(static_cast<u32>(i));
                }
            }
            crd::containers::Array<u32> got_arr(&alloc);
            bvh_overlap(tree, pspan, box, got_arr);
            std::vector<u32> got_cb;
            bvh_overlap(tree, pspan, box, [&](u32 p) { got_cb.push_back(p); });

            std::vector<u32> got_v(got_arr.data(), got_arr.data() + got_arr.size());
            std::sort(ref.begin(), ref.end());
            std::sort(got_v.begin(), got_v.end());
            std::sort(got_cb.begin(), got_cb.end());
            REQUIRE(got_v == ref);
            REQUIRE(got_cb == ref);
        }
    }
}
