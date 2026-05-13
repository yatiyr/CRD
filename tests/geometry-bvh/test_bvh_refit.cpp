// crd-geometry-bvh v1b — bvh_refit tests: idempotent on a static set, bounds
// correctness + query correctness after moving primitives, topology untouched.

#include <crd/geometry/bvh/bvh.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
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
using crd::geometry::bvh::bvh_refit;
using crd::geometry::bvh::BvhBuildOptions;
using crd::geometry::bvh::BvhNode;
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

AABB3<f32> translated(const AABB3<f32>& b, const Vec3<f32>& d)
{
    return AABB3<f32>(Vec3<f32>(b.min.x + d.x, b.min.y + d.y, b.min.z + d.z),
                      Vec3<f32>(b.max.x + d.x, b.max.y + d.y, b.max.z + d.z));
}

AABB3<f32> exact_union(const std::vector<AABB3<f32>>& prims, const u32* idx, u32 first, u32 count)
{
    AABB3<f32> b(Vec3<f32>(1e30F, 1e30F, 1e30F), Vec3<f32>(-1e30F, -1e30F, -1e30F));
    for (u32 i = first; i < first + count; ++i)
    {
        const AABB3<f32>& p = prims[idx[i]];
        b.min.x = std::min(b.min.x, p.min.x);
        b.min.y = std::min(b.min.y, p.min.y);
        b.min.z = std::min(b.min.z, p.min.z);
        b.max.x = std::max(b.max.x, p.max.x);
        b.max.y = std::max(b.max.y, p.max.y);
        b.max.z = std::max(b.max.z, p.max.z);
    }
    return b;
}

bool encloses(const AABB3<f32>& outer, const AABB3<f32>& inner) noexcept
{
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && outer.min.z <= inner.min.z &&
           outer.max.x >= inner.max.x && outer.max.y >= inner.max.y && outer.max.z >= inner.max.z;
}

Vec3<f32> normalized(const Vec3<f32>& v)
{
    const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return Vec3<f32>(v.x / len, v.y / len, v.z / len);
}

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
    return BvhRayHit{best_p, best_t};
}

// Validates that every node's bounds match the exact union it should hold, given `prims`.
void check_bounds_consistent(const BvhTree& tree, const std::vector<AABB3<f32>>& prims)
{
    const auto nodes = tree.nodes();
    const auto idx = tree.prim_indices();
    for (usize ni = nodes.size(); ni-- > 0;)
    {
        const BvhNode& node = nodes[ni];
        if (node.is_leaf())
        {
            REQUIRE(node.bounds == exact_union(prims, idx.data(), node.left_first, node.prim_count));
        }
        else
        {
            AABB3<f32> u = nodes[node.left_first].bounds;
            const AABB3<f32>& r = nodes[node.left_first + 1U].bounds;
            u.min.x = std::min(u.min.x, r.min.x);
            u.min.y = std::min(u.min.y, r.min.y);
            u.min.z = std::min(u.min.z, r.min.z);
            u.max.x = std::max(u.max.x, r.max.x);
            u.max.y = std::max(u.max.y, r.max.y);
            u.max.z = std::max(u.max.z, r.max.z);
            REQUIRE(node.bounds == u);
        }
    }
    for (const AABB3<f32>& b : prims)
    {
        REQUIRE(encloses(tree.bounds(), b));
    }
}

} // namespace

TEST_CASE("BVH refit: same prims leaves the tree byte-identical", "[geometry][bvh][refit]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0x12F17);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 400; ++i)
    {
        prims.push_back(random_box(rng, 50.0F, 2.0F));
    }
    const auto span = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree a = bvh_build(span, &alloc);
    BvhTree b = bvh_build(span, &alloc);
    bvh_refit(b, span);
    REQUIRE(a.node_count() == b.node_count());
    REQUIRE(std::memcmp(a.nodes().data(), b.nodes().data(), a.nodes().size() * sizeof(BvhNode)) == 0);
    REQUIRE(std::memcmp(a.prim_indices().data(), b.prim_indices().data(), a.prim_indices().size() * sizeof(u32)) == 0);
}

TEST_CASE("BVH refit: bounds and queries stay correct after moving primitives", "[geometry][bvh][refit]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0x720FE7);
    for (usize trial = 0; trial < 4; ++trial)
    {
        const usize n = 50U + (rng.next() % 500U);
        std::vector<AABB3<f32>> prims;
        for (usize i = 0; i < n; ++i)
        {
            prims.push_back(random_box(rng, 60.0F, 3.0F));
        }
        BvhBuildOptions opts;
        opts.max_leaf_prims = static_cast<crd::u16>(1U + (rng.next() % 6U));
        BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc, opts);

        // Move every box — some by a little, some far enough to leave their old leaf entirely.
        std::vector<AABB3<f32>> moved;
        moved.reserve(n);
        for (usize i = 0; i < n; ++i)
        {
            const f32 mag = (i % 7U == 0U) ? 80.0F : 1.5F;
            moved.push_back(
                translated(prims[i], Vec3<f32>(rng.range(-mag, mag), rng.range(-mag, mag), rng.range(-mag, mag))));
        }
        const auto mspan = crd::containers::ConstSpan<AABB3<f32>>(moved.data(), moved.size());
        bvh_refit(tree, mspan);
        check_bounds_consistent(tree, moved);

        for (usize r = 0; r < 250; ++r)
        {
            const Ray3<f32> ray{Vec3<f32>(rng.range(-150, 150), rng.range(-150, 150), rng.range(-150, 150)),
                                normalized(Vec3<f32>(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)))};
            const std::optional<BvhRayHit> got = bvh_raycast(tree, mspan, ray);
            const std::optional<BvhRayHit> ref = brute_raycast(moved, ray, std::numeric_limits<f32>::infinity());
            REQUIRE(got.has_value() == ref.has_value());
            if (got)
            {
                REQUIRE(got->t == ref->t);
            }
        }
        for (usize q = 0; q < 150; ++q)
        {
            const AABB3<f32> box = random_box(rng, 100.0F, 8.0F);
            std::vector<u32> refset;
            for (usize i = 0; i < n; ++i)
            {
                if (intersects(moved[i], box))
                {
                    refset.push_back(static_cast<u32>(i));
                }
            }
            std::vector<u32> gotset;
            bvh_overlap(tree, mspan, box, [&](u32 p) { gotset.push_back(p); });
            std::sort(refset.begin(), refset.end());
            std::sort(gotset.begin(), gotset.end());
            REQUIRE(gotset == refset);
        }
    }
}

TEST_CASE("BVH refit: topology is untouched (only bounds change)", "[geometry][bvh][refit]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0x70905);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 300; ++i)
    {
        prims.push_back(random_box(rng, 40.0F, 2.0F));
    }
    BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc);

    const usize node_count = tree.node_count();
    const u32 root = tree.root();
    std::vector<u32> idx_before(tree.prim_indices().data(), tree.prim_indices().data() + tree.prim_indices().size());
    struct TopoBit
    {
        u32 left_first;
        crd::u16 prim_count;
        crd::u8 split_axis;
    };
    std::vector<TopoBit> topo_before;
    for (const BvhNode& n : tree.nodes())
    {
        topo_before.push_back(TopoBit{n.left_first, n.prim_count, n.split_axis});
    }

    std::vector<AABB3<f32>> moved;
    for (usize i = 0; i < prims.size(); ++i)
    {
        moved.push_back(translated(prims[i], Vec3<f32>(rng.range(-5, 5), rng.range(-5, 5), rng.range(-5, 5))));
    }
    bvh_refit(tree, crd::containers::ConstSpan<AABB3<f32>>(moved.data(), moved.size()));

    REQUIRE(tree.node_count() == node_count);
    REQUIRE(tree.root() == root);
    std::vector<u32> idx_after(tree.prim_indices().data(), tree.prim_indices().data() + tree.prim_indices().size());
    REQUIRE(idx_after == idx_before);
    const auto nodes = tree.nodes();
    for (usize i = 0; i < nodes.size(); ++i)
    {
        REQUIRE(nodes[i].left_first == topo_before[i].left_first);
        REQUIRE(nodes[i].prim_count == topo_before[i].prim_count);
        REQUIRE(nodes[i].split_axis == topo_before[i].split_axis);
    }
}

TEST_CASE("BVH refit: empty tree is a no-op; single-leaf recomputes its bounds", "[geometry][bvh][refit]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "bvh-test");

    BvhTree empty = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(), &alloc);
    bvh_refit(empty, crd::containers::ConstSpan<AABB3<f32>>());
    REQUIRE(empty.is_empty());

    const AABB3<f32> box0(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1));
    BvhTree one = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(&box0, 1), &alloc);
    const AABB3<f32> box1(Vec3<f32>(10, 20, 30), Vec3<f32>(11, 22, 33));
    bvh_refit(one, crd::containers::ConstSpan<AABB3<f32>>(&box1, 1));
    REQUIRE(one.node_count() == 1U);
    REQUIRE(one.nodes()[0].is_leaf());
    REQUIRE(one.nodes()[0].bounds == box1);
}
