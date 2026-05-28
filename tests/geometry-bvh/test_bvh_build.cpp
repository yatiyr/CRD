// crd-geometry-bvh v1a — binned-SAH builder tests: structural invariants,
// leaf-size cap, degenerate corpora, deterministic replay, SAH quality.

#include <crd/geometry/bvh/bvh.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <vector> // test-only scratch — not engine code

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::bvh::bvh_build;
using crd::geometry::bvh::bvh_sah_cost;
using crd::geometry::bvh::BvhBuildOptions;
using crd::geometry::bvh::BvhNode;
using crd::geometry::bvh::BvhTree;
using crd::geometry::primitives::AABB3;
using crd::math::Vec3;

// splitmix64 — deterministic test PRNG.
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
    f32 unit() { return static_cast<f32>(next() >> 40) / static_cast<f32>(1U << 24); } // [0,1)
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
};

AABB3<f32> random_box(Rng& rng, f32 world, f32 max_size)
{
    const Vec3<f32> c(rng.range(-world, world), rng.range(-world, world), rng.range(-world, world));
    const Vec3<f32> h(rng.range(0.01F, max_size), rng.range(0.01F, max_size), rng.range(0.01F, max_size));
    return AABB3<f32>(Vec3<f32>(c.x - h.x, c.y - h.y, c.z - h.z), Vec3<f32>(c.x + h.x, c.y + h.y, c.z + h.z));
}

bool encloses(const AABB3<f32>& outer, const AABB3<f32>& inner) noexcept
{
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && outer.min.z <= inner.min.z &&
           outer.max.x >= inner.max.x && outer.max.y >= inner.max.y && outer.max.z >= inner.max.z;
}

// Walks the tree and checks every structural invariant against `prims`.
void validate_tree(const BvhTree& tree, const std::vector<AABB3<f32>>& prims, u32 max_leaf)
{
    REQUIRE(tree.prim_count() == prims.size());
    if (prims.empty())
    {
        REQUIRE(tree.is_empty());
        REQUIRE(tree.node_count() == 0);
        return;
    }
    const auto nodes = tree.nodes();
    const auto idx = tree.prim_indices();
    REQUIRE(tree.root() == 0U);
    REQUIRE(nodes.size() >= 1U);
    REQUIRE(nodes.size() <= 2U * prims.size());

    std::vector<u32> ref_count(prims.size(), 0);
    usize total_leaf_prims = 0;
    for (usize ni = 0; ni < nodes.size(); ++ni)
    {
        const BvhNode& node = nodes[ni];
        if (node.is_leaf())
        {
            REQUIRE(node.prim_count >= 1U);
            REQUIRE(static_cast<u32>(node.prim_count) <= max_leaf);
            AABB3<f32> tight(Vec3<f32>(1e30F, 1e30F, 1e30F), Vec3<f32>(-1e30F, -1e30F, -1e30F));
            for (u32 k = node.left_first; k < node.left_first + node.prim_count; ++k)
            {
                REQUIRE(k < idx.size());
                const u32 p = idx[k];
                REQUIRE(p < prims.size());
                ++ref_count[p];
                tight.min.x = std::min(tight.min.x, prims[p].min.x);
                tight.min.y = std::min(tight.min.y, prims[p].min.y);
                tight.min.z = std::min(tight.min.z, prims[p].min.z);
                tight.max.x = std::max(tight.max.x, prims[p].max.x);
                tight.max.y = std::max(tight.max.y, prims[p].max.y);
                tight.max.z = std::max(tight.max.z, prims[p].max.z);
            }
            // The leaf's recorded bounds are the exact union of its prims (min/max are rounding-free).
            REQUIRE(node.bounds == tight);
            total_leaf_prims += node.prim_count;
        }
        else
        {
            REQUIRE(node.left_first + 1U < nodes.size());
            REQUIRE(node.split_axis < 3U);
            REQUIRE(encloses(node.bounds, nodes[node.left_first].bounds));
            REQUIRE(encloses(node.bounds, nodes[node.left_first + 1U].bounds));
        }
    }
    REQUIRE(total_leaf_prims == prims.size());
    for (usize i = 0; i < prims.size(); ++i)
    {
        REQUIRE(ref_count[i] == 1U); // every primitive referenced exactly once
    }
    // Root bounds enclose every primitive.
    for (const AABB3<f32>& b : prims)
    {
        REQUIRE(encloses(tree.bounds(), b));
    }
}

} // namespace

TEST_CASE("BVH build: empty span yields an empty tree", "[geometry][bvh][build]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(), &alloc);
    REQUIRE(tree.is_empty());
    REQUIRE(tree.node_count() == 0U);
    REQUIRE(tree.prim_count() == 0U);
    REQUIRE(bvh_sah_cost(tree) == 0.0F);
}

TEST_CASE("BVH build: single primitive yields one leaf", "[geometry][bvh][build]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");
    const AABB3<f32> box(Vec3<f32>(-1, -2, -3), Vec3<f32>(4, 5, 6));
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(&box, 1), &alloc);
    REQUIRE(tree.node_count() == 1U);
    REQUIRE(tree.prim_count() == 1U);
    REQUIRE(tree.nodes()[0].is_leaf());
    REQUIRE(tree.nodes()[0].prim_count == 1U);
    REQUIRE(tree.nodes()[0].bounds == box);
    REQUIRE(tree.prim_indices()[0] == 0U);
}

TEST_CASE("BVH build: structural invariants on a random corpus", "[geometry][bvh][build]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0xB1A5ED);
    for (usize trial = 0; trial < 6; ++trial)
    {
        const usize n = 1U + (rng.next() % 700U);
        std::vector<AABB3<f32>> prims;
        prims.reserve(n);
        for (usize i = 0; i < n; ++i)
        {
            prims.push_back(random_box(rng, 100.0F, 3.0F));
        }
        BvhBuildOptions opts;
        opts.max_leaf_prims = static_cast<crd::u16>(1U + (rng.next() % 8U));
        opts.sah_bins = static_cast<u32>(4U + (rng.next() % 24U));
        const BvhTree tree =
            bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc, opts);
        validate_tree(tree, prims, opts.max_leaf_prims);
    }
}

TEST_CASE("BVH build: coincident centroids do not blow the stack and leaves stay capped", "[geometry][bvh][build]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "bvh-test");
    std::vector<AABB3<f32>> prims(1000, AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1)));
    BvhBuildOptions opts;
    opts.max_leaf_prims = 4;
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc, opts);
    validate_tree(tree, prims, 4);
}

TEST_CASE("BVH build: long-thin row stays well-balanced", "[geometry][bvh][build]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "bvh-test");
    std::vector<AABB3<f32>> prims;
    constexpr usize k_n = 256;
    for (usize i = 0; i < k_n; ++i)
    {
        const f32 x = static_cast<f32>(i);
        prims.emplace_back(Vec3<f32>(x - 0.4F, -0.4F, -0.4F), Vec3<f32>(x + 0.4F, 0.4F, 0.4F));
    }
    BvhBuildOptions opts;
    opts.max_leaf_prims = 4;
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc, opts);
    validate_tree(tree, prims, 4);
    // A reasonable BVH over a row keeps total expected leaf work far below a single leaf.
    const f32 cost = bvh_sah_cost(tree);
    REQUIRE(cost > 0.0F);
    REQUIRE(cost < static_cast<f32>(k_n) * 0.25F);
}

TEST_CASE("BVH build: deterministic replay (same input -> bit-identical tree)", "[geometry][bvh][build]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0xDE7E47);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 400; ++i)
    {
        prims.push_back(random_box(rng, 50.0F, 2.0F));
    }
    const auto span = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree a = bvh_build(span, &alloc);
    const BvhTree b = bvh_build(span, &alloc);
    REQUIRE(a.node_count() == b.node_count());
    REQUIRE(a.prim_count() == b.prim_count());
    REQUIRE(a.root() == b.root());
    REQUIRE(std::memcmp(a.nodes().data(), b.nodes().data(), a.nodes().size() * sizeof(BvhNode)) == 0);
    REQUIRE(std::memcmp(a.prim_indices().data(), b.prim_indices().data(), a.prim_indices().size() * sizeof(u32)) == 0);
}

TEST_CASE("BVH build: SAH builder beats a single-leaf tree", "[geometry][bvh][build]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0x5A4C05);
    std::vector<AABB3<f32>> prims;
    constexpr usize k_n = 500;
    for (usize i = 0; i < k_n; ++i)
    {
        prims.push_back(random_box(rng, 100.0F, 1.5F));
    }
    const auto span = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());

    BvhBuildOptions one_leaf;
    one_leaf.max_leaf_prims = 0xFFFF; // forces a single leaf — the "no acceleration" baseline
    const f32 baseline = bvh_sah_cost(bvh_build(span, &alloc, one_leaf));
    REQUIRE(baseline > 0.0F);

    BvhBuildOptions normal;
    normal.max_leaf_prims = 4;
    const f32 built = bvh_sah_cost(bvh_build(span, &alloc, normal));
    REQUIRE(built > 0.0F);
    REQUIRE(built < baseline * 0.25F); // the SAH split should slash expected work by far more than 4x
}

TEST_CASE("BVH build: sah_bins below 2 is clamped, not a crash", "[geometry][bvh][build]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "bvh-test");
    Rng rng(0x11B5);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 200; ++i)
    {
        prims.push_back(random_box(rng, 30.0F, 1.0F));
    }
    BvhBuildOptions opts;
    opts.sah_bins = 0;
    opts.max_leaf_prims = 4;
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc, opts);
    validate_tree(tree, prims, 4);
}
