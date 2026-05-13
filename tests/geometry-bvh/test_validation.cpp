// crd-geometry-bvh v1i-c — validation discipline (ADR-0076 §15, §16 pin #3).
//
// Exercises the NaN/Inf contract at the BVH-query layer (raycast / overlap /
// closest_point / shapecast / find_overlapping_pairs): garbage ray /
// box / query inputs must never crash, never UB; the tree itself is built
// from clean prims (builders REJECT non-finite inputs, per the contract — so
// we don't try to build a BVH from a NaN corpus). And the large-coordinate
// sweep: build a clean tree at the origin and at a +1e6 origin, query at
// matching offsets, results should agree within f32-ULP tolerance.

#include "test_corpus.hpp"

#include <crd/geometry/bvh/bvh.hpp>
#include <crd/geometry/queries.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::bvh::bvh_build;
using crd::geometry::bvh::BvhTree;
using crd::geometry::bvh::DynamicBvh;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;
using crd::geometry::primitives::Sphere;
using crd::math::Vec3;
namespace tc = crd::geometry::test_corpus;

// A small clean corpus shared across the large-coordinate tests.
std::vector<AABB3<f32>> clean_corpus()
{
    return {
        AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1)),
        AABB3<f32>(Vec3<f32>(3, 0, 0), Vec3<f32>(4, 1, 1)),
        AABB3<f32>(Vec3<f32>(0, 3, 0), Vec3<f32>(1, 4, 1)),
        AABB3<f32>(Vec3<f32>(0, 0, 3), Vec3<f32>(1, 1, 4)),
        AABB3<f32>(Vec3<f32>(10, 0, 0), Vec3<f32>(11, 1, 1)),
    };
}

std::vector<AABB3<f32>> shifted(const std::vector<AABB3<f32>>& prims, const Vec3<f32>& offset)
{
    std::vector<AABB3<f32>> out;
    out.reserve(prims.size());
    for (const AABB3<f32>& a : prims)
    {
        out.push_back(tc::shift(a, offset));
    }
    return out;
}

} // namespace

// ---- NaN/Inf query inputs: BVH must tolerate ------------------------------

TEST_CASE("validation: BVH raycast tolerates degenerate rays", "[geometry][validation][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "val-test");
    const std::vector<AABB3<f32>> prims = clean_corpus();
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    for (const Ray3<f32>& ray : tc::degenerate_rays())
    {
        // The contract: never UB. The result (hit or miss) is allowed.
        (void)crd::geometry::raycast(tree, pspan, ray);
        SUCCEED();
    }
}

TEST_CASE("validation: BVH overlap tolerates degenerate query AABBs", "[geometry][validation][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "val-test");
    const std::vector<AABB3<f32>> prims = clean_corpus();
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    for (const AABB3<f32>& q : tc::degenerate_aabbs())
    {
        crd::containers::Array<u32> out(&alloc);
        crd::geometry::overlap(tree, pspan, q, out);
        SUCCEED(); // contract — never UB
    }
}

TEST_CASE("validation: BVH closest_point tolerates degenerate query points", "[geometry][validation][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "val-test");
    const std::vector<AABB3<f32>> prims = clean_corpus();
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    const std::vector<Vec3<f32>> queries = {
        Vec3<f32>(tc::k_nan, 0, 0), Vec3<f32>(0, tc::k_nan, 0), Vec3<f32>(0, 0, tc::k_nan),
        Vec3<f32>(+tc::k_inf, 0, 0), Vec3<f32>(-tc::k_inf, 0, 0),
    };
    for (const Vec3<f32>& q : queries)
    {
        (void)crd::geometry::closest_point(tree, pspan, q);
        SUCCEED();
    }
}

TEST_CASE("validation: BVH shapecast tolerates degenerate moving shapes", "[geometry][validation][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "val-test");
    const std::vector<AABB3<f32>> prims = clean_corpus();
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    // Degenerate sphere shapes (negative / NaN / ∞ radius) — shapecast must
    // tolerate. The is_finite assert in the binary-inflated-raycast helper
    // fires in debug for non-finite *origin* / *dir* / *pad* (which is
    // derived from radius); the contract says queries with non-finite inputs
    // are user errors that builders catch — for queries, we accept the
    // assert in debug and require no UB in release. So here we exercise the
    // finite-but-extreme cases only.
    for (const Sphere<f32>& s : {Sphere<f32>(Vec3<f32>(0, 0, 0), 0.0F),    // zero radius
                                 Sphere<f32>(Vec3<f32>(0, 0, 0), -1.0F),   // negative
                                 Sphere<f32>(Vec3<f32>(0, 0, 0), 1e-12F)}) // very small
    {
        (void)crd::geometry::cast_sphere(tree, pspan, s, Vec3<f32>(1, 0, 0));
        SUCCEED();
    }
}

TEST_CASE("validation: DynamicBvh find_overlapping_pairs tolerates an empty tree", "[geometry][validation][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "val-test");
    const DynamicBvh dt(&alloc);
    crd::containers::Array<crd::geometry::bvh::DynamicBvhPair> out(&alloc);
    crd::geometry::find_overlapping_pairs(dt, out);
    REQUIRE(out.size() == 0U);
}

// ---- Large-coordinate sweep: BVH queries at +1e6 origin ------------------

TEST_CASE("validation: BVH raycast is shift-invariant at +1e6 origin", "[geometry][validation][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "val-test");
    const std::vector<AABB3<f32>> prims = clean_corpus();
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    const Ray3<f32> ray(Vec3<f32>(-5, 0.5F, 0.5F), Vec3<f32>(1, 0, 0));
    const auto hit_origin = crd::geometry::raycast(tree, pspan, ray);
    REQUIRE(hit_origin.has_value());

    const Vec3<f32> offset(tc::k_far_origin_modest, tc::k_far_origin_modest, tc::k_far_origin_modest);
    const std::vector<AABB3<f32>> prims_far = shifted(prims, offset);
    const auto pspan_far = crd::containers::ConstSpan<AABB3<f32>>(prims_far.data(), prims_far.size());
    const BvhTree tree_far = bvh_build(pspan_far, &alloc);
    const Ray3<f32> ray_far = tc::shift(ray, offset);
    const auto hit_far = crd::geometry::raycast(tree_far, pspan_far, ray_far);
    REQUIRE(hit_far.has_value());

    // Compare the named AABB equivalence, not the payload index. The two
    // builders may pick different leaves on a SAH tiebreak after the shift
    // (a future tiebreak refactor would surface here as a false fail if we
    // compared payloads). The right shift-invariance claim: the named hit
    // box at the far origin is the *same shape* as the named hit box at the
    // origin once shifted back, within local-ULP precision.
    const AABB3<f32>& hit_box_origin = prims[hit_origin->payload];
    const AABB3<f32> hit_box_far_back = tc::shift(prims_far[hit_far->payload],
                                                  Vec3<f32>(-offset.x, -offset.y, -offset.z));
    const f32 box_tol = tc::ulp_tolerance_for(tc::k_far_origin_modest) * 4.0F;
    REQUIRE(std::abs(hit_box_far_back.min.x - hit_box_origin.min.x) <= box_tol);
    REQUIRE(std::abs(hit_box_far_back.min.y - hit_box_origin.min.y) <= box_tol);
    REQUIRE(std::abs(hit_box_far_back.min.z - hit_box_origin.min.z) <= box_tol);
    REQUIRE(std::abs(hit_box_far_back.max.x - hit_box_origin.max.x) <= box_tol);
    REQUIRE(std::abs(hit_box_far_back.max.y - hit_box_origin.max.y) <= box_tol);
    REQUIRE(std::abs(hit_box_far_back.max.z - hit_box_origin.max.z) <= box_tol);
    const f32 t_tol = tc::ulp_tolerance_for(tc::k_far_origin_modest) * 4.0F;
    REQUIRE(std::abs(hit_origin->t - hit_far->t) <= t_tol);
}

TEST_CASE("validation: BVH overlap is shift-invariant at +1e7 origin", "[geometry][validation][bvh]")
{
    // overlap is a pure boolean per-prim AABB-vs-AABB test — at +1e7 the
    // coordinates lose precision but the boolean answers stay correct as
    // long as the input AABBs themselves remain disjoint or overlapping
    // after the shift (they do — the gap is O(1), the precision loss is
    // O(1)).
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "val-test");
    const std::vector<AABB3<f32>> prims = clean_corpus();
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    const AABB3<f32> query(Vec3<f32>(0.5F, 0.5F, 0.5F), Vec3<f32>(3.5F, 0.7F, 0.7F)); // overlaps prims 0 and 1
    crd::containers::Array<u32> out_origin(&alloc);
    crd::geometry::overlap(tree, pspan, query, out_origin);

    const Vec3<f32> offset(tc::k_far_origin_stress, tc::k_far_origin_stress, tc::k_far_origin_stress);
    const std::vector<AABB3<f32>> prims_far = shifted(prims, offset);
    const auto pspan_far = crd::containers::ConstSpan<AABB3<f32>>(prims_far.data(), prims_far.size());
    const BvhTree tree_far = bvh_build(pspan_far, &alloc);
    const AABB3<f32> query_far = tc::shift(query, offset);
    crd::containers::Array<u32> out_far(&alloc);
    crd::geometry::overlap(tree_far, pspan_far, query_far, out_far);

    // Sort both for comparison — overlap iteration order is deterministic
    // per tree but the two trees may differ in node-array layout.
    std::vector<u32> a(out_origin.data(), out_origin.data() + out_origin.size());
    std::vector<u32> b(out_far.data(), out_far.data() + out_far.size());
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    REQUIRE(a == b);
}
