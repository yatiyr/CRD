// Phase 3.1.7 v5-queries-extension — facade parity tests.
//
// Verifies that the unified `crd::geometry::{raycast,overlap,radius,
// nearest_n,find_overlapping_pairs}` overloads forward correctly to each
// v5 backend's native API — same results, same emission order, byte-
// identical for deterministic inputs.
//
// Doesn't re-cover correctness (that's what the per-backend tests are
// for) — covers the WIRING. A regression in the facade's overload
// resolution or arg-passing shows up as a parity mismatch.

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/queries.hpp>          // the facade under test
#include <crd/geometry/spatial/spatial.hpp>          // backend headers
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;
using crd::math::Vec3f;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{64U << 20}; };

AABB3<f32> aabb_around(const Vec3f& c, f32 h)
{
    return AABB3<f32>{Vec3f{c.x - h, c.y - h, c.z - h}, Vec3f{c.x + h, c.y + h, c.z + h}};
}
} // namespace

// =============================================================================
// LooseOctree — overlap + raycast
// =============================================================================

TEST_CASE("queries facade: LooseOctree overlap matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::OctreeBuildOptions<f32> opts{
        AABB3<f32>{Vec3f{-50, -50, -50}, Vec3f{50, 50, 50}}, 2.0F, 8U, 8U};
    crd::geometry::spatial::LooseOctree<f32> tree{&f.alloc, opts};
    std::mt19937 rng(13U);
    std::uniform_real_distribution<f32> uc(-40.0F, 40.0F);
    std::uniform_real_distribution<f32> uh(0.3F, 1.0F);
    for (u32 i = 0; i < 100U; ++i)
    {
        (void)tree.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)), i);
    }

    const AABB3<f32> q = aabb_around(Vec3f{0, 0, 0}, 10.0F);

    crd::containers::Array<u32> via_native(&f.alloc);
    tree.overlap(q, via_native);
    std::sort(via_native.data(), via_native.data() + via_native.size());

    crd::containers::Array<u32> via_facade(&f.alloc);
    crd::geometry::overlap(tree, q, via_facade);
    std::sort(via_facade.data(), via_facade.data() + via_facade.size());

    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i) { REQUIRE(via_facade[i] == via_native[i]); }
}

TEST_CASE("queries facade: LooseOctree raycast matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::OctreeBuildOptions<f32> opts{
        AABB3<f32>{Vec3f{-50, -50, -50}, Vec3f{50, 50, 50}}, 2.0F, 8U, 8U};
    crd::geometry::spatial::LooseOctree<f32> tree{&f.alloc, opts};
    (void)tree.insert(aabb_around(Vec3f{5, 0, 0}, 0.5F), 5U);
    (void)tree.insert(aabb_around(Vec3f{10, 0, 0}, 0.5F), 10U);

    Ray3<f32> ray{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}};
    auto via_native = tree.raycast(ray);
    auto via_facade = crd::geometry::raycast(tree, ray);

    REQUIRE(via_facade.has_value() == via_native.has_value());
    REQUIRE(via_facade->payload == via_native->payload);
    REQUIRE(via_facade->t == via_native->t);
}

// =============================================================================
// RTree — overlap + raycast + nearest_n
// =============================================================================

TEST_CASE("queries facade: RTree overlap matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::RTree<f32> tree{&f.alloc};
    std::mt19937 rng(13U);
    std::uniform_real_distribution<f32> uc(-40.0F, 40.0F);
    std::uniform_real_distribution<f32> uh(0.3F, 1.0F);
    for (u32 i = 0; i < 100U; ++i)
    {
        (void)tree.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)), i);
    }
    const AABB3<f32> q = aabb_around(Vec3f{0, 0, 0}, 10.0F);
    crd::containers::Array<u32> via_native(&f.alloc);
    tree.overlap(q, via_native);
    std::sort(via_native.data(), via_native.data() + via_native.size());
    crd::containers::Array<u32> via_facade(&f.alloc);
    crd::geometry::overlap(tree, q, via_facade);
    std::sort(via_facade.data(), via_facade.data() + via_facade.size());
    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i) { REQUIRE(via_facade[i] == via_native[i]); }
}

TEST_CASE("queries facade: RTree raycast matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::RTree<f32> tree{&f.alloc};
    (void)tree.insert(aabb_around(Vec3f{5, 0, 0}, 0.5F), 5U);
    (void)tree.insert(aabb_around(Vec3f{10, 0, 0}, 0.5F), 10U);
    Ray3<f32> ray{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}};
    auto via_native = tree.raycast(ray);
    auto via_facade = crd::geometry::raycast(tree, ray);
    REQUIRE(via_facade->payload == via_native->payload);
    REQUIRE(via_facade->t == via_native->t);
}

TEST_CASE("queries facade: RTree nearest_n matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::RTree<f32> tree{&f.alloc};
    std::mt19937 rng(17U);
    std::uniform_real_distribution<f32> uc(-30.0F, 30.0F);
    for (u32 i = 0; i < 80U; ++i)
    {
        (void)tree.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, 0.4F), i);
    }
    const Vec3f q{0, 0, 0};
    constexpr usize k = 5;
    crd::containers::Array<typename crd::geometry::spatial::RTree<f32>::Neighbor> via_native(&f.alloc);
    tree.nearest_n(q, k, via_native);
    crd::containers::Array<typename crd::geometry::spatial::RTree<f32>::Neighbor> via_facade(&f.alloc);
    crd::geometry::nearest_n(tree, q, k, via_facade);
    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i)
    {
        REQUIRE(via_facade[i].payload == via_native[i].payload);
        REQUIRE(via_facade[i].distance_squared == via_native[i].distance_squared);
    }
}

// =============================================================================
// SpatialHash — overlap + raycast + radius + find_overlapping_pairs
// =============================================================================

TEST_CASE("queries facade: SpatialHash overlap matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::SpatialHash<f32> tree{&f.alloc,
        crd::geometry::spatial::SpatialHashConfig<f32>{2.0F, 256U}};
    std::mt19937 rng(13U);
    std::uniform_real_distribution<f32> uc(-40.0F, 40.0F);
    std::uniform_real_distribution<f32> uh(0.3F, 1.0F);
    for (u32 i = 0; i < 100U; ++i)
    {
        (void)tree.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)), i);
    }
    const AABB3<f32> q = aabb_around(Vec3f{0, 0, 0}, 10.0F);
    crd::containers::Array<u32> via_native(&f.alloc);
    tree.overlap(q, via_native);
    std::sort(via_native.data(), via_native.data() + via_native.size());
    crd::containers::Array<u32> via_facade(&f.alloc);
    crd::geometry::overlap(tree, q, via_facade);
    std::sort(via_facade.data(), via_facade.data() + via_facade.size());
    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i) { REQUIRE(via_facade[i] == via_native[i]); }
}

TEST_CASE("queries facade: SpatialHash radius matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::SpatialHash<f32> tree{&f.alloc,
        crd::geometry::spatial::SpatialHashConfig<f32>{1.0F, 256U}};
    std::mt19937 rng(17U);
    std::uniform_real_distribution<f32> uc(-30.0F, 30.0F);
    for (u32 i = 0; i < 80U; ++i)
    {
        (void)tree.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, 0.4F), i);
    }
    const Vec3f q{0, 0, 0};
    crd::containers::Array<u32> via_native(&f.alloc);
    tree.radius(q, 5.0F, via_native);
    std::sort(via_native.data(), via_native.data() + via_native.size());
    crd::containers::Array<u32> via_facade(&f.alloc);
    crd::geometry::radius(tree, q, 5.0F, via_facade);
    std::sort(via_facade.data(), via_facade.data() + via_facade.size());
    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i) { REQUIRE(via_facade[i] == via_native[i]); }
}

TEST_CASE("queries facade: SpatialHash raycast matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::SpatialHash<f32> tree{&f.alloc,
        crd::geometry::spatial::SpatialHashConfig<f32>{1.0F, 256U}};
    (void)tree.insert(aabb_around(Vec3f{5, 0, 0}, 0.5F), 5U);
    (void)tree.insert(aabb_around(Vec3f{10, 0, 0}, 0.5F), 10U);
    Ray3<f32> ray{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}};
    auto via_native = tree.raycast(ray);
    auto via_facade = crd::geometry::raycast(tree, ray);
    REQUIRE(via_facade->payload == via_native->payload);
    REQUIRE(via_facade->t == via_native->t);
}

TEST_CASE("queries facade: SpatialHash find_overlapping_pairs matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::SpatialHash<f32> tree{&f.alloc,
        crd::geometry::spatial::SpatialHashConfig<f32>{1.0F, 256U}};
    std::mt19937 rng(99U);
    std::uniform_real_distribution<f32> uc(-10.0F, 10.0F);
    std::uniform_real_distribution<f32> uh(0.3F, 0.7F);
    for (u32 i = 0; i < 40U; ++i)
    {
        (void)tree.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)), i);
    }
    crd::containers::Array<crd::geometry::spatial::SpatialHashPair> via_native(&f.alloc);
    tree.find_overlapping_pairs(via_native);
    crd::containers::Array<crd::geometry::spatial::SpatialHashPair> via_facade(&f.alloc);
    crd::geometry::find_overlapping_pairs(tree, via_facade);
    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i) { REQUIRE(via_facade[i] == via_native[i]); }
}

// =============================================================================
// UniformGrid — overlap + raycast + radius + find_overlapping_pairs
// =============================================================================

TEST_CASE("queries facade: UniformGrid overlap matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::UniformGrid<f32> tree{&f.alloc,
        crd::geometry::spatial::UniformGridConfig<f32>{
            AABB3<f32>{Vec3f{-40, -40, -40}, Vec3f{40, 40, 40}}, 2.0F}};
    std::mt19937 rng(13U);
    std::uniform_real_distribution<f32> uc(-35.0F, 35.0F);
    std::uniform_real_distribution<f32> uh(0.3F, 1.0F);
    for (u32 i = 0; i < 100U; ++i)
    {
        (void)tree.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)), i);
    }
    const AABB3<f32> q = aabb_around(Vec3f{0, 0, 0}, 10.0F);
    crd::containers::Array<u32> via_native(&f.alloc);
    tree.overlap(q, via_native);
    std::sort(via_native.data(), via_native.data() + via_native.size());
    crd::containers::Array<u32> via_facade(&f.alloc);
    crd::geometry::overlap(tree, q, via_facade);
    std::sort(via_facade.data(), via_facade.data() + via_facade.size());
    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i) { REQUIRE(via_facade[i] == via_native[i]); }
}

TEST_CASE("queries facade: UniformGrid radius matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::UniformGrid<f32> tree{&f.alloc,
        crd::geometry::spatial::UniformGridConfig<f32>{
            AABB3<f32>{Vec3f{-30, -30, -30}, Vec3f{30, 30, 30}}, 1.0F}};
    std::mt19937 rng(17U);
    std::uniform_real_distribution<f32> uc(-25.0F, 25.0F);
    for (u32 i = 0; i < 80U; ++i)
    {
        (void)tree.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, 0.4F), i);
    }
    const Vec3f q{0, 0, 0};
    crd::containers::Array<u32> via_native(&f.alloc);
    tree.radius(q, 5.0F, via_native);
    std::sort(via_native.data(), via_native.data() + via_native.size());
    crd::containers::Array<u32> via_facade(&f.alloc);
    crd::geometry::radius(tree, q, 5.0F, via_facade);
    std::sort(via_facade.data(), via_facade.data() + via_facade.size());
    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i) { REQUIRE(via_facade[i] == via_native[i]); }
}

TEST_CASE("queries facade: UniformGrid raycast matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::UniformGrid<f32> tree{&f.alloc,
        crd::geometry::spatial::UniformGridConfig<f32>{
            AABB3<f32>{Vec3f{-50, -50, -50}, Vec3f{50, 50, 50}}, 2.0F}};
    (void)tree.insert(aabb_around(Vec3f{5, 0, 0}, 0.5F), 5U);
    (void)tree.insert(aabb_around(Vec3f{10, 0, 0}, 0.5F), 10U);
    Ray3<f32> ray{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}};
    auto via_native = tree.raycast(ray);
    auto via_facade = crd::geometry::raycast(tree, ray);
    REQUIRE(via_facade->payload == via_native->payload);
    REQUIRE(via_facade->t == via_native->t);
}

TEST_CASE("queries facade: UniformGrid find_overlapping_pairs matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::UniformGrid<f32> tree{&f.alloc,
        crd::geometry::spatial::UniformGridConfig<f32>{
            AABB3<f32>{Vec3f{-20, -20, -20}, Vec3f{20, 20, 20}}, 1.0F}};
    std::mt19937 rng(99U);
    std::uniform_real_distribution<f32> uc(-10.0F, 10.0F);
    std::uniform_real_distribution<f32> uh(0.3F, 0.7F);
    for (u32 i = 0; i < 40U; ++i)
    {
        (void)tree.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)), i);
    }
    crd::containers::Array<crd::geometry::spatial::UniformGridPair> via_native(&f.alloc);
    tree.find_overlapping_pairs(via_native);
    crd::containers::Array<crd::geometry::spatial::UniformGridPair> via_facade(&f.alloc);
    crd::geometry::find_overlapping_pairs(tree, via_facade);
    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i) { REQUIRE(via_facade[i] == via_native[i]); }
}

// =============================================================================
// KdTree — radius + nearest_n
// =============================================================================

TEST_CASE("queries facade: KdTree radius matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3f> pts(&f.alloc);
    std::mt19937 rng(31U);
    std::uniform_real_distribution<f32> u(-1.0F, 1.0F);
    for (u32 i = 0; i < 200U; ++i) { pts.push_back(Vec3f{u(rng), u(rng), u(rng)}); }
    auto tree = crd::geometry::spatial::kd_build<f32>(
        crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);
    const Vec3f q{0, 0, 0};
    crd::containers::Array<crd::geometry::spatial::KdRadiusHit<f32>> via_native(&f.alloc);
    crd::geometry::spatial::kd_radius<f32>(tree,
        crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, q, 0.5F, via_native);
    crd::containers::Array<crd::geometry::spatial::KdRadiusHit<f32>> via_facade(&f.alloc);
    crd::geometry::radius(tree,
        crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, q, 0.5F, via_facade);
    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i)
    {
        REQUIRE(via_facade[i].payload == via_native[i].payload);
        REQUIRE(via_facade[i].distance_squared == via_native[i].distance_squared);
    }
}

TEST_CASE("queries facade: KdTree nearest_n matches native", "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3f> pts(&f.alloc);
    std::mt19937 rng(42U);
    std::uniform_real_distribution<f32> u(-1.0F, 1.0F);
    for (u32 i = 0; i < 200U; ++i) { pts.push_back(Vec3f{u(rng), u(rng), u(rng)}); }
    auto tree = crd::geometry::spatial::kd_build<f32>(
        crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);
    const Vec3f q{0, 0, 0};
    constexpr usize k = 10;
    crd::containers::Array<crd::geometry::spatial::KdNeighbor<f32>> via_native(&f.alloc);
    crd::geometry::spatial::kd_nearest_n<f32>(tree,
        crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, q, k, via_native);
    crd::containers::Array<crd::geometry::spatial::KdNeighbor<f32>> via_facade(&f.alloc);
    crd::geometry::nearest_n(tree,
        crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, q, k, via_facade);
    REQUIRE(via_facade.size() == via_native.size());
    for (usize i = 0; i < via_facade.size(); ++i)
    {
        REQUIRE(via_facade[i].payload == via_native[i].payload);
        REQUIRE(via_facade[i].distance_squared == via_native[i].distance_squared);
    }
}

// =============================================================================
// Cross-backend uniformity — same name across 4 AABB backends
// =============================================================================

TEST_CASE("queries facade: same overload name across 4 AABB backends returns same set",
          "[geometry-spatial][queries][facade]")
{
    AllocFixture f{};
    crd::geometry::spatial::LooseOctree<f32> octree{&f.alloc,
        crd::geometry::spatial::OctreeBuildOptions<f32>{
            AABB3<f32>{Vec3f{-50, -50, -50}, Vec3f{50, 50, 50}}, 2.0F, 8U, 8U}};
    crd::geometry::spatial::RTree<f32> rtree{&f.alloc};
    crd::geometry::spatial::SpatialHash<f32> shash{&f.alloc,
        crd::geometry::spatial::SpatialHashConfig<f32>{2.0F, 256U}};
    crd::geometry::spatial::UniformGrid<f32> ugrid{&f.alloc,
        crd::geometry::spatial::UniformGridConfig<f32>{
            AABB3<f32>{Vec3f{-50, -50, -50}, Vec3f{50, 50, 50}}, 2.0F}};

    std::mt19937 rng(7U);
    std::uniform_real_distribution<f32> uc(-40.0F, 40.0F);
    for (u32 i = 0; i < 80U; ++i)
    {
        const auto box = aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, 0.5F);
        (void)octree.insert(box, i);
        (void)rtree.insert(box, i);
        (void)shash.insert(box, i);
        (void)ugrid.insert(box, i);
    }

    const AABB3<f32> q = aabb_around(Vec3f{0, 0, 0}, 8.0F);

    // Same function name (`crd::geometry::overlap`), 4 different trees.
    crd::containers::Array<u32> a(&f.alloc), b(&f.alloc), c(&f.alloc), d(&f.alloc);
    crd::geometry::overlap(octree, q, a);
    crd::geometry::overlap(rtree, q, b);
    crd::geometry::overlap(shash, q, c);
    crd::geometry::overlap(ugrid, q, d);

    auto sort_arr = [](auto& arr) { std::sort(arr.data(), arr.data() + arr.size()); };
    sort_arr(a); sort_arr(b); sort_arr(c); sort_arr(d);

    // All four return the SAME set on the same content.
    REQUIRE(a.size() == b.size());
    REQUIRE(b.size() == c.size());
    REQUIRE(c.size() == d.size());
    for (usize i = 0; i < a.size(); ++i)
    {
        REQUIRE(a[i] == b[i]);
        REQUIRE(b[i] == c[i]);
        REQUIRE(c[i] == d[i]);
    }
}
