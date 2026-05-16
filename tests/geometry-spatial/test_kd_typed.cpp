// crd-geometry-spatial v5a — typed Quantity wrapper round-trip tests.
//
// Verifies the strip-compute-retag wrappers in `kd_queries_typed.hpp` produce
// the same result as the raw API when the typed view + raw span are bridged
// from the same backing buffer (ADR-0078 §2 D2 layout pin).

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/kd_queries_typed.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <catch2/catch_test_macros.hpp>

#include <random>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::spatial::kd_build;
using crd::geometry::spatial::kd_nearest_n;
using crd::geometry::spatial::KdNeighbor;
using crd::geometry::spatial::KdNeighborT;
using crd::geometry::spatial::KdRadiusHit;
using crd::geometry::spatial::KdRadiusHitT;
using crd::geometry::spatial::kd_radius;
using crd::math::from_raw_vec;
using crd::math::Vec3;
using crd::math::Vec3f;
using crd::units::dim::Length;
using crd::units::Length32;
using crd::units::Quantity;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 20}; };

crd::containers::Array<Vec3f> make_cloud(u32 n, u32 seed, crd::memory::IAllocator* a)
{
    crd::containers::Array<Vec3f> pts(a);
    pts.reserve(n);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u(-1.0F, 1.0F);
    for (u32 i = 0; i < n; ++i) { pts.push_back(Vec3f{u(rng), u(rng), u(rng)}); }
    return pts;
}
} // namespace

TEST_CASE("kd_radius typed wrapper matches raw call", "[geometry-spatial][kd][typed]")
{
    AllocFixture f{};
    auto raw_pts = make_cloud(300U, 13U, &f.alloc);

    // Typed view bridged from the same backing buffer.
    crd::containers::Array<Vec3<Length32>> typed_pts(&f.alloc);
    typed_pts.reserve(raw_pts.size());
    for (const auto& p : raw_pts) { typed_pts.push_back(from_raw_vec<Length>(p)); }

    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{raw_pts.data(), raw_pts.size()},
                                &f.alloc);

    const Vec3f q_raw{0.1F, -0.2F, 0.3F};
    const auto q_typed = from_raw_vec<Length>(q_raw);
    const f32 r_raw = 0.4F;
    const Length32 r_typed{r_raw};

    crd::containers::Array<KdRadiusHit<f32>> raw_out(&f.alloc);
    kd_radius<f32>(tree, crd::containers::ConstSpan<Vec3f>{raw_pts.data(), raw_pts.size()},
                    q_raw, r_raw, raw_out);

    crd::containers::Array<KdRadiusHitT<Length, f32>> typed_out(&f.alloc);
    kd_radius<Length, f32>(tree,
                            crd::containers::ConstSpan<Vec3<Length32>>{typed_pts.data(), typed_pts.size()},
                            crd::containers::ConstSpan<Vec3f>{raw_pts.data(), raw_pts.size()},
                            q_typed, r_typed, typed_out);

    REQUIRE(raw_out.size() == typed_out.size());
    for (usize i = 0; i < raw_out.size(); ++i)
    {
        REQUIRE(raw_out[i].payload == typed_out[i].payload);
        REQUIRE(raw_out[i].distance_squared == typed_out[i].distance_squared.value);
    }
}

TEST_CASE("kd_nearest_n typed wrapper matches raw call", "[geometry-spatial][kd][typed]")
{
    AllocFixture f{};
    auto raw_pts = make_cloud(200U, 71U, &f.alloc);
    crd::containers::Array<Vec3<Length32>> typed_pts(&f.alloc);
    typed_pts.reserve(raw_pts.size());
    for (const auto& p : raw_pts) { typed_pts.push_back(from_raw_vec<Length>(p)); }

    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{raw_pts.data(), raw_pts.size()},
                                &f.alloc);
    const Vec3f q_raw{0.05F, 0.15F, -0.1F};
    const auto q_typed = from_raw_vec<Length>(q_raw);

    crd::containers::Array<KdNeighbor<f32>> raw_out(&f.alloc);
    kd_nearest_n<f32>(tree, crd::containers::ConstSpan<Vec3f>{raw_pts.data(), raw_pts.size()},
                        q_raw, 10U, raw_out);

    crd::containers::Array<KdNeighborT<Length, f32>> typed_out(&f.alloc);
    kd_nearest_n<Length, f32>(tree,
                                crd::containers::ConstSpan<Vec3<Length32>>{typed_pts.data(), typed_pts.size()},
                                crd::containers::ConstSpan<Vec3f>{raw_pts.data(), raw_pts.size()},
                                q_typed, 10U, typed_out);

    REQUIRE(raw_out.size() == typed_out.size());
    for (usize i = 0; i < raw_out.size(); ++i)
    {
        REQUIRE(raw_out[i].payload == typed_out[i].payload);
        REQUIRE(raw_out[i].distance_squared == typed_out[i].distance_squared.value);
    }
}
