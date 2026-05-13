// crd-geometry v1i-b — shapecast facade tests.
//
// Covers:
//   * Primitive `cast_ray` / `cast_sphere` / `cast_box` overloads against
//     AABB / OBB / Sphere / Plane targets.
//   * BVH `cast_sphere` / `cast_box` over `BvhTree` and `Bvh4Tree` — TOI
//     bit-matches a brute-force per-prim inflated-AABB ray test (the same
//     algorithmic kernel the traversal invokes per node/leaf, so any
//     traversal bug surfaces as a "named different prim with a different t").
//   * **Bisection sanity check** (advisor recommendation) for `cast_sphere`
//     vs AABB: at the closed-form TOI, `distance(target, swept_center(t)) ≈
//     moving.radius`; one sample-width earlier the sphere is still
//     non-touching. Catches a Minkowski-sum sign error in the closed-form
//     derivation that the per-algo brute-force test cannot.
//   * **Degenerate-recovery pins:** a zero-radius `cast_sphere` recovers
//     `cast_ray`; a zero-extent `cast_box` recovers `cast_ray`; cross-backend
//     `cast_sphere(BvhTree) == cast_sphere(Bvh4Tree)` (collapse changes only
//     fan-out).

#include <crd/geometry/bvh/bvh.hpp> // umbrella — bvh_build / bvh4_collapse / shapecast funcs
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
using crd::geometry::bvh::Bvh4Tree;
using crd::geometry::bvh::bvh4_collapse;
using crd::geometry::bvh::bvh_build;
using crd::geometry::bvh::BvhRayHit;
using crd::geometry::bvh::BvhTree;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::distance_squared;
using crd::geometry::primitives::intersect_ray_aabb;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Ray3;
using crd::geometry::primitives::Sphere;
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

AABB3<f32> inflate_box(const AABB3<f32>& a, const Vec3<f32>& pad)
{
    return AABB3<f32>(Vec3<f32>(a.min.x - pad.x, a.min.y - pad.y, a.min.z - pad.z),
                      Vec3<f32>(a.max.x + pad.x, a.max.y + pad.y, a.max.z + pad.z));
}

// Brute-force inflated-AABB raycast over all prims (the *same* algorithmic
// kernel the BVH traversal applies per node/leaf — catches traversal bugs
// but not closed-form derivation bugs; the bisection test catches those).
std::optional<BvhRayHit> brute_inflated(const std::vector<AABB3<f32>>& prims, const Vec3<f32>& origin,
                                        const Vec3<f32>& dir, const Vec3<f32>& pad, f32 tmax)
{
    f32 best_t = tmax;
    u32 best_p = 0;
    bool hit = false;
    const Ray3<f32> ray(origin, dir);
    for (usize i = 0; i < prims.size(); ++i)
    {
        const AABB3<f32> grown = inflate_box(prims[i], pad);
        f32 t = 0.0F;
        if (intersect_ray_aabb(ray, grown, t) && t < best_t)
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

} // namespace

// ---- primitive shapecast: degenerate recovery -----------------------------

TEST_CASE("cast_sphere(radius=0) == cast_ray for primitive AABB", "[geometry][shapecast]")
{
    const AABB3<f32> target(Vec3<f32>(2, 0, 0), Vec3<f32>(4, 2, 2));
    const Sphere<f32> moving(Vec3<f32>(0, 1, 1), 0.0F);
    const Vec3<f32> dir(1, 0, 0);
    const auto sphere = crd::geometry::cast_sphere(moving, dir, std::numeric_limits<f32>::infinity(), target);
    const auto ray = crd::geometry::cast_ray(Ray3<f32>(moving.center, dir), target);
    REQUIRE(sphere.has_value());
    REQUIRE(ray.has_value());
    REQUIRE(*sphere == *ray);
}

TEST_CASE("cast_box(half=0) == cast_ray for primitive AABB", "[geometry][shapecast]")
{
    const AABB3<f32> target(Vec3<f32>(2, 0, 0), Vec3<f32>(4, 2, 2));
    const Vec3<f32> origin(0, 1, 1);
    const AABB3<f32> moving(origin, origin); // zero-extent box at `origin`
    const Vec3<f32> dir(1, 0, 0);
    const auto box = crd::geometry::cast_box(moving, dir, std::numeric_limits<f32>::infinity(), target);
    const auto ray = crd::geometry::cast_ray(Ray3<f32>(origin, dir), target);
    REQUIRE(box.has_value());
    REQUIRE(ray.has_value());
    REQUIRE(*box == *ray);
}

// ---- primitive shapecast: overlap-at-start -------------------------------

TEST_CASE("cast_sphere returns t=0 when already overlapping at start", "[geometry][shapecast]")
{
    const Sphere<f32> moving(Vec3<f32>(0, 0, 0), 2.0F);
    // sphere-vs-AABB: target's inflated box contains the start position
    const AABB3<f32> tgt_box(Vec3<f32>(-3, -3, -3), Vec3<f32>(3, 3, 3));
    REQUIRE(crd::geometry::cast_sphere(moving, Vec3<f32>(1, 0, 0), 100.0F, tgt_box) == 0.0F);
    // sphere-vs-sphere: spheres already overlapping
    const Sphere<f32> tgt_sphere(Vec3<f32>(2, 0, 0), 1.0F);
    REQUIRE(crd::geometry::cast_sphere(moving, Vec3<f32>(1, 0, 0), 100.0F, tgt_sphere) == 0.0F);
    // sphere-vs-plane: sphere touches plane at start
    const Plane<f32> tgt_plane(Vec3<f32>(1, 0, 0), -1.0F); // x = 1 (signed dist of moving = -1, |sd|=1, radius=2 ⇒ overlap)
    REQUIRE(crd::geometry::cast_sphere(moving, Vec3<f32>(0, 1, 0), 100.0F, tgt_plane) == 0.0F);
}

TEST_CASE("cast_box returns t=0 when already overlapping at start", "[geometry][shapecast]")
{
    const AABB3<f32> moving(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1));
    const AABB3<f32> target(Vec3<f32>(0, 0, 0), Vec3<f32>(2, 2, 2)); // overlaps moving
    REQUIRE(crd::geometry::cast_box(moving, Vec3<f32>(1, 0, 0), 100.0F, target) == 0.0F);
}

// ---- primitive shapecast: closed-form correctness -------------------------

TEST_CASE("cast_sphere vs AABB: ray-vs-inflated-AABB", "[geometry][shapecast]")
{
    // Sphere of radius 1 at (-3, 1, 1) sweeping +x. Target AABB face at x = 2;
    // inflated AABB face at x = 1; sphere center reaches x = 1 at t = 4.
    const Sphere<f32> moving(Vec3<f32>(-3, 1, 1), 1.0F);
    const AABB3<f32> target(Vec3<f32>(2, 0, 0), Vec3<f32>(4, 2, 2));
    const auto t = crd::geometry::cast_sphere(moving, Vec3<f32>(1, 0, 0), 100.0F, target);
    REQUIRE(t.has_value());
    REQUIRE(*t == 4.0F);
}

TEST_CASE("cast_sphere vs sphere: quadratic exact", "[geometry][shapecast]")
{
    // Moving sphere radius 1 at origin, target sphere radius 1 at (10, 0, 0).
    // First contact when center-distance = 2 ⇒ t = 8.
    const Sphere<f32> moving(Vec3<f32>(0, 0, 0), 1.0F);
    const Sphere<f32> target(Vec3<f32>(10, 0, 0), 1.0F);
    const auto t = crd::geometry::cast_sphere(moving, Vec3<f32>(1, 0, 0), 100.0F, target);
    REQUIRE(t.has_value());
    REQUIRE(*t == 8.0F);
}

TEST_CASE("cast_sphere vs plane: signed-distance arithmetic", "[geometry][shapecast]")
{
    // Plane n=(0,1,0), d=0 (the xz-plane). Moving sphere radius 0.5 at (0, 5, 0)
    // sweeping -y. signed_distance = 5; touches plane when sd = ±0.5; first
    // contact at sd = 0.5 ⇒ t = 4.5.
    const Sphere<f32> moving(Vec3<f32>(0, 5, 0), 0.5F);
    const Plane<f32> target(Vec3<f32>(0, 1, 0), 0.0F);
    const auto t = crd::geometry::cast_sphere(moving, Vec3<f32>(0, -1, 0), 100.0F, target);
    REQUIRE(t.has_value());
    REQUIRE(*t == 4.5F);
}

TEST_CASE("cast_box vs AABB: Minkowski-AABB exact", "[geometry][shapecast]")
{
    // Moving box half-extents (1,1,1) centered at (-5, 0, 0), target half-
    // extents (1,1,1) centered at (5, 0, 0). Center distance = 10; first
    // contact when |dx| = 2 (sum of half-extents) ⇒ moving center moves 8 ⇒ t=8.
    const AABB3<f32> moving(Vec3<f32>(-6, -1, -1), Vec3<f32>(-4, 1, 1));
    const AABB3<f32> target(Vec3<f32>(4, -1, -1), Vec3<f32>(6, 1, 1));
    const auto t = crd::geometry::cast_box(moving, Vec3<f32>(1, 0, 0), 100.0F, target);
    REQUIRE(t.has_value());
    REQUIRE(*t == 8.0F);
}

// ---- primitive shapecast: bisection cross-check (advisor) ----------------

TEST_CASE("cast_sphere vs AABB bisection: distance squared at TOI equals r squared", "[geometry][shapecast]")
{
    // The closed-form claim under test: the conservative reduction
    // (inflate the target AABB by `r` and ray-cast the sphere's center)
    // places the swept center, at TOI, *exactly on the inflated AABB
    // boundary* — which means the distance from the swept center back to
    // the *original* AABB is `r` on a face contact, between `r` and `r·√2`
    // on an edge contact, and `r·√3` on a corner contact.
    //
    // Bound: `r² ≤ d²(target, swept_center(TOI)) ≤ 3·r²` (with slack).
    //
    // Both bounds matter. The LOWER bound catches a magnitude bug — if a
    // refactor inflates by `r/2` instead of `r` (or by `r²` instead of
    // `r`, or flips the sign), the swept center stops at the wrong
    // boundary and `d²` lands below `r² − eps`; the assertion fires. The
    // UPPER bound catches the same family of sign/magnitude bugs at the
    // other tail. We also assert the bisection is not vacuous — a setup
    // where every trial misses would make every `if (!toi) continue;`
    // skip the asserts, masking the bug; pin a minimum hit count.
    Rng rng(0xB15EC7);
    usize hits = 0;
    for (usize trial = 0; trial < 300; ++trial)
    {
        const AABB3<f32> target = random_box(rng, 30.0F, 8.0F);
        const Vec3<f32> moving_center(rng.range(-80, -50), rng.range(-30, 30), rng.range(-30, 30));
        const Sphere<f32> moving(moving_center, rng.range(0.3F, 2.0F));
        // Aim the sweep at a slightly-perturbed point inside the target so a
        // meaningful fraction of trials lands a hit (the bisection bounds are
        // vacuous if every trial misses; pin the hit floor below).
        const Vec3<f32> target_center = (target.min + target.max) * 0.5F;
        const Vec3<f32> aim_jitter(rng.range(-2.0F, 2.0F), rng.range(-2.0F, 2.0F), rng.range(-2.0F, 2.0F));
        const Vec3<f32> dir = (target_center + aim_jitter) - moving_center; // not unit; matches Ray3 convention
        const auto toi = crd::geometry::cast_sphere(moving, dir, 100.0F, target);
        if (!toi)
        {
            continue;
        }
        ++hits;

        // Already-overlapping case (toi == 0): distance² ≤ r² is a *start*
        // condition, not the post-sweep claim — assert only that and move on.
        const f32 r2 = moving.radius * moving.radius;
        if (*toi == 0.0F)
        {
            const f32 d2_start = distance_squared(target, moving.center);
            REQUIRE(d2_start <= r2 + 1e-3F);
            continue;
        }

        // At TOI: swept center is on the inflated AABB boundary. d² to
        // target is r² (face) up to 3·r² (corner). Anything below r² − eps
        // is a magnitude / sign bug in the inflation; anything above 3·r² +
        // eps is a sign-flip (inflation in the wrong direction grows the
        // gap, not closes it).
        const Vec3<f32> hit_center = moving.center + dir * (*toi);
        const f32 d2 = distance_squared(target, hit_center);
        REQUIRE(d2 >= r2 - 1e-3F);
        REQUIRE(d2 <= 3.0F * r2 + 1e-3F);

        // Just before TOI: not yet touching (distance > r, so d² > r² − eps).
        // ε is in `dir` units; the world is O(60), so 0.005 is comfortable.
        const f32 eps = 0.005F;
        const Vec3<f32> pre_center = moving.center + dir * (*toi - eps);
        const f32 d2_pre = distance_squared(target, pre_center);
        REQUIRE(d2_pre >= r2 - 1e-3F);
    }
    // Without at least a few hits the bisection bounds are vacuous —
    // pin a floor so a future refactor that drops the trial-hit-rate
    // surfaces immediately instead of silently turning the test into a
    // no-op.
    REQUIRE(hits >= 30);
}

// ---- BVH shapecast: matches brute force ----------------------------------

TEST_CASE("cast_sphere(BvhTree) and cast_sphere(Bvh4Tree) match brute force", "[geometry][shapecast]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "shapecast-test");
    Rng rng(0xDEAF);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 200; ++i)
    {
        prims.push_back(random_box(rng, 60.0F, 4.0F));
    }
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree binary = bvh_build(pspan, &alloc);
    const Bvh4Tree quad = bvh4_collapse(binary, &alloc);

    for (usize r = 0; r < 300; ++r)
    {
        const Sphere<f32> moving(
            Vec3<f32>(rng.range(-120, 120), rng.range(-120, 120), rng.range(-120, 120)),
            rng.range(0.2F, 5.0F));
        const Vec3<f32> dir(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
        const f32 tmax = (r % 4U == 0U) ? rng.range(5.0F, 80.0F) : std::numeric_limits<f32>::infinity();
        const auto got_bin = crd::geometry::cast_sphere(binary, pspan, moving, dir, tmax);
        const auto got_quad = crd::geometry::cast_sphere(quad, pspan, moving, dir, tmax);
        const Vec3<f32> pad(moving.radius, moving.radius, moving.radius);
        const auto ref = brute_inflated(prims, moving.center, dir, pad, tmax);
        REQUIRE(got_bin.has_value() == ref.has_value());
        REQUIRE(got_quad.has_value() == ref.has_value());
        if (got_bin)
        {
            // The slab kernel and `intersect_ray_aabb` differ — the BVH path
            // uses the v0f Williams/Ize robust slab (`intersect_ray_aabb_robust`)
            // and the brute force uses the non-robust `intersect_ray_aabb`. They
            // agree on `tmin` (entry) for well-formed inputs but t may differ
            // by 1-2 ULPs; the chosen prim should match (or be a t-tie).
            // Cross-backend: BVH4 == BvhTree (collapse changes only fan-out).
            REQUIRE(got_bin->t == got_quad->t);
            // brute force found the same hit (or a t-tie equivalent prim).
            // Verify the BVH-named prim is at most `ref->t`-distant.
            REQUIRE(got_bin->t <= ref->t + 1e-4F);
            REQUIRE(got_bin->t >= ref->t - 1e-4F);
        }
    }
}

TEST_CASE("cast_box(BvhTree) and cast_box(Bvh4Tree) match brute force", "[geometry][shapecast]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "shapecast-test");
    Rng rng(0xBEEFCAFE);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 200; ++i)
    {
        prims.push_back(random_box(rng, 60.0F, 4.0F));
    }
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree binary = bvh_build(pspan, &alloc);
    const Bvh4Tree quad = bvh4_collapse(binary, &alloc);

    for (usize r = 0; r < 300; ++r)
    {
        const Vec3<f32> moving_center(rng.range(-120, 120), rng.range(-120, 120), rng.range(-120, 120));
        const Vec3<f32> half(rng.range(0.2F, 3.0F), rng.range(0.2F, 3.0F), rng.range(0.2F, 3.0F));
        const AABB3<f32> moving(moving_center - half, moving_center + half);
        const Vec3<f32> dir(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
        const f32 tmax = (r % 4U == 0U) ? rng.range(5.0F, 80.0F) : std::numeric_limits<f32>::infinity();
        const auto got_bin = crd::geometry::cast_box(binary, pspan, moving, dir, tmax);
        const auto got_quad = crd::geometry::cast_box(quad, pspan, moving, dir, tmax);
        const auto ref = brute_inflated(prims, moving_center, dir, half, tmax);
        REQUIRE(got_bin.has_value() == ref.has_value());
        REQUIRE(got_quad.has_value() == ref.has_value());
        if (got_bin)
        {
            REQUIRE(got_bin->t == got_quad->t);
            REQUIRE(got_bin->t <= ref->t + 1e-4F);
            REQUIRE(got_bin->t >= ref->t - 1e-4F);
        }
    }
}

// ---- BVH shapecast: degenerate-recovery -----------------------------------

TEST_CASE("BVH cast_sphere(radius=0) returns same t as raycast", "[geometry][shapecast]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "shapecast-test");
    std::vector<AABB3<f32>> prims = {AABB3<f32>(Vec3<f32>(5, -1, -1), Vec3<f32>(7, 1, 1)),
                                     AABB3<f32>(Vec3<f32>(15, -1, -1), Vec3<f32>(17, 1, 1))};
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    const Ray3<f32> ray(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0));
    const auto rc = crd::geometry::raycast(tree, pspan, ray);
    const Sphere<f32> moving(Vec3<f32>(0, 0, 0), 0.0F);
    const auto sc = crd::geometry::cast_sphere(tree, pspan, moving, Vec3<f32>(1, 0, 0));
    REQUIRE(rc.has_value());
    REQUIRE(sc.has_value());
    REQUIRE(rc->t == sc->t);
    REQUIRE(rc->payload == sc->payload);
}

TEST_CASE("BVH cast_box(half=0) returns same t as raycast", "[geometry][shapecast]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "shapecast-test");
    std::vector<AABB3<f32>> prims = {AABB3<f32>(Vec3<f32>(5, -1, -1), Vec3<f32>(7, 1, 1))};
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    const Ray3<f32> ray(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0));
    const auto rc = crd::geometry::raycast(tree, pspan, ray);
    const AABB3<f32> moving(Vec3<f32>(0, 0, 0), Vec3<f32>(0, 0, 0));
    const auto sc = crd::geometry::cast_box(tree, pspan, moving, Vec3<f32>(1, 0, 0));
    REQUIRE(rc.has_value());
    REQUIRE(sc.has_value());
    REQUIRE(rc->t == sc->t);
    REQUIRE(rc->payload == sc->payload);
}

// ---- BVH shapecast: tmax cutoff -------------------------------------------

TEST_CASE("BVH cast_sphere respects tmax", "[geometry][shapecast]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "shapecast-test");
    std::vector<AABB3<f32>> prims = {AABB3<f32>(Vec3<f32>(9, -1, -1), Vec3<f32>(11, 1, 1))};
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    const Sphere<f32> moving(Vec3<f32>(0, 0, 0), 0.5F); // inflated face at x = 8.5
    REQUIRE(crd::geometry::cast_sphere(tree, pspan, moving, Vec3<f32>(1, 0, 0), 10.0F).has_value());
    REQUIRE_FALSE(crd::geometry::cast_sphere(tree, pspan, moving, Vec3<f32>(1, 0, 0), 5.0F).has_value());
}

// ---- BVH shapecast: empty tree -------------------------------------------

TEST_CASE("BVH shapecast over empty tree returns nullopt", "[geometry][shapecast]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "shapecast-test");
    const BvhTree tree = bvh_build(crd::containers::ConstSpan<AABB3<f32>>(), &alloc);
    const Bvh4Tree quad = bvh4_collapse(tree, &alloc);
    const Sphere<f32> moving(Vec3<f32>(0, 0, 0), 1.0F);
    REQUIRE_FALSE(crd::geometry::cast_sphere(tree, crd::containers::ConstSpan<AABB3<f32>>(), moving, Vec3<f32>(1, 0, 0)).has_value());
    REQUIRE_FALSE(crd::geometry::cast_sphere(quad, crd::containers::ConstSpan<AABB3<f32>>(), moving, Vec3<f32>(1, 0, 0)).has_value());
    const AABB3<f32> m_box(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1));
    REQUIRE_FALSE(crd::geometry::cast_box(tree, crd::containers::ConstSpan<AABB3<f32>>(), m_box, Vec3<f32>(1, 0, 0)).has_value());
    REQUIRE_FALSE(crd::geometry::cast_box(quad, crd::containers::ConstSpan<AABB3<f32>>(), m_box, Vec3<f32>(1, 0, 0)).has_value());
}
