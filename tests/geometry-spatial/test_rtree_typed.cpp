// crd-geometry-spatial v5c — typed Quantity wrapper round-trip tests for RTree.

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/rtree_queries_typed.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::spatial::rtree_insert;
using crd::geometry::spatial::rtree_overlap;
using crd::geometry::spatial::rtree_raycast;
using crd::geometry::spatial::RTree;
using crd::geometry::spatial::RTreeRay3T;
using crd::math::from_raw_vec;
using crd::math::Vec3f;
using crd::units::dim::Length;
using crd::units::Length32;

TEST_CASE("RTree typed insert + overlap round-trip", "[geometry-spatial][rtree][typed]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    RTree<f32> tree{&alloc};

    AABB3<crd::units::Quantity<Length, f32>> typed_aabb{
        from_raw_vec<Length>(Vec3f{0, 0, 0}),
        from_raw_vec<Length>(Vec3f{1, 1, 1})};
    auto h = rtree_insert<Length, f32>(tree, typed_aabb, 99U);
    REQUIRE(h.valid());

    AABB3<crd::units::Quantity<Length, f32>> typed_q{
        from_raw_vec<Length>(Vec3f{-1, -1, -1}),
        from_raw_vec<Length>(Vec3f{2, 2, 2})};
    crd::containers::Array<u32> hits(&alloc);
    rtree_overlap<Length, f32>(tree, typed_q, hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 99U);
}

TEST_CASE("RTree typed raycast returns typed t", "[geometry-spatial][rtree][typed]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    RTree<f32> tree{&alloc};
    (void)tree.insert(AABB3<f32>{Vec3f{4.5F, -0.5F, -0.5F}, Vec3f{5.5F, 0.5F, 0.5F}}, 11U);

    RTreeRay3T<Length, f32> ray{from_raw_vec<Length>(Vec3f{0, 0, 0}), Vec3f{1, 0, 0}};
    auto hit = rtree_raycast<Length, f32>(tree, ray);
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 11U);
    REQUIRE(hit->t.value >= 4.4F);
    REQUIRE(hit->t.value <= 4.6F);
}
