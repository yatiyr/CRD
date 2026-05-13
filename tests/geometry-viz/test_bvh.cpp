// crd-geometry-viz v1j-a — BVH traversal visualisation tests.

#include <crd/draw/render_buffer.hpp>
#include <crd/geometry/bvh/bvh.hpp>
#include <crd/geometry/viz/bvh.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::draw::RenderBuffer;
using crd::geometry::bvh::Bvh4Tree;
using crd::geometry::bvh::bvh4_collapse;
using crd::geometry::bvh::bvh_build;
using crd::geometry::bvh::BvhTree;
using crd::geometry::bvh::DynamicBvh;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Frustum;
using crd::geometry::primitives::Plane;
using crd::math::Vec3f;
namespace viz = crd::geometry::viz;

std::vector<AABB3<f32>> small_corpus()
{
    return {AABB3<f32>(Vec3f(0, 0, 0), Vec3f(1, 1, 1)), AABB3<f32>(Vec3f(2, 0, 0), Vec3f(3, 1, 1)),
            AABB3<f32>(Vec3f(0, 2, 0), Vec3f(1, 3, 1)), AABB3<f32>(Vec3f(5, 0, 0), Vec3f(6, 1, 1))};
}
} // namespace

TEST_CASE("viz::depth_color cycles every 8 depths", "[geometry][viz][bvh]")
{
    for (u32 d = 0; d < 16U; ++d)
    {
        REQUIRE(viz::depth_color(d) == viz::depth_color(d + 8U));
    }
}

TEST_CASE("viz::draw_bvh(BvhTree) emits an AABB per node", "[geometry][viz][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "viz-test");
    const std::vector<AABB3<f32>> prims = small_corpus();
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    RenderBuffer buf(&alloc);
    viz::draw_bvh(buf, tree, pspan);
    // Each node emits 12 edges (an AABB wireframe).
    REQUIRE(buf.line_count() == tree.node_count() * 12U);
}

TEST_CASE("viz::draw_bvh(BvhTree, depth_limit=1) caps the walk", "[geometry][viz][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "viz-test");
    const std::vector<AABB3<f32>> prims = small_corpus();
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    RenderBuffer buf(&alloc);
    viz::draw_bvh(buf, tree, pspan, 1U /* depth_limit */);
    // At most 3 nodes visited (root + 2 children) at depth 0/1.
    REQUIRE(buf.line_count() <= 3U * 12U);
    REQUIRE(buf.line_count() > 0U);
}

TEST_CASE("viz::draw_bvh(Bvh4Tree) emits AABBs for nodes + children", "[geometry][viz][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "viz-test");
    const std::vector<AABB3<f32>> prims = small_corpus();
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree binary = bvh_build(pspan, &alloc);
    const Bvh4Tree quad = bvh4_collapse(binary, &alloc);
    RenderBuffer buf(&alloc);
    viz::draw_bvh(buf, quad, pspan);
    REQUIRE(buf.line_count() > 0U);
}

TEST_CASE("viz::draw_bvh_bounds(DynamicBvh) emits exactly the root AABB", "[geometry][viz][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    DynamicBvh dt(&alloc);
    (void)dt.insert(AABB3<f32>(Vec3f(0, 0, 0), Vec3f(1, 1, 1)), 1U);
    (void)dt.insert(AABB3<f32>(Vec3f(5, 0, 0), Vec3f(6, 1, 1)), 2U);
    RenderBuffer buf(&alloc);
    viz::draw_bvh_bounds(buf, dt);
    REQUIRE(buf.line_count() == 12U); // exactly one AABB wireframe = 12 edges
}

TEST_CASE("viz::draw_bvh_bounds(empty DynamicBvh) emits nothing", "[geometry][viz][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "viz-test");
    const DynamicBvh dt(&alloc);
    RenderBuffer buf(&alloc);
    viz::draw_bvh_bounds(buf, dt);
    REQUIRE(buf.line_count() == 0U);
}

TEST_CASE("viz::draw_overlap_pairs(DynamicBvh) is the no-position-lookup no-op", "[geometry][viz][bvh]")
{
    // The convenience overload deliberately emits nothing because the
    // DynamicBvh has no centroid table — callers wanting per-pair lines
    // must use `draw_overlap_pairs_with(buf, tree, ud_to_pos)` and supply
    // a position lookup.
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    DynamicBvh dt(&alloc);
    (void)dt.insert(AABB3<f32>(Vec3f(-1, -1, -1), Vec3f(1, 1, 1)), 10U);
    (void)dt.insert(AABB3<f32>(Vec3f(0, 0, 0), Vec3f(2, 2, 2)), 20U); // overlaps the above
    RenderBuffer buf(&alloc);
    viz::draw_overlap_pairs(buf, dt);
    REQUIRE(buf.line_count() == 0U);
    REQUIRE(buf.point_count() == 0U);
}

TEST_CASE("viz::draw_overlap_pairs_with: one line per overlapping pair", "[geometry][viz][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    DynamicBvh dt(&alloc);
    // Three boxes: A and B overlap, C is isolated.
    (void)dt.insert(AABB3<f32>(Vec3f(-1, -1, -1), Vec3f(1, 1, 1)), 10U);
    (void)dt.insert(AABB3<f32>(Vec3f(0, 0, 0), Vec3f(2, 2, 2)), 20U);
    (void)dt.insert(AABB3<f32>(Vec3f(50, 0, 0), Vec3f(52, 2, 2)), 30U);
    // user_data → position lookup table; matches the values we inserted.
    auto lookup = [](u32 ud) -> Vec3f {
        if (ud == 10U) return Vec3f(0, 0, 0);
        if (ud == 20U) return Vec3f(1, 1, 1);
        if (ud == 30U) return Vec3f(51, 1, 1);
        return Vec3f(0, 0, 0);
    };
    RenderBuffer buf(&alloc);
    viz::draw_overlap_pairs_with(buf, dt, lookup);
    REQUIRE(buf.line_count() == 1U); // A↔B
}

TEST_CASE("viz::draw_frustum_cull: kept vs culled per prim", "[geometry][viz][bvh]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "viz-test");
    // Unit-cube frustum (planes inward, x/y/z ∈ [-1, 1]).
    Frustum<f32> fr;
    fr.planes[0] = Plane<f32>(Vec3f(1, 0, 0), 1.0F);
    fr.planes[1] = Plane<f32>(Vec3f(-1, 0, 0), 1.0F);
    fr.planes[2] = Plane<f32>(Vec3f(0, 1, 0), 1.0F);
    fr.planes[3] = Plane<f32>(Vec3f(0, -1, 0), 1.0F);
    fr.planes[4] = Plane<f32>(Vec3f(0, 0, 1), 1.0F);
    fr.planes[5] = Plane<f32>(Vec3f(0, 0, -1), 1.0F);

    // Two prims: one inside the unit cube, one well outside.
    const std::vector<AABB3<f32>> prims = {
        AABB3<f32>(Vec3f(-0.5F, -0.5F, -0.5F), Vec3f(0.5F, 0.5F, 0.5F)), // kept
        AABB3<f32>(Vec3f(10, 10, 10), Vec3f(11, 11, 11)),                  // culled
    };
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    RenderBuffer buf(&alloc);
    viz::draw_frustum_cull(buf, fr, tree, pspan);
    // Each prim emits an AABB wireframe (12 edges) — kept vs culled colour.
    REQUIRE(buf.line_count() == 2U * 12U);
}
