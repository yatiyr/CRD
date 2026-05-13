// crd-geometry v1i-c — find_overlapping_pairs(DynamicBvh) tests.
//
//   * Empty / single-leaf → no pairs.
//   * find_overlapping_pairs matches a brute-force O(n²) reference on random
//     corpora of varying density; pairs reported with `(min, max)` user_data
//     order; the BVH and brute-force pair-sets agree as sorted sets.
//   * Determinism: two invocations on the same tree state produce
//     bit-identical pair sequences.
//   * After insert / remove / update, the pair set tracks the new fat-AABB
//     state.
//   * Callback and Array forms produce the same set.

#include <crd/geometry/bvh/bvh.hpp>
#include <crd/geometry/queries.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <vector>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::bvh::DynamicBvh;
using crd::geometry::bvh::DynamicBvhPair;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::intersects;
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

// Brute-force O(n²) reference: every (i<j) leaf pair whose fat AABBs overlap.
// `fats` provides the *fat* AABBs (`DynamicBvh` inflates tight AABBs by
// `fat_margin` on insert; the BVH self-overlap is over the fat AABBs).
std::vector<DynamicBvhPair> brute(const std::vector<AABB3<f32>>& fats, const std::vector<u32>& user_data)
{
    std::vector<DynamicBvhPair> pairs;
    for (usize i = 0; i < fats.size(); ++i)
    {
        for (usize j = i + 1; j < fats.size(); ++j)
        {
            if (intersects(fats[i], fats[j]))
            {
                const u32 a = user_data[i] < user_data[j] ? user_data[i] : user_data[j];
                const u32 b = user_data[i] < user_data[j] ? user_data[j] : user_data[i];
                pairs.push_back(DynamicBvhPair{a, b});
            }
        }
    }
    std::sort(pairs.begin(), pairs.end());
    return pairs;
}

} // namespace

TEST_CASE("find_overlapping_pairs: empty tree emits no pairs", "[geometry][bvh][pairs]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "pairs-test");
    const DynamicBvh dt(&alloc);
    crd::containers::Array<DynamicBvhPair> out(&alloc);
    crd::geometry::find_overlapping_pairs(dt, out);
    REQUIRE(out.size() == 0U);
}

TEST_CASE("find_overlapping_pairs: single leaf emits no pairs", "[geometry][bvh][pairs]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "pairs-test");
    DynamicBvh dt(&alloc);
    (void)dt.insert(AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1)), 42U);
    crd::containers::Array<DynamicBvhPair> out(&alloc);
    crd::geometry::find_overlapping_pairs(dt, out);
    REQUIRE(out.size() == 0U);
}

TEST_CASE("find_overlapping_pairs: hand-built scene matches expectation", "[geometry][bvh][pairs]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "pairs-test");
    DynamicBvh dt(&alloc);
    // Three boxes, two of which overlap (A and B), the third (C) is far.
    (void)dt.insert(AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1)), 10U);  // A
    (void)dt.insert(AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(2, 2, 2)), 20U);    // B  (overlaps A)
    (void)dt.insert(AABB3<f32>(Vec3<f32>(50, 50, 50), Vec3<f32>(52, 52, 52)), 30U); // C  (isolated)
    crd::containers::Array<DynamicBvhPair> out(&alloc);
    crd::geometry::find_overlapping_pairs(dt, out);
    REQUIRE(out.size() == 1U);
    REQUIRE(out[0].a == 10U);
    REQUIRE(out[0].b == 20U);
}

TEST_CASE("find_overlapping_pairs: matches brute force on random corpus", "[geometry][bvh][pairs]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "pairs-test");
    Rng rng(0xBAD5EED);
    for (usize trial = 0; trial < 5; ++trial)
    {
        // World size varies → pair density varies (small world ⇒ many overlaps;
        // large world ⇒ sparse). Cover both regimes.
        const f32 world = 10.0F + rng.range(0.0F, 40.0F);
        const usize n = 30U + (rng.next() % 220U); // 30..250 leaves
        DynamicBvh dt(&alloc);
        std::vector<AABB3<f32>> fats;
        std::vector<u32> ud;
        for (usize i = 0; i < n; ++i)
        {
            const AABB3<f32> tight = random_box(rng, world, 3.0F);
            const u32 user_data = static_cast<u32>(i + 1U);
            const auto id = dt.insert(tight, user_data);
            fats.push_back(dt.fat_aabb(id)); // fat = tight inflated by cfg.fat_margin
            ud.push_back(user_data);
        }

        crd::containers::Array<DynamicBvhPair> got(&alloc);
        crd::geometry::find_overlapping_pairs(dt, got);
        std::vector<DynamicBvhPair> got_v;
        for (usize i = 0; i < got.size(); ++i)
        {
            got_v.push_back(got[i]);
        }
        std::sort(got_v.begin(), got_v.end());
        const std::vector<DynamicBvhPair> ref = brute(fats, ud);
        REQUIRE(got_v == ref);
        // Each emitted pair must be in `(min, max)` order.
        for (const DynamicBvhPair& p : got_v)
        {
            REQUIRE(p.a < p.b);
        }
    }
}

TEST_CASE("find_overlapping_pairs: callback form matches Array form", "[geometry][bvh][pairs]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "pairs-test");
    Rng rng(0xCB1);
    DynamicBvh dt(&alloc);
    for (usize i = 0; i < 100; ++i)
    {
        (void)dt.insert(random_box(rng, 15.0F, 3.0F), static_cast<u32>(i + 1U));
    }
    crd::containers::Array<DynamicBvhPair> via_array(&alloc);
    crd::geometry::find_overlapping_pairs(dt, via_array);
    std::vector<DynamicBvhPair> via_callback;
    crd::geometry::find_overlapping_pairs(dt, [&via_callback](u32 a, u32 b) {
        via_callback.push_back(DynamicBvhPair{a, b});
    });
    REQUIRE(via_array.size() == via_callback.size());
    for (usize i = 0; i < via_array.size(); ++i)
    {
        REQUIRE(via_array[i] == via_callback[i]);
    }
}

TEST_CASE("find_overlapping_pairs: deterministic across re-invocations", "[geometry][bvh][pairs]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "pairs-test");
    Rng rng(0xDD);
    DynamicBvh dt(&alloc);
    for (usize i = 0; i < 80; ++i)
    {
        (void)dt.insert(random_box(rng, 12.0F, 2.5F), static_cast<u32>(i + 1U));
    }
    crd::containers::Array<DynamicBvhPair> a(&alloc);
    crd::geometry::find_overlapping_pairs(dt, a);
    crd::containers::Array<DynamicBvhPair> b(&alloc);
    crd::geometry::find_overlapping_pairs(dt, b);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i)
    {
        REQUIRE(a[i] == b[i]);
    }
}

TEST_CASE("find_overlapping_pairs: n=10000 dense corpus does not exhaust the allocator",
          "[geometry][bvh][pairs]")
{
    // Eylem v1c broadphase will hit this with O(10⁴+) bodies in deep trees;
    // the cross-stack `Array<CrossWork>` grows up to 4× per pop in dense
    // overlap regimes. This test is a soak — n=10000 in a small world so
    // overlaps are dense — that surfaces a TLSF OOM if the work stacks grow
    // pathologically (advisor flag #2 on v1i-c). It also cross-checks the
    // count against brute force at this scale to make sure the algorithm
    // doesn't quietly drop pairs at high n.
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 26, nullptr, "pairs-soak"); // 64 MB headroom
    Rng rng(0x5EEDED);
    const usize n = 10000;
    DynamicBvh dt(&alloc);
    std::vector<AABB3<f32>> fats;
    std::vector<u32> ud;
    fats.reserve(n);
    ud.reserve(n);
    for (usize i = 0; i < n; ++i)
    {
        // Small world (size 20) + small boxes ⇒ pairs in the low-thousands range.
        const AABB3<f32> tight = random_box(rng, 20.0F, 1.5F);
        const u32 user_data = static_cast<u32>(i + 1U);
        const auto id = dt.insert(tight, user_data);
        fats.push_back(dt.fat_aabb(id));
        ud.push_back(user_data);
    }
    crd::containers::Array<DynamicBvhPair> got(&alloc);
    crd::geometry::find_overlapping_pairs(dt, got);

    // Full O(n²) brute-force cross-check at n=10000 — 50M pair tests; runs
    // in a few seconds. The full check (not a sampling shortcut) is the
    // right test here because a sampling check could miss the BVH silently
    // dropping a fraction of pairs at scale, which is exactly the class of
    // bug this soak is designed to catch.
    const std::vector<DynamicBvhPair> ref = brute(fats, ud);
    REQUIRE(got.size() == ref.size());
    // Spot-check a sorted slice matches.
    std::vector<DynamicBvhPair> got_v(got.data(), got.data() + got.size());
    std::sort(got_v.begin(), got_v.end());
    REQUIRE(got_v == ref);
}

TEST_CASE("find_overlapping_pairs: caller-owned scratch reuse matches alloc-per-call",
          "[geometry][bvh][pairs]")
{
    // Eylem v1c's broadphase will hit find_overlapping_pairs every physics
    // tick. The scratch-taking overload reuses caller-owned work stacks
    // (`scratch.walk` / `scratch.cross`) and amortises capacity growth across
    // calls. Cross-check: the scratch path produces bit-identical results to
    // the alloc-per-call path, even after sequential reuse with mutation
    // between calls.
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "scratch-test");
    Rng rng(0x5C7A7C8);
    DynamicBvh dt(&alloc);
    std::vector<crd::geometry::bvh::DynamicBvhNodeId> ids;
    for (usize i = 0; i < 150; ++i)
    {
        ids.push_back(dt.insert(random_box(rng, 12.0F, 2.0F), static_cast<u32>(i + 1U)));
    }

    crd::geometry::bvh::DynamicBvhPairScratch scratch(&alloc);

    auto sorted_pairs = [&](crd::containers::Array<DynamicBvhPair>& a) {
        std::vector<DynamicBvhPair> v(a.data(), a.data() + a.size());
        std::sort(v.begin(), v.end());
        return v;
    };

    for (usize iter = 0; iter < 5; ++iter)
    {
        crd::containers::Array<DynamicBvhPair> via_alloc(&alloc);
        crd::geometry::find_overlapping_pairs(dt, via_alloc);

        crd::containers::Array<DynamicBvhPair> via_scratch(&alloc);
        crd::geometry::find_overlapping_pairs(dt, via_scratch, scratch);

        REQUIRE(sorted_pairs(via_alloc) == sorted_pairs(via_scratch));

        // Mutate the tree before the next iter so the scratch sees varying
        // sizes — a real broadphase consumer will do exactly this.
        const usize victim = (rng.next() % ids.size());
        dt.update(ids[victim], random_box(rng, 12.0F, 2.0F));
    }

    // After reuse, the scratch retains capacity for next time but is
    // logically empty (its size is reset by the next call's `clear()`).
    crd::containers::Array<DynamicBvhPair> final_run(&alloc);
    crd::geometry::find_overlapping_pairs(dt, final_run, scratch);
    crd::containers::Array<DynamicBvhPair> final_ref(&alloc);
    crd::geometry::find_overlapping_pairs(dt, final_ref);
    REQUIRE(sorted_pairs(final_run) == sorted_pairs(final_ref));
}

TEST_CASE("find_overlapping_pairs: tracks insert / remove / update", "[geometry][bvh][pairs]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "pairs-test");
    DynamicBvh dt(&alloc);
    const auto a = dt.insert(AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1)), 10U);
    const auto b = dt.insert(AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(2, 2, 2)), 20U);   // overlaps A
    const auto c = dt.insert(AABB3<f32>(Vec3<f32>(50, 0, 0), Vec3<f32>(52, 2, 2)), 30U); // isolated

    crd::containers::Array<DynamicBvhPair> out(&alloc);
    crd::geometry::find_overlapping_pairs(dt, out);
    REQUIRE(out.size() == 1U);
    REQUIRE(out[0] == DynamicBvhPair{10U, 20U});

    // Move B away — now no pair.
    (void)dt.update(b, AABB3<f32>(Vec3<f32>(100, 100, 100), Vec3<f32>(102, 102, 102)));
    out.clear();
    crd::geometry::find_overlapping_pairs(dt, out);
    REQUIRE(out.size() == 0U);

    // Move C onto A's fat AABB.
    (void)dt.update(c, AABB3<f32>(Vec3<f32>(-0.5F, -0.5F, -0.5F), Vec3<f32>(0.5F, 0.5F, 0.5F)));
    out.clear();
    crd::geometry::find_overlapping_pairs(dt, out);
    REQUIRE(out.size() == 1U);
    REQUIRE(out[0] == DynamicBvhPair{10U, 30U});

    // Remove A — no pairs left.
    dt.remove(a);
    out.clear();
    crd::geometry::find_overlapping_pairs(dt, out);
    REQUIRE(out.size() == 0U);
}
