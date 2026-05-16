// crd-geometry-spatial v5e — typed Quantity wrapper round-trip tests.

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/grid_queries_typed.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::spatial::UniformGrid;
using crd::geometry::spatial::UniformGridConfig;
using crd::geometry::spatial::UniformGridRay3T;
using crd::geometry::spatial::uniform_grid_insert;
using crd::geometry::spatial::uniform_grid_overlap;
using crd::geometry::spatial::uniform_grid_radius;
using crd::geometry::spatial::uniform_grid_raycast;
using crd::math::from_raw_vec;
using crd::math::Vec3f;
using crd::units::dim::Length;
using crd::units::Length32;

TEST_CASE("UniformGrid typed insert + overlap round-trip", "[geometry-spatial][grid][typed]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    UniformGrid<f32> g{&alloc, UniformGridConfig<f32>{
        AABB3<f32>{Vec3f{-10, -10, -10}, Vec3f{10, 10, 10}}, 1.0F}};

    AABB3<crd::units::Quantity<Length, f32>> typed_aabb{
        from_raw_vec<Length>(Vec3f{0, 0, 0}),
        from_raw_vec<Length>(Vec3f{1, 1, 1})};
    auto id = uniform_grid_insert<Length, f32>(g, typed_aabb, 99U);
    REQUIRE(id.valid());

    AABB3<crd::units::Quantity<Length, f32>> typed_q{
        from_raw_vec<Length>(Vec3f{-1, -1, -1}),
        from_raw_vec<Length>(Vec3f{2, 2, 2})};
    crd::containers::Array<u32> hits(&alloc);
    uniform_grid_overlap<Length, f32>(g, typed_q, hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 99U);
}

TEST_CASE("UniformGrid typed radius round-trip", "[geometry-spatial][grid][typed]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    UniformGrid<f32> g{&alloc, UniformGridConfig<f32>{
        AABB3<f32>{Vec3f{-10, -10, -10}, Vec3f{10, 10, 10}}, 1.0F}};
    (void)g.insert(AABB3<f32>{Vec3f{0, 0, 0}, Vec3f{0.5F, 0.5F, 0.5F}}, 1U);
    (void)g.insert(AABB3<f32>{Vec3f{5, 5, 5}, Vec3f{5.5F, 5.5F, 5.5F}}, 2U);
    crd::containers::Array<u32> hits(&alloc);
    uniform_grid_radius<Length, f32>(g, from_raw_vec<Length>(Vec3f{0, 0, 0}), Length32{1.0F}, hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 1U);
}

TEST_CASE("UniformGrid typed raycast returns typed t", "[geometry-spatial][grid][typed]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    UniformGrid<f32> g{&alloc, UniformGridConfig<f32>{
        AABB3<f32>{Vec3f{-10, -10, -10}, Vec3f{10, 10, 10}}, 1.0F}};
    (void)g.insert(AABB3<f32>{Vec3f{4.5F, -0.5F, -0.5F}, Vec3f{5.5F, 0.5F, 0.5F}}, 11U);
    UniformGridRay3T<Length, f32> ray{from_raw_vec<Length>(Vec3f{0, 0, 0}), Vec3f{1, 0, 0}};
    auto hit = uniform_grid_raycast<Length, f32>(g, ray);
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 11U);
    REQUIRE(hit->t.value >= 4.4F);
    REQUIRE(hit->t.value <= 4.6F);
}
