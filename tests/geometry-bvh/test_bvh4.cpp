// crd-geometry-bvh v1d — Bvh4Tree (quad-BVH collapse) tests: structural
// invariants, queries match the binary tree they were collapsed from,
// deterministic collapse, edge cases.

#include <crd/geometry/bvh/bvh.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/math/simd/vec4f.hpp>
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
using crd::geometry::bvh::bvh4_collapse;
using crd::geometry::bvh::bvh4_overlap;
using crd::geometry::bvh::bvh4_raycast;
using crd::geometry::bvh::Bvh4Child;
using crd::geometry::bvh::Bvh4Node;
using crd::geometry::bvh::Bvh4Tree;
using crd::geometry::bvh::bvh_build;
using crd::geometry::bvh::bvh_overlap;
using crd::geometry::bvh::bvh_raycast;
using crd::geometry::bvh::BvhBuildOptions;
using crd::geometry::bvh::BvhRayHit;
using crd::geometry::bvh::BvhTree;
using crd::geometry::bvh::Ray4AabbResult;
using crd::geometry::bvh::ray_vs_4_aabb;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::intersect_ray_aabb_robust;
using crd::geometry::primitives::intersects;
using crd::geometry::primitives::precompute_ray_aabb;
using crd::geometry::primitives::Ray3;
using crd::math::Vec3;
using crd::math::simd::Vec4f;

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

bool encloses(const AABB3<f32>& outer, const AABB3<f32>& inner) noexcept
{
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && outer.min.z <= inner.min.z &&
           outer.max.x >= inner.max.x && outer.max.y >= inner.max.y && outer.max.z >= inner.max.z;
}

AABB3<f32> union_of(const AABB3<f32>& a, const AABB3<f32>& b) noexcept
{
    return AABB3<f32>(Vec3<f32>(std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)),
                      Vec3<f32>(std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)));
}

void validate_bvh4(const Bvh4Tree& tree, usize prim_count, usize binary_node_count)
{
    REQUIRE(tree.prim_count() == prim_count);
    if (prim_count == 0)
    {
        REQUIRE(tree.is_empty());
        return;
    }
    const auto nodes = tree.nodes();
    const auto idx = tree.prim_indices();
    REQUIRE(nodes.size() >= 1U);
    REQUIRE(nodes.size() <= binary_node_count); // the collapse never makes more nodes than the binary tree
    REQUIRE(tree.root() == 0U);

    std::vector<u32> ref_count(prim_count, 0);
    usize leaf_prims_total = 0;
    for (usize ni = 0; ni < nodes.size(); ++ni)
    {
        const Bvh4Node& n = nodes[ni];
        // child_count is 2..4 normally; 1 only for the synthetic single-leaf root.
        if (nodes.size() == 1U && n.child_count == 1U)
        {
            REQUIRE(n.children[0].is_leaf());
        }
        else
        {
            REQUIRE(n.child_count >= 2U);
        }
        REQUIRE(n.child_count <= 4U);
        AABB3<f32> u(Vec3<f32>(1e30F, 1e30F, 1e30F), Vec3<f32>(-1e30F, -1e30F, -1e30F));
        for (crd::u8 c = 0; c < n.child_count; ++c)
        {
            const Bvh4Child& ch = n.children[c];
            u = union_of(u, ch.bounds);
            if (ch.is_leaf())
            {
                REQUIRE(ch.count >= 1U);
                for (u32 k = ch.first; k < ch.first + ch.count; ++k)
                {
                    REQUIRE(k < idx.size());
                    const u32 p = idx[k];
                    REQUIRE(p < prim_count);
                    ++ref_count[p];
                }
                leaf_prims_total += ch.count;
            }
            else
            {
                REQUIRE(ch.first < nodes.size());
                REQUIRE(ch.first > ni); // children always created after their parent
                // The child-ref's bounds is the exact bounds of the node it points at.
                REQUIRE(ch.bounds == nodes[ch.first].bounds);
            }
        }
        REQUIRE(n.bounds == u);
    }
    REQUIRE(leaf_prims_total == prim_count);
    for (usize i = 0; i < prim_count; ++i)
    {
        REQUIRE(ref_count[i] == 1U);
    }
}

} // namespace

TEST_CASE("BVH4 collapse: structural invariants on random corpora", "[geometry][bvh][bvh4]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0xB44C0);
    for (usize trial = 0; trial < 6; ++trial)
    {
        const usize n = 1U + (rng.next() % 700U);
        std::vector<AABB3<f32>> prims;
        for (usize i = 0; i < n; ++i)
        {
            prims.push_back(random_box(rng, 100.0F, 3.0F));
        }
        BvhBuildOptions opts;
        opts.max_leaf_prims = static_cast<crd::u16>(1U + (rng.next() % 8U));
        const BvhTree binary =
            bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc, opts);
        const Bvh4Tree quad = bvh4_collapse(binary, &alloc);
        validate_bvh4(quad, n, binary.node_count());
        // Root bounds enclose every primitive.
        for (const AABB3<f32>& b : prims)
        {
            REQUIRE(encloses(quad.bounds(), b));
        }
    }
}

TEST_CASE("BVH4 raycast matches the binary tree it was collapsed from", "[geometry][bvh][bvh4]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0x4A4C57);
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
        const BvhTree binary = bvh_build(pspan, &alloc, opts);
        const Bvh4Tree quad = bvh4_collapse(binary, &alloc);

        for (usize r = 0; r < 400; ++r)
        {
            const Ray3<f32> ray{Vec3<f32>(rng.range(-120, 120), rng.range(-120, 120), rng.range(-120, 120)),
                                normalized(Vec3<f32>(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)))};
            const f32 tmax = (r % 5U == 0U) ? rng.range(10.0F, 60.0F) : std::numeric_limits<f32>::infinity();
            const std::optional<BvhRayHit> q4 = bvh4_raycast(quad, pspan, ray, tmax);
            const std::optional<BvhRayHit> q2 = bvh_raycast(binary, pspan, ray, tmax);
            REQUIRE(q4.has_value() == q2.has_value());
            if (q4)
            {
                REQUIRE(q4->t == q2->t);
                f32 check = 0.0F;
                REQUIRE(intersect_ray_aabb_robust(ray, prims[q4->prim_index], 0.0F,
                                                  std::numeric_limits<f32>::infinity(), check));
                REQUIRE(check == q4->t);
            }
        }
    }
}

TEST_CASE("BVH4 overlap matches the binary tree it was collapsed from", "[geometry][bvh][bvh4]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0x04E41A);
    for (usize trial = 0; trial < 4; ++trial)
    {
        const usize n = 30U + (rng.next() % 500U);
        std::vector<AABB3<f32>> prims;
        for (usize i = 0; i < n; ++i)
        {
            prims.push_back(random_box(rng, 60.0F, 3.0F));
        }
        const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
        const BvhTree binary = bvh_build(pspan, &alloc);
        const Bvh4Tree quad = bvh4_collapse(binary, &alloc);

        for (usize q = 0; q < 250; ++q)
        {
            const AABB3<f32> box = random_box(rng, 80.0F, 10.0F);
            std::vector<u32> set2;
            bvh_overlap(binary, pspan, box, [&](u32 p) { set2.push_back(p); });
            std::vector<u32> set4;
            bvh4_overlap(quad, pspan, box, [&](u32 p) { set4.push_back(p); });
            crd::containers::Array<u32> arr4(&alloc);
            bvh4_overlap(quad, pspan, box, arr4);
            std::vector<u32> arr4v(arr4.data(), arr4.data() + arr4.size());
            std::sort(set2.begin(), set2.end());
            std::sort(set4.begin(), set4.end());
            std::sort(arr4v.begin(), arr4v.end());
            REQUIRE(set4 == set2);
            REQUIRE(arr4v == set2);
        }
    }
}

TEST_CASE("BVH4 collapse: deterministic (same binary tree -> bit-identical quad tree)", "[geometry][bvh][bvh4]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0xDE7E44);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 400; ++i)
    {
        prims.push_back(random_box(rng, 50.0F, 2.0F));
    }
    const BvhTree binary = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size()), &alloc);
    const Bvh4Tree a = bvh4_collapse(binary, &alloc);
    const Bvh4Tree b = bvh4_collapse(binary, &alloc);
    REQUIRE(a.node_count() == b.node_count());
    REQUIRE(a.prim_count() == b.prim_count());
    REQUIRE(a.root() == b.root());
    REQUIRE(std::memcmp(a.nodes().data(), b.nodes().data(), a.nodes().size() * sizeof(Bvh4Node)) == 0);
    REQUIRE(std::memcmp(a.prim_indices().data(), b.prim_indices().data(), a.prim_indices().size() * sizeof(u32)) == 0);
}

TEST_CASE("BVH4 collapse: empty, single-leaf, two-prim trees", "[geometry][bvh][bvh4]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "bvh-test");

    const Bvh4Tree empty = bvh4_collapse(bvh_build(crd::containers::ConstSpan<AABB3<f32>>(), &alloc), &alloc);
    REQUIRE(empty.is_empty());
    REQUIRE(empty.node_count() == 0U);

    const AABB3<f32> box0(Vec3<f32>(-1, -2, -3), Vec3<f32>(4, 5, 6));
    const Bvh4Tree one = bvh4_collapse(bvh_build(crd::containers::ConstSpan<AABB3<f32>>(&box0, 1), &alloc), &alloc);
    REQUIRE(one.node_count() == 1U);
    REQUIRE(one.nodes()[0].child_count == 1U);
    REQUIRE(one.nodes()[0].children[0].is_leaf());
    REQUIRE(one.nodes()[0].children[0].count == 1U);
    REQUIRE(one.bounds() == box0);

    std::vector<AABB3<f32>> two = {AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1)),
                                   AABB3<f32>(Vec3<f32>(10, 0, 0), Vec3<f32>(11, 1, 1))};
    BvhBuildOptions leaf1;
    leaf1.max_leaf_prims = 1; // forces two leaves under one interior node
    const BvhTree two_bin = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(two.data(), two.size()), &alloc, leaf1);
    const Bvh4Tree two_quad = bvh4_collapse(two_bin, &alloc);
    REQUIRE(two_quad.node_count() == 1U);
    REQUIRE(two_quad.nodes()[0].child_count == 2U);
    validate_bvh4(two_quad, 2, two_bin.node_count());
}

TEST_CASE("BVH4: ray_vs_4_aabb (Vec4f kernel) lane-by-lane vs the scalar robust slab", "[geometry][bvh][bvh4][simd]")
{
    Rng rng(0x4144CE);
    for (usize trial = 0; trial < 2000; ++trial)
    {
        AABB3<f32> boxes[4];
        f32 minx[4];
        f32 miny[4];
        f32 minz[4];
        f32 maxx[4];
        f32 maxy[4];
        f32 maxz[4];
        for (int c = 0; c < 4; ++c)
        {
            boxes[c] = random_box(rng, 50.0F, 4.0F);
            minx[c] = boxes[c].min.x;
            miny[c] = boxes[c].min.y;
            minz[c] = boxes[c].min.z;
            maxx[c] = boxes[c].max.x;
            maxy[c] = boxes[c].max.y;
            maxz[c] = boxes[c].max.z;
        }
        const Ray3<f32> ray{Vec3<f32>(rng.range(-80, 80), rng.range(-80, 80), rng.range(-80, 80)),
                            normalized(Vec3<f32>(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)))};
        const auto pre = precompute_ray_aabb(ray);
        const f32 tmax = (trial % 4U == 0U) ? rng.range(5.0F, 40.0F) : std::numeric_limits<f32>::infinity();

        const Ray4AabbResult r = ray_vs_4_aabb(ray, pre, Vec4f::load(minx), Vec4f::load(miny), Vec4f::load(minz),
                                               Vec4f::load(maxx), Vec4f::load(maxy), Vec4f::load(maxz), 0.0F, tmax);
        f32 h4[4];
        f32 t4[4];
        r.hit_mask.store(h4);
        r.t_enter.store(t4);
        for (int c = 0; c < 4; ++c)
        {
            f32 ts = 0.0F;
            const bool hs = intersect_ray_aabb_robust(ray, pre, boxes[c], 0.0F, tmax, ts);
            REQUIRE((h4[c] != 0.0F) == hs);
            if (hs)
            {
                REQUIRE(t4[c] == ts);
            }
        }
    }
}
