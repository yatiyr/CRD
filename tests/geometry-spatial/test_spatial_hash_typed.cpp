// crd-geometry-spatial v5d — typed Quantity wrapper round-trip tests for SpatialHash.

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/hash_queries_typed.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;
using crd::geometry::spatial::SpatialHash;
using crd::geometry::spatial::SpatialHashConfig;
using crd::geometry::spatial::SpatialHashRay3T;
using crd::geometry::spatial::spatial_hash_insert;
using crd::geometry::spatial::spatial_hash_overlap;
using crd::geometry::spatial::spatial_hash_radius;
using crd::geometry::spatial::spatial_hash_raycast;
using crd::math::from_raw_vec;
using crd::math::Vec3f;
using crd::units::dim::Length;
using crd::units::Length32;

TEST_CASE("SpatialHash typed insert + overlap round-trip", "[geometry-spatial][hash][typed]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    SpatialHash<f32> h{&alloc, SpatialHashConfig<f32>{1.0F, 256U}};

    AABB3<crd::units::Quantity<Length, f32>> typed_aabb{
        from_raw_vec<Length>(Vec3f{0, 0, 0}),
        from_raw_vec<Length>(Vec3f{1, 1, 1})};
    auto id = spatial_hash_insert<Length, f32>(h, typed_aabb, 99U);
    REQUIRE(id.valid());

    AABB3<crd::units::Quantity<Length, f32>> typed_q{
        from_raw_vec<Length>(Vec3f{-1, -1, -1}),
        from_raw_vec<Length>(Vec3f{2, 2, 2})};
    crd::containers::Array<u32> hits(&alloc);
    spatial_hash_overlap<Length, f32>(h, typed_q, hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 99U);
}

TEST_CASE("SpatialHash typed radius round-trip", "[geometry-spatial][hash][typed]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    SpatialHash<f32> h{&alloc, SpatialHashConfig<f32>{1.0F, 256U}};
    (void)h.insert(AABB3<f32>{Vec3f{0, 0, 0}, Vec3f{0.5F, 0.5F, 0.5F}}, 1U);
    (void)h.insert(AABB3<f32>{Vec3f{5, 5, 5}, Vec3f{5.5F, 5.5F, 5.5F}}, 2U);

    crd::containers::Array<u32> hits(&alloc);
    spatial_hash_radius<Length, f32>(h, from_raw_vec<Length>(Vec3f{0, 0, 0}), Length32{1.0F}, hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 1U);
}

TEST_CASE("SpatialHash typed raycast returns typed t", "[geometry-spatial][hash][typed]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    SpatialHash<f32> h{&alloc, SpatialHashConfig<f32>{1.0F, 256U}};
    (void)h.insert(AABB3<f32>{Vec3f{4.5F, -0.5F, -0.5F}, Vec3f{5.5F, 0.5F, 0.5F}}, 11U);

    SpatialHashRay3T<Length, f32> ray{from_raw_vec<Length>(Vec3f{0, 0, 0}), Vec3f{1, 0, 0}};
    auto hit = spatial_hash_raycast<Length, f32>(h, ray);
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 11U);
    REQUIRE(hit->t.value >= 4.4F);
    REQUIRE(hit->t.value <= 4.6F);
}
