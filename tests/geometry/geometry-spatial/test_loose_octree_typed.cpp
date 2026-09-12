// crd-geometry-spatial v5b — typed Quantity wrapper round-trip tests for LooseOctree.

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/octree_queries_typed.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::spatial::LooseOctree;
using crd::geometry::spatial::OctreeBuildOptions;
using crd::geometry::spatial::octree_insert;
using crd::geometry::spatial::octree_overlap;
using crd::geometry::spatial::octree_raycast;
using crd::geometry::spatial::OctreeRay3T;
using crd::math::from_raw_vec;
using crd::math::Vec3;
using crd::math::Vec3f;
using crd::units::dim::Length;
using crd::units::Length32;

TEST_CASE("LooseOctree typed insert + overlap matches raw API", "[geometry-spatial][octree][typed]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    OctreeBuildOptions<f32> opts{
        AABB3<f32>{Vec3f{-100, -100, -100}, Vec3f{100, 100, 100}}, 2.0F, 8U, 8U};
    LooseOctree<f32> tree{&alloc, opts};

    // Insert via typed wrapper using AABB3<Quantity<Length, f32>>.
    AABB3<crd::units::Quantity<Length, f32>> typed_aabb{
        from_raw_vec<Length>(Vec3f{0, 0, 0}),
        from_raw_vec<Length>(Vec3f{1, 1, 1})};
    auto h = octree_insert<Length, f32>(tree, typed_aabb, 99U);
    REQUIRE(h.valid());

    // Overlap typed (Array sink form).
    AABB3<crd::units::Quantity<Length, f32>> typed_q{
        from_raw_vec<Length>(Vec3f{-1, -1, -1}),
        from_raw_vec<Length>(Vec3f{2, 2, 2})};
    crd::containers::Array<u32> hits(&alloc);
    octree_overlap<Length, f32>(tree, typed_q, hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 99U);
}

TEST_CASE("LooseOctree typed raycast returns typed t", "[geometry-spatial][octree][typed]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    OctreeBuildOptions<f32> opts{
        AABB3<f32>{Vec3f{-100, -100, -100}, Vec3f{100, 100, 100}}, 2.0F, 8U, 8U};
    LooseOctree<f32> tree{&alloc, opts};

    AABB3<f32> raw{Vec3f{4.5F, -0.5F, -0.5F}, Vec3f{5.5F, 0.5F, 0.5F}};
    (void)tree.insert(raw, 11U);

    OctreeRay3T<Length, f32> typed_ray{
        from_raw_vec<Length>(Vec3f{0, 0, 0}),
        Vec3f{1, 0, 0}};
    auto hit = octree_raycast<Length, f32>(tree, typed_ray);
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 11U);
    // Typed t in metres: should be ~4.5.
    REQUIRE(hit->t.value >= 4.4F);
    REQUIRE(hit->t.value <= 4.6F);
}
