// crd-geometry v1i-a — unified query facade tests.
//
// Each facade overload (`crd::geometry::raycast` / `overlap` / `closest_point`)
// is verified against the per-backend function it forwards to: `bvh_raycast`,
// `bvh4_raycast`, `bvh_overlap`, `bvh4_overlap`, `DynamicBvh::query`,
// `bvh_closest_point`, `bvh4_closest_point`, `DynamicBvh::closest_point`. We
// also cross-check that BvhTree and Bvh4Tree facade calls agree (the collapse
// changes only fan-out — same nearest hit / same overlap set / same closest
// prim with bit-identical distance²). The new `bvh4_closest_point` and
// `DynamicBvh::closest_point` get dedicated correctness coverage against the
// brute-force reference too.

#include <crd/geometry/bvh/bvh.hpp> // umbrella — bvh_build / BvhBuildOptions / bvh4_collapse / DynamicBvh / closest_point fns
#include <crd/geometry/queries.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::ClosestPointResult;
using crd::geometry::RayHit;
using crd::geometry::bvh::Bvh4Tree;
using crd::geometry::bvh::bvh4_closest_point;
using crd::geometry::bvh::bvh4_collapse;
using crd::geometry::bvh::bvh4_raycast;
using crd::geometry::bvh::bvh_build;
using crd::geometry::bvh::BvhBuildOptions;
using crd::geometry::bvh::BvhClosestPoint;
using crd::geometry::bvh::BvhRayHit;
using crd::geometry::bvh::BvhTree;
using crd::geometry::bvh::DynamicBvh;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::closest_point;
using crd::geometry::primitives::Ray3;
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

// ---- result-type layout pins ------------------------------------------------

TEST_CASE("queries: BvhRayHit / BvhClosestPoint are templated alias of RayHit / ClosestPointResult", "[geometry][queries]")
{
    static_assert(std::is_same_v<BvhRayHit, RayHit<u32>>, "BvhRayHit must be RayHit<u32>");
    static_assert(std::is_same_v<BvhClosestPoint, ClosestPointResult<u32>>,
                  "BvhClosestPoint must be ClosestPointResult<u32>");
    // Field-order pin (ADR-0076 section 16 pin #2): RayHit{t, payload};
    // ClosestPointResult{point, distance_squared, payload}.
    const BvhRayHit rh{1.5F, 42U};
    REQUIRE(rh.t == 1.5F);
    REQUIRE(rh.payload == 42U);
    const BvhClosestPoint cp{Vec3<f32>(3, 4, 5), 25.0F, 7U};
    REQUIRE(cp.point == Vec3<f32>(3, 4, 5));
    REQUIRE(cp.distance_squared == 25.0F);
    REQUIRE(cp.payload == 7U);
}

// ---- raycast facade ---------------------------------------------------------

TEST_CASE("queries.raycast(BvhTree) == bvh_raycast on a random corpus", "[geometry][queries][raycast]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "queries-test");
    Rng rng(0xCAFE);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 300; ++i)
    {
        prims.push_back(random_box(rng, 80.0F, 4.0F));
    }
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);

    for (usize r = 0; r < 500; ++r)
    {
        const Vec3<f32> o(rng.range(-120, 120), rng.range(-120, 120), rng.range(-120, 120));
        const Vec3<f32> d(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
        const Ray3<f32> ray{o, d};
        const f32 tmax = (r % 4U == 0U) ? rng.range(5.0F, 100.0F) : std::numeric_limits<f32>::infinity();
        const std::optional<BvhRayHit> facade = crd::geometry::raycast(tree, pspan, ray, tmax);
        const std::optional<BvhRayHit> direct = crd::geometry::bvh::bvh_raycast(tree, pspan, ray, tmax);
        REQUIRE(facade.has_value() == direct.has_value());
        if (facade)
        {
            REQUIRE(facade->t == direct->t);
            REQUIRE(facade->payload == direct->payload);
        }
    }
}

TEST_CASE("queries.raycast(Bvh4Tree) == bvh4_raycast and == queries.raycast(BvhTree)", "[geometry][queries][raycast]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "queries-test");
    Rng rng(0xBEEF);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 300; ++i)
    {
        prims.push_back(random_box(rng, 80.0F, 4.0F));
    }
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree binary = bvh_build(pspan, &alloc);
    const Bvh4Tree quad = bvh4_collapse(binary, &alloc);

    for (usize r = 0; r < 500; ++r)
    {
        const Vec3<f32> o(rng.range(-120, 120), rng.range(-120, 120), rng.range(-120, 120));
        const Vec3<f32> d(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
        const Ray3<f32> ray{o, d};
        const f32 tmax = (r % 5U == 0U) ? rng.range(5.0F, 100.0F) : std::numeric_limits<f32>::infinity();
        const std::optional<BvhRayHit> q_facade = crd::geometry::raycast(quad, pspan, ray, tmax);
        const std::optional<BvhRayHit> q_direct = bvh4_raycast(quad, pspan, ray, tmax);
        const std::optional<BvhRayHit> b_facade = crd::geometry::raycast(binary, pspan, ray, tmax);
        REQUIRE(q_facade.has_value() == q_direct.has_value());
        REQUIRE(q_facade.has_value() == b_facade.has_value());
        if (q_facade)
        {
            REQUIRE(q_facade->t == q_direct->t);
            REQUIRE(q_facade->payload == q_direct->payload);
            REQUIRE(q_facade->t == b_facade->t); // collapse doesn't change the chosen hit
        }
    }
}

// ---- overlap facade ---------------------------------------------------------

TEST_CASE("queries.overlap(BvhTree) and overlap(Bvh4Tree) match brute force", "[geometry][queries][overlap]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "queries-test");
    Rng rng(0xF00D);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 200; ++i)
    {
        prims.push_back(random_box(rng, 60.0F, 4.0F));
    }
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree binary = bvh_build(pspan, &alloc);
    const Bvh4Tree quad = bvh4_collapse(binary, &alloc);

    for (usize r = 0; r < 50; ++r)
    {
        const AABB3<f32> q = random_box(rng, 70.0F, 6.0F);

        crd::containers::Array<u32> bin_facade(&alloc);
        crd::geometry::overlap(binary, pspan, q, bin_facade);
        crd::containers::Array<u32> quad_facade(&alloc);
        crd::geometry::overlap(quad, pspan, q, quad_facade);

        std::vector<u32> bin_vec, quad_vec;
        for (usize i = 0; i < bin_facade.size(); ++i)
        {
            bin_vec.push_back(bin_facade[i]);
        }
        for (usize i = 0; i < quad_facade.size(); ++i)
        {
            quad_vec.push_back(quad_facade[i]);
        }
        std::sort(bin_vec.begin(), bin_vec.end());
        std::sort(quad_vec.begin(), quad_vec.end());

        std::vector<u32> brute;
        for (usize i = 0; i < prims.size(); ++i)
        {
            if (crd::geometry::primitives::intersects(prims[i], q))
            {
                brute.push_back(static_cast<u32>(i));
            }
        }
        std::sort(brute.begin(), brute.end());

        REQUIRE(bin_vec == brute);
        REQUIRE(quad_vec == brute);

        // Callback form agrees with Array form.
        std::vector<u32> cb;
        crd::geometry::overlap(binary, pspan, q, [&cb](u32 p) { cb.push_back(p); });
        std::sort(cb.begin(), cb.end());
        REQUIRE(cb == brute);
    }
}

TEST_CASE("queries.overlap(DynamicBvh) visits fat AABBs by user_data", "[geometry][queries][overlap]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "queries-test");
    DynamicBvh dt(&alloc);
    const u32 a = dt.insert(AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1)), 100U).value;
    const u32 b = dt.insert(AABB3<f32>(Vec3<f32>(5, 5, 5), Vec3<f32>(7, 7, 7)), 200U).value;
    const u32 c = dt.insert(AABB3<f32>(Vec3<f32>(-10, -10, -10), Vec3<f32>(-8, -8, -8)), 300U).value;
    (void)a;
    (void)b;
    (void)c;

    crd::containers::Array<u32> out(&alloc);
    crd::geometry::overlap(dt, AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(6, 6, 6)), out);
    std::vector<u32> got;
    for (usize i = 0; i < out.size(); ++i)
    {
        got.push_back(out[i]);
    }
    std::sort(got.begin(), got.end());
    REQUIRE(got == std::vector<u32>{100U, 200U}); // user_data for a and b, not c
}

// ---- closest_point facade ---------------------------------------------------

TEST_CASE("queries.closest_point(BvhTree) == bvh_closest_point on random corpus", "[geometry][queries][closest]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "queries-test");
    Rng rng(0xC0FFEE);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 250; ++i)
    {
        prims.push_back(random_box(rng, 60.0F, 4.0F));
    }
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);

    for (usize r = 0; r < 300; ++r)
    {
        const Vec3<f32> q(rng.range(-100, 100), rng.range(-100, 100), rng.range(-100, 100));
        const f32 max_dist = (r % 5U == 0U) ? rng.range(5.0F, 50.0F) : std::numeric_limits<f32>::infinity();
        const std::optional<BvhClosestPoint> facade = crd::geometry::closest_point(tree, pspan, q, max_dist);
        const std::optional<BvhClosestPoint> direct = crd::geometry::bvh::bvh_closest_point(tree, pspan, q, max_dist);
        REQUIRE(facade.has_value() == direct.has_value());
        if (facade)
        {
            REQUIRE(facade->distance_squared == direct->distance_squared);
            REQUIRE(facade->payload == direct->payload);
            REQUIRE(facade->point == direct->point);
        }
    }
}

TEST_CASE("bvh4_closest_point matches brute force and BvhTree closest_point", "[geometry][queries][closest]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "queries-test");
    Rng rng(0xD15EA5E);
    for (usize trial = 0; trial < 3; ++trial)
    {
        const usize n = 80U + (rng.next() % 400U);
        std::vector<AABB3<f32>> prims;
        for (usize i = 0; i < n; ++i)
        {
            prims.push_back(random_box(rng, 70.0F, 4.0F));
        }
        BvhBuildOptions opts;
        opts.max_leaf_prims = static_cast<crd::u16>(1U + (rng.next() % 6U));
        const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
        const BvhTree binary = bvh_build(pspan, &alloc, opts);
        const Bvh4Tree quad = bvh4_collapse(binary, &alloc);

        for (usize r = 0; r < 200; ++r)
        {
            const Vec3<f32> q(rng.range(-120, 120), rng.range(-120, 120), rng.range(-120, 120));
            const f32 max_dist = (r % 5U == 0U) ? rng.range(5.0F, 60.0F) : std::numeric_limits<f32>::infinity();
            const std::optional<BvhClosestPoint> got = bvh4_closest_point(quad, pspan, q, max_dist);
            const std::optional<BvhClosestPoint> ref = brute_closest(prims, q, max_dist);
            REQUIRE(got.has_value() == ref.has_value());
            if (got)
            {
                // Squared distance bit-matches (same closest_point call); on a
                // tie the chosen prim may differ but each named prim genuinely
                // realises that distance + the point is on its AABB.
                REQUIRE(got->distance_squared == ref->distance_squared);
                const Vec3<f32> d = got->point - q;
                REQUIRE(dot(d, d) == got->distance_squared);
                REQUIRE(closest_point(prims[got->payload], q) == got->point);
            }
            // Cross-backend agreement: BVH4 and BvhTree closest-point produce
            // the same squared distance (the collapse changes only fan-out).
            const std::optional<BvhClosestPoint> bin = crd::geometry::closest_point(binary, pspan, q, max_dist);
            REQUIRE(bin.has_value() == got.has_value());
            if (got)
            {
                REQUIRE(bin->distance_squared == got->distance_squared);
            }
        }
    }
}

TEST_CASE("bvh4_closest_point: empty / single-prim / cutoff", "[geometry][queries][closest]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "queries-test");
    // empty
    const BvhTree e_bin = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(), &alloc);
    const Bvh4Tree e_quad = bvh4_collapse(e_bin, &alloc);
    REQUIRE_FALSE(bvh4_closest_point(e_quad, crd::containers::ConstSpan<AABB3<f32>>(), Vec3<f32>(0, 0, 0)).has_value());

    // single primitive
    const AABB3<f32> box(Vec3<f32>(-1, -2, -3), Vec3<f32>(4, 5, 6));
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(&box, 1);
    const BvhTree bin = bvh_build(pspan, &alloc);
    const Bvh4Tree quad = bvh4_collapse(bin, &alloc);
    const std::optional<BvhClosestPoint> got = bvh4_closest_point(quad, pspan, Vec3<f32>(100, 0, 0));
    REQUIRE(got.has_value());
    REQUIRE(got->payload == 0U);
    REQUIRE(got->point == closest_point(box, Vec3<f32>(100, 0, 0)));

    // max_dist cutoff (closest face at x=9)
    std::vector<AABB3<f32>> prims = {AABB3<f32>(Vec3<f32>(9, -1, -1), Vec3<f32>(11, 1, 1))};
    const auto pspan2 = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree bin2 = bvh_build(pspan2, &alloc);
    const Bvh4Tree quad2 = bvh4_collapse(bin2, &alloc);
    REQUIRE(bvh4_closest_point(quad2, pspan2, Vec3<f32>(0, 0, 0), 10.0F).has_value());
    REQUIRE_FALSE(bvh4_closest_point(quad2, pspan2, Vec3<f32>(0, 0, 0), 5.0F).has_value());
}

TEST_CASE("DynamicBvh::closest_point matches the closest fat-AABB by user_data", "[geometry][queries][closest]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "queries-test");
    Rng rng(0xDEADC0DE);
    DynamicBvh dt(&alloc);

    // Insert a corpus, remember each leaf's fat AABB + user_data.
    struct Entry
    {
        u32 ud;
        AABB3<f32> fat;
    };
    std::vector<Entry> entries;
    const usize n = 250;
    for (usize i = 0; i < n; ++i)
    {
        const AABB3<f32> tight = random_box(rng, 60.0F, 3.0F);
        const u32 ud = static_cast<u32>(i + 1U);
        const auto id = dt.insert(tight, ud);
        entries.push_back(Entry{ud, dt.fat_aabb(id)});
    }

    for (usize r = 0; r < 300; ++r)
    {
        const Vec3<f32> q(rng.range(-100, 100), rng.range(-100, 100), rng.range(-100, 100));
        const f32 max_dist = (r % 5U == 0U) ? rng.range(5.0F, 50.0F) : std::numeric_limits<f32>::infinity();
        const std::optional<ClosestPointResult<u32>> got = crd::geometry::closest_point(dt, q, max_dist);

        // Brute force over the fat AABBs (broadphase reference).
        f32 best =
            (max_dist >= std::numeric_limits<f32>::infinity()) ? std::numeric_limits<f32>::infinity() : max_dist * max_dist;
        bool any = false;
        u32 best_ud = 0;
        Vec3<f32> best_pt{};
        for (const Entry& e : entries)
        {
            const Vec3<f32> cp = closest_point(e.fat, q);
            const Vec3<f32> d = cp - q;
            const f32 d2 = dot(d, d);
            if (d2 < best)
            {
                best = d2;
                best_ud = e.ud;
                best_pt = cp;
                any = true;
            }
        }
        REQUIRE(got.has_value() == any);
        if (got)
        {
            REQUIRE(got->distance_squared == best);
            // On a tie the BVH and brute force may name different leaves, but
            // each named one must genuinely realise that distance + its `point`
            // must lie on the named leaf's fat AABB.
            (void)best_ud;
            (void)best_pt;
            const Vec3<f32> d = got->point - q;
            REQUIRE(dot(d, d) == got->distance_squared);
            // Resolve the named leaf's fat AABB via the user_data → entry map.
            const Entry* named = nullptr;
            for (const Entry& e : entries)
            {
                if (e.ud == got->payload)
                {
                    named = &e;
                    break;
                }
            }
            REQUIRE(named != nullptr);
            REQUIRE(got->point == closest_point(named->fat, q));
        }
    }
}

TEST_CASE("DynamicBvh::closest_point: empty / inside / cutoff", "[geometry][queries][closest]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "queries-test");
    DynamicBvh dt(&alloc);
    REQUIRE_FALSE(dt.closest_point(Vec3<f32>(0, 0, 0)).has_value());

    const AABB3<f32> tight(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1));
    const auto id = dt.insert(tight, 42U);
    const AABB3<f32> fat = dt.fat_aabb(id);
    (void)fat;

    // Query inside the fat AABB → distance 0 at the query point.
    const std::optional<ClosestPointResult<u32>> inside = dt.closest_point(Vec3<f32>(0, 0, 0));
    REQUIRE(inside.has_value());
    REQUIRE(inside->distance_squared == 0.0F);
    REQUIRE(inside->point == Vec3<f32>(0, 0, 0));
    REQUIRE(inside->payload == 42U);

    // Cutoff: too far → nullopt.
    const std::optional<ClosestPointResult<u32>> far = dt.closest_point(Vec3<f32>(100, 0, 0), 1.0F);
    REQUIRE_FALSE(far.has_value());
}
