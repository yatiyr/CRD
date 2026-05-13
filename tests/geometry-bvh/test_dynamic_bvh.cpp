// crd-geometry-bvh v1c — DynamicBvh tests: insert/remove/update + query/raycast
// vs brute force (over fat AABBs), structural validity after random op sequences,
// height/SAH stay bounded, fat-margin no-op behaviour, handle stability.

#include <crd/geometry/bvh/bvh.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::bvh::DynamicBvh;
using crd::geometry::bvh::DynamicBvhConfig;
using crd::geometry::bvh::DynamicBvhNodeId;
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

// A leaf the test tracks: its handle, its tracked user_data (== its index in the vector).
struct TrackedLeaf
{
    DynamicBvhNodeId id;
    bool live{true};
};

std::vector<u32> sorted_set(std::vector<u32> v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

} // namespace

TEST_CASE("DynamicBvh: empty tree and single leaf", "[geometry][bvh][dynamic]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "bvh-test");
    DynamicBvh tree(&alloc);
    REQUIRE(tree.is_empty());
    REQUIRE(tree.leaf_count() == 0U);
    tree.validate();
    bool any = false;
    tree.query(AABB3<f32>(Vec3<f32>(-1e9F, -1e9F, -1e9F), Vec3<f32>(1e9F, 1e9F, 1e9F)), [&](u32) { any = true; });
    REQUIRE_FALSE(any);

    const AABB3<f32> tight(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1));
    const DynamicBvhNodeId id = tree.insert(tight, 42U);
    REQUIRE_FALSE(tree.is_empty());
    REQUIRE(tree.leaf_count() == 1U);
    REQUIRE(tree.user_data(id) == 42U);
    REQUIRE(tree.max_depth() == 0U);
    // The stored fat AABB encloses the tight one.
    const AABB3<f32> fat = tree.fat_aabb(id);
    REQUIRE(fat.min.x <= tight.min.x);
    REQUIRE(fat.max.x >= tight.max.x);
    REQUIRE(tree.bounds() == fat);
    tree.validate();

    tree.remove(id);
    REQUIRE(tree.is_empty());
    REQUIRE(tree.leaf_count() == 0U);
    tree.validate();
}

TEST_CASE("DynamicBvh: query overlap matches brute force over fat AABBs", "[geometry][bvh][dynamic]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0xD7A1C);
    DynamicBvh tree(&alloc, DynamicBvhConfig{0.25F});
    constexpr usize n = 600;
    std::vector<DynamicBvhNodeId> ids;
    for (usize i = 0; i < n; ++i)
    {
        ids.push_back(tree.insert(random_box(rng, 80.0F, 3.0F), static_cast<u32>(i)));
    }
    tree.validate();

    for (usize q = 0; q < 300; ++q)
    {
        const AABB3<f32> box = random_box(rng, 100.0F, 10.0F);
        std::vector<u32> refset;
        for (usize i = 0; i < n; ++i)
        {
            if (intersects(tree.fat_aabb(ids[i]), box))
            {
                refset.push_back(static_cast<u32>(i));
            }
        }
        std::vector<u32> gotset;
        tree.query(box, [&](u32 ud) { gotset.push_back(ud); });
        // (also exercise the Array<u32> overload)
        crd::containers::Array<u32> got_arr(&alloc);
        tree.query(box, got_arr);
        std::vector<u32> got_arr_v(got_arr.data(), got_arr.data() + got_arr.size());
        REQUIRE(sorted_set(gotset) == sorted_set(refset));
        REQUIRE(sorted_set(got_arr_v) == sorted_set(refset));
    }
}

TEST_CASE("DynamicBvh: raycast visits exactly the leaves whose fat AABB the ray hits", "[geometry][bvh][dynamic]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0x7A4CE);
    DynamicBvh tree(&alloc);
    constexpr usize n = 400;
    std::vector<DynamicBvhNodeId> ids;
    for (usize i = 0; i < n; ++i)
    {
        ids.push_back(tree.insert(random_box(rng, 60.0F, 3.0F), static_cast<u32>(i)));
    }
    for (usize r = 0; r < 300; ++r)
    {
        const Ray3<f32> ray{Vec3<f32>(rng.range(-120, 120), rng.range(-120, 120), rng.range(-120, 120)),
                            normalized(Vec3<f32>(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)))};
        std::vector<u32> refset;
        for (usize i = 0; i < n; ++i)
        {
            f32 t = 0.0F;
            if (intersect_ray_aabb_robust(ray, tree.fat_aabb(ids[i]), 0.0F, std::numeric_limits<f32>::infinity(), t))
            {
                refset.push_back(static_cast<u32>(i));
            }
        }
        std::vector<u32> gotset;
        tree.raycast(ray, [&](u32 ud) { gotset.push_back(ud); });
        REQUIRE(sorted_set(gotset) == sorted_set(refset));
    }
}

TEST_CASE("DynamicBvh: update is a no-op inside the margin, reinserts when it escapes", "[geometry][bvh][dynamic]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "bvh-test");
    DynamicBvh tree(&alloc, DynamicBvhConfig{1.0F});
    const DynamicBvhNodeId a = tree.insert(AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(2, 2, 2)), 0U);
    const DynamicBvhNodeId b = tree.insert(AABB3<f32>(Vec3<f32>(50, 50, 50), Vec3<f32>(52, 52, 52)), 1U);
    const AABB3<f32> fat_a_before = tree.fat_aabb(a);

    // Tiny move — new tight box still inside the fat one.
    REQUIRE_FALSE(tree.update(a, AABB3<f32>(Vec3<f32>(0.2F, 0.1F, -0.3F), Vec3<f32>(2.1F, 2.0F, 1.6F))));
    REQUIRE(tree.fat_aabb(a) == fat_a_before);
    tree.validate();

    // Big move — escapes the fat box; the leaf is reinserted, handle stays valid.
    REQUIRE(tree.update(a, AABB3<f32>(Vec3<f32>(100, 100, 100), Vec3<f32>(102, 102, 102))));
    REQUIRE(tree.user_data(a) == 0U);
    REQUIRE(tree.fat_aabb(a).min.x <= 100.0F);
    REQUIRE(tree.fat_aabb(a).max.x >= 102.0F);
    tree.validate();
    // Found at the new location, not the old.
    std::vector<u32> hit_new;
    tree.query(AABB3<f32>(Vec3<f32>(99, 99, 99), Vec3<f32>(103, 103, 103)), [&](u32 ud) { hit_new.push_back(ud); });
    REQUIRE(std::find(hit_new.begin(), hit_new.end(), 0U) != hit_new.end());
    std::vector<u32> hit_old;
    tree.query(AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(3, 3, 3)), [&](u32 ud) { hit_old.push_back(ud); });
    REQUIRE(std::find(hit_old.begin(), hit_old.end(), 0U) == hit_old.end());
    // b untouched.
    REQUIRE(tree.user_data(b) == 1U);
}

TEST_CASE("DynamicBvh: structural validity + query correctness across a random op sequence", "[geometry][bvh][dynamic]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0x09E5E9);
    DynamicBvh tree(&alloc, DynamicBvhConfig{0.5F});
    std::vector<TrackedLeaf> leaves; // index == user_data

    for (usize step = 0; step < 4000; ++step)
    {
        const u32 op = static_cast<u32>(rng.next() % 10U);
        if (op < 5U || leaves.empty()) // insert (also when empty)
        {
            const u32 ud = static_cast<u32>(leaves.size());
            leaves.push_back(TrackedLeaf{tree.insert(random_box(rng, 70.0F, 3.0F), ud), true});
        }
        else
        {
            // pick a random live leaf
            usize tries = 0;
            usize idx = leaves.size();
            while (tries < 8)
            {
                const usize cand = static_cast<usize>(rng.next() % leaves.size());
                if (leaves[cand].live)
                {
                    idx = cand;
                    break;
                }
                ++tries;
            }
            if (idx == leaves.size())
            {
                continue; // unlucky; skip this step
            }
            if (op < 8U) // remove
            {
                tree.remove(leaves[idx].id);
                leaves[idx].live = false;
            }
            else // update
            {
                tree.update(leaves[idx].id, random_box(rng, 70.0F, 3.0F));
            }
        }
        if (step % 250U == 0U)
        {
            tree.validate();
        }
    }
    tree.validate();

    // Final query cross-check vs brute force over the live leaves' fat AABBs.
    usize live = 0;
    for (const TrackedLeaf& l : leaves)
    {
        if (l.live)
        {
            ++live;
        }
    }
    REQUIRE(tree.leaf_count() == live);
    for (usize q = 0; q < 200; ++q)
    {
        const AABB3<f32> box = random_box(rng, 90.0F, 10.0F);
        std::vector<u32> refset;
        for (usize i = 0; i < leaves.size(); ++i)
        {
            if (leaves[i].live && intersects(tree.fat_aabb(leaves[i].id), box))
            {
                refset.push_back(static_cast<u32>(i));
            }
        }
        std::vector<u32> gotset;
        tree.query(box, [&](u32 ud) { gotset.push_back(ud); });
        REQUIRE(sorted_set(gotset) == sorted_set(refset));
    }
}

TEST_CASE("DynamicBvh: stays balanced -- depth and SAH cost bounded", "[geometry][bvh][dynamic]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0xBA1A4CE);
    DynamicBvh tree(&alloc);
    constexpr usize n = 2000;
    for (usize i = 0; i < n; ++i)
    {
        (void)tree.insert(random_box(rng, 100.0F, 2.0F), static_cast<u32>(i));
    }
    tree.validate();
    // Height-balanced tree rotations keep the depth near log2(n); allow generous slack.
    usize log2n = 0;
    while ((usize{1} << (log2n + 1)) <= n)
    {
        ++log2n;
    }
    REQUIRE(tree.max_depth() <= 3U * log2n + 12U);
    const f32 cost = tree.sah_cost();
    REQUIRE(cost > 0.0F);
    REQUIRE(std::isfinite(cost));
}

TEST_CASE("DynamicBvh: handles stay valid after operations on other leaves", "[geometry][bvh][dynamic]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "bvh-test");
    DynamicBvh tree(&alloc, DynamicBvhConfig{0.1F});
    const DynamicBvhNodeId a = tree.insert(AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1)), 100U);
    const DynamicBvhNodeId b = tree.insert(AABB3<f32>(Vec3<f32>(10, 0, 0), Vec3<f32>(11, 1, 1)), 200U);
    const DynamicBvhNodeId c = tree.insert(AABB3<f32>(Vec3<f32>(20, 0, 0), Vec3<f32>(21, 1, 1)), 300U);
    tree.remove(b);
    REQUIRE(tree.update(a, AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(5, 5, 5))));
    REQUIRE(tree.user_data(a) == 100U);
    REQUIRE(tree.user_data(c) == 300U);
    std::vector<u32> hit;
    tree.query(AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(25, 25, 25)), [&](u32 ud) { hit.push_back(ud); });
    REQUIRE(std::find(hit.begin(), hit.end(), 100U) != hit.end());
    REQUIRE(std::find(hit.begin(), hit.end(), 300U) != hit.end());
    REQUIRE(std::find(hit.begin(), hit.end(), 200U) == hit.end());
    tree.validate();
}
