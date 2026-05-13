// crd-geometry-bvh v1e — bvh_closest_point tests: matches brute force over the
// prim AABBs, the max_dist cutoff, query-inside-a-box, empty / single-prim.

#include <crd/geometry/bvh/bvh.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>
#include <vector>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::bvh::bvh_build;
using crd::geometry::bvh::bvh_closest_point;
using crd::geometry::bvh::BvhBuildOptions;
using crd::geometry::bvh::BvhClosestPoint;
using crd::geometry::bvh::BvhTree;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::closest_point;
using crd::math::dot;
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

// Brute-force closest prim using the exact same per-box test the BVH uses.
std::optional<BvhClosestPoint> brute_closest(const std::vector<AABB3<f32>>& prims, const Vec3<f32>& q, f32 max_dist)
{
    f32 best =
        (max_dist >= std::numeric_limits<f32>::infinity()) ? std::numeric_limits<f32>::infinity() : max_dist * max_dist;
    u32 best_p = 0;
    Vec3<f32> best_pt{};
    bool hit = false;
    for (usize i = 0; i < prims.size(); ++i)
    {
        const Vec3<f32> cp = closest_point(prims[i], q);
        const Vec3<f32> d = cp - q;
        const f32 d2 = dot(d, d);
        if (d2 < best)
        {
            best = d2;
            best_p = static_cast<u32>(i);
            best_pt = cp;
            hit = true;
        }
    }
    if (!hit)
    {
        return std::nullopt;
    }
    return BvhClosestPoint{best_pt, best, best_p};
}

} // namespace

TEST_CASE("BVH closest-point: empty tree returns nullopt", "[geometry][bvh][closest]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(), &alloc);
    REQUIRE_FALSE(bvh_closest_point(tree, crd::containers::ConstSpan<AABB3<f32>>(), Vec3<f32>(0, 0, 0)).has_value());
}

TEST_CASE("BVH closest-point: matches brute force on a random corpus", "[geometry][bvh][closest]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0xC105E);
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
        const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
        const BvhTree tree = bvh_build(pspan, &alloc, opts);

        for (usize r = 0; r < 400; ++r)
        {
            const Vec3<f32> q(rng.range(-120, 120), rng.range(-120, 120), rng.range(-120, 120));
            const f32 max_dist = (r % 5U == 0U) ? rng.range(5.0F, 60.0F) : std::numeric_limits<f32>::infinity();
            const std::optional<BvhClosestPoint> got = bvh_closest_point(tree, pspan, q, max_dist);
            const std::optional<BvhClosestPoint> ref = brute_closest(prims, q, max_dist);
            REQUIRE(got.has_value() == ref.has_value());
            if (got)
            {
                // The squared distance is computed identically (same closest_point call), so it
                // bit-matches; on a tie the BVH and brute force may name different prims, but each
                // named prim genuinely realizes that distance, and `point` is on that prim's AABB.
                REQUIRE(got->distance_squared == ref->distance_squared);
                const Vec3<f32> d = got->point - q;
                REQUIRE(dot(d, d) == got->distance_squared);
                REQUIRE(closest_point(prims[got->payload], q) == got->point);
            }
        }
    }
}

TEST_CASE("BVH closest-point: query inside a box gives distance 0 at the query point", "[geometry][bvh][closest]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");
    std::vector<AABB3<f32>> prims = {AABB3<f32>(Vec3<f32>(-2, -2, -2), Vec3<f32>(2, 2, 2)),
                                     AABB3<f32>(Vec3<f32>(10, 10, 10), Vec3<f32>(12, 12, 12))};
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    const Vec3<f32> q(0.5F, -1.0F, 1.3F); // inside box 0
    const std::optional<BvhClosestPoint> got = bvh_closest_point(tree, pspan, q);
    REQUIRE(got.has_value());
    REQUIRE(got->payload == 0U);
    REQUIRE(got->distance_squared == 0.0F);
    REQUIRE(got->point == q);
}

TEST_CASE("BVH closest-point: max_dist cutoff", "[geometry][bvh][closest]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");
    std::vector<AABB3<f32>> prims = {AABB3<f32>(Vec3<f32>(9, -1, -1), Vec3<f32>(11, 1, 1))}; // closest face at x=9
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    const Vec3<f32> q(0, 0, 0); // distance to the box is 9
    REQUIRE(bvh_closest_point(tree, pspan, q, 10.0F).has_value());
    REQUIRE_FALSE(bvh_closest_point(tree, pspan, q, 5.0F).has_value());
    const std::optional<BvhClosestPoint> got = bvh_closest_point(tree, pspan, q, 10.0F);
    REQUIRE(got.has_value());
    REQUIRE(got->distance_squared == 81.0F); // 9^2
    REQUIRE(got->point == Vec3<f32>(9, 0, 0));
}

TEST_CASE("BVH closest-point: single primitive", "[geometry][bvh][closest]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");
    const AABB3<f32> box(Vec3<f32>(-1, -2, -3), Vec3<f32>(4, 5, 6));
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(&box, 1);
    const BvhTree tree = bvh_build(pspan, &alloc);
    const Vec3<f32> q(100, 0, 0);
    const std::optional<BvhClosestPoint> got = bvh_closest_point(tree, pspan, q);
    REQUIRE(got.has_value());
    REQUIRE(got->payload == 0U);
    REQUIRE(got->point == closest_point(box, q));
}
