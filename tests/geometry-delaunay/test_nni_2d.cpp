// Tests for crd-geometry-delaunay v8f Sibson NNI 2D.
//
// Coverage:
//   - Diagnostic statuses propagated from Delaunay + new NNI-specific ones
//     (QueryNonFinite, OutsideHull, OnSite).
//   - **Site-coincident query** returns that site's exact value
//     (status OnSite).
//   - **Linear-function reproduction**: NNI of `f(x, y) = ax + by + c` at
//     any interior query returns the EXACT linear value (Sibson's key
//     mathematical property — interpolating a linear field gives back
//     the same linear field).
//   - **Convex-hull boundedness**: NNI result is always in [min(value),
//     max(value)] for any interior query.
//   - **Continuity** (probe near a site): result near site `i` is close
//     to `value[i]` (continuity at sites).
//   - One-shot functional form matches the class form.
//   - Many-query reuse on cached interpolator (smoke test for
//     correctness across multiple queries).
//   - OutsideHull detection on a clearly-outside query.
//   - Determinism (same query on same input -> same value byte-for-byte).
//   - f64 precision tier.

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/nni_2d.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec2;
using crd::geometry::delaunay::NniInterpolator2;
using crd::geometry::delaunay::NniStatus;
using crd::geometry::delaunay::sibson_interpolate_2d;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{16U * 1024U * 1024U, nullptr, "nni2d-test-arena"};
};

} // anonymous namespace

TEST_CASE("sibson_interpolate_2d: < 3 sites -> TooFewPoints",
          "[geometry-delaunay][nni][v8f]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    crd::containers::Array<f32> vals(&f.alloc);
    vals.push_back(0.0F);
    vals.push_back(1.0F);
    auto r = sibson_interpolate_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f32>{vals.data(), vals.size()},
        Vec2<f32>{0.5F, 0.0F}, &f.alloc);
    CHECK(r.status == NniStatus::TooFewPoints);
}

TEST_CASE("sibson_interpolate_2d: query non-finite -> QueryNonFinite",
          "[geometry-delaunay][nni][v8f]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{0, 1});
    pts.push_back(Vec2<f32>{1, 1});
    crd::containers::Array<f32> vals(&f.alloc);
    vals.push_back(0.0F);
    vals.push_back(1.0F);
    vals.push_back(2.0F);
    vals.push_back(3.0F);
    auto r = sibson_interpolate_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f32>{vals.data(), vals.size()},
        Vec2<f32>{std::numeric_limits<f32>::infinity(), 0.0F}, &f.alloc);
    CHECK(r.status == NniStatus::QueryNonFinite);
}

TEST_CASE("sibson_interpolate_2d: query outside hull -> OutsideHull",
          "[geometry-delaunay][nni][v8f]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{0, 1});
    pts.push_back(Vec2<f32>{1, 1});
    crd::containers::Array<f32> vals(&f.alloc);
    vals.push_back(0.0F);
    vals.push_back(1.0F);
    vals.push_back(2.0F);
    vals.push_back(3.0F);
    auto r = sibson_interpolate_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f32>{vals.data(), vals.size()},
        Vec2<f32>{5.0F, 5.0F}, &f.alloc); // far outside
    CHECK(r.status == NniStatus::OutsideHull);
}

TEST_CASE("sibson_interpolate_2d: query exactly at a site returns that site's value",
          "[geometry-delaunay][nni][v8f][on-site]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{2, 0});
    pts.push_back(Vec2<f32>{0, 2});
    pts.push_back(Vec2<f32>{2, 2});
    pts.push_back(Vec2<f32>{1, 1});
    crd::containers::Array<f32> vals(&f.alloc);
    vals.push_back(10.0F);
    vals.push_back(20.0F);
    vals.push_back(30.0F);
    vals.push_back(40.0F);
    vals.push_back(99.0F); // distinct
    auto r = sibson_interpolate_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f32>{vals.data(), vals.size()},
        Vec2<f32>{1, 1}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.status == NniStatus::OnSite);
    CHECK(r.value == 99.0F);
}

TEST_CASE("sibson_interpolate_2d: linear field reproduction (Sibson's hallmark)",
          "[geometry-delaunay][nni][v8f][linear-reproduction]")
{
    // For f(x, y) = a*x + b*y + c on input sites, NNI at ANY interior
    // query must return f(q.x, q.y) EXACTLY (up to FP precision). This is
    // Sibson's mathematical guarantee.
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{0.0, 0.0});
    pts.push_back(Vec2<f64>{3.0, 0.0});
    pts.push_back(Vec2<f64>{0.0, 3.0});
    pts.push_back(Vec2<f64>{3.0, 3.0});
    pts.push_back(Vec2<f64>{1.0, 1.0});
    pts.push_back(Vec2<f64>{2.0, 1.0});
    pts.push_back(Vec2<f64>{1.0, 2.0});
    pts.push_back(Vec2<f64>{2.0, 2.0});

    constexpr f64 kA = 2.5;
    constexpr f64 kB = -1.7;
    constexpr f64 kC = 5.0;

    crd::containers::Array<f64> vals(&f.alloc);
    for (const auto& p : pts)
    {
        vals.push_back(kA * p.x + kB * p.y + kC);
    }

    NniInterpolator2<f64> interp{
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f64>{vals.data(), vals.size()},
        &f.alloc};
    REQUIRE(interp.build_status() == NniStatus::Ok);

    // Interior queries; Sibson must reproduce f(q) exactly.
    const Vec2<f64> queries[] = {
        {1.5, 1.5}, {1.2, 1.7}, {2.1, 1.3}, {1.8, 2.4}, {1.0, 1.5},
    };
    for (const auto& q : queries)
    {
        auto r = interp.interpolate(q);
        REQUIRE(r.ok());
        const f64 expected = kA * q.x + kB * q.y + kC;
        CHECK(std::abs(r.value - expected) < 1.0e-8);
    }
}

TEST_CASE("sibson_interpolate_2d: convex-hull boundedness",
          "[geometry-delaunay][nni][v8f][bounded]")
{
    // NNI result must be in [min(value), max(value)] for any interior query.
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{4, 0});
    pts.push_back(Vec2<f32>{0, 4});
    pts.push_back(Vec2<f32>{4, 4});
    pts.push_back(Vec2<f32>{1, 1});
    pts.push_back(Vec2<f32>{3, 1});
    pts.push_back(Vec2<f32>{1, 3});
    pts.push_back(Vec2<f32>{3, 3});
    pts.push_back(Vec2<f32>{2, 2});

    crd::containers::Array<f32> vals(&f.alloc);
    vals.push_back(1.0F);
    vals.push_back(2.0F);
    vals.push_back(3.0F);
    vals.push_back(4.0F);
    vals.push_back(5.0F);
    vals.push_back(6.0F);
    vals.push_back(7.0F);
    vals.push_back(8.0F);
    vals.push_back(9.0F);

    NniInterpolator2<f32> interp{
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f32>{vals.data(), vals.size()},
        &f.alloc};
    REQUIRE(interp.build_status() == NniStatus::Ok);

    const f32 vmin = 1.0F;
    const f32 vmax = 9.0F;
    // Probe many interior points.
    for (u32 j = 1; j <= 7; ++j)
    {
        for (u32 i = 1; i <= 7; ++i)
        {
            const Vec2<f32> q{static_cast<f32>(i) * 0.5F, static_cast<f32>(j) * 0.5F};
            auto r = interp.interpolate(q);
            if (r.ok())
            {
                CHECK(r.value >= vmin - 1.0e-3F);
                CHECK(r.value <= vmax + 1.0e-3F);
            }
        }
    }
}

TEST_CASE("sibson_interpolate_2d: continuity near a site",
          "[geometry-delaunay][nni][v8f][continuity]")
{
    // Query near site i should give value close to value[i].
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{0, 0});
    pts.push_back(Vec2<f64>{2, 0});
    pts.push_back(Vec2<f64>{0, 2});
    pts.push_back(Vec2<f64>{2, 2});
    pts.push_back(Vec2<f64>{1, 1});

    crd::containers::Array<f64> vals(&f.alloc);
    vals.push_back(10.0);
    vals.push_back(20.0);
    vals.push_back(30.0);
    vals.push_back(40.0);
    vals.push_back(99.0);

    NniInterpolator2<f64> interp{
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f64>{vals.data(), vals.size()},
        &f.alloc};
    REQUIRE(interp.build_status() == NniStatus::Ok);

    // Probe at (1, 1) + small offset.
    const Vec2<f64> near_centre{1.0 + 1.0e-4, 1.0 + 1.0e-4};
    auto r = interp.interpolate(near_centre);
    REQUIRE(r.ok());
    CHECK(std::abs(r.value - 99.0) < 0.1);
}

TEST_CASE("sibson_interpolate_2d: functional form matches class form",
          "[geometry-delaunay][nni][v8f]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{0, 0});
    pts.push_back(Vec2<f64>{3, 0});
    pts.push_back(Vec2<f64>{0, 3});
    pts.push_back(Vec2<f64>{3, 3});
    pts.push_back(Vec2<f64>{1.5, 1.5});

    crd::containers::Array<f64> vals(&f.alloc);
    vals.push_back(0.0);
    vals.push_back(3.0);
    vals.push_back(3.0);
    vals.push_back(6.0);
    vals.push_back(3.0);

    NniInterpolator2<f64> interp{
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f64>{vals.data(), vals.size()},
        &f.alloc};
    REQUIRE(interp.build_status() == NniStatus::Ok);

    const Vec2<f64> q{1.0, 1.0};
    auto r_class = interp.interpolate(q);
    auto r_func = sibson_interpolate_2d<f64>(
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f64>{vals.data(), vals.size()}, q, &f.alloc);
    REQUIRE(r_class.ok());
    REQUIRE(r_func.ok());
    CHECK(r_class.value == r_func.value);
}

TEST_CASE("sibson_interpolate_2d: many-query reuse on cached interpolator",
          "[geometry-delaunay][nni][v8f]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{0, 0});
    pts.push_back(Vec2<f64>{3, 0});
    pts.push_back(Vec2<f64>{0, 3});
    pts.push_back(Vec2<f64>{3, 3});
    pts.push_back(Vec2<f64>{1.5, 1.5});

    crd::containers::Array<f64> vals(&f.alloc);
    for (u32 i = 0; i < 5; ++i) { vals.push_back(static_cast<f64>(i)); }

    NniInterpolator2<f64> interp{
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f64>{vals.data(), vals.size()},
        &f.alloc};
    REQUIRE(interp.build_status() == NniStatus::Ok);

    // Run many queries; verify each returns a valid bounded value.
    u32 ok_count = 0;
    for (u32 j = 1; j <= 5; ++j)
    {
        for (u32 i = 1; i <= 5; ++i)
        {
            const Vec2<f64> q{static_cast<f64>(i) * 0.5, static_cast<f64>(j) * 0.5};
            auto r = interp.interpolate(q);
            if (r.ok())
            {
                CHECK(r.value >= 0.0);
                CHECK(r.value <= 4.0);
                ++ok_count;
            }
        }
    }
    CHECK(ok_count >= 10U); // most interior queries should succeed
}

TEST_CASE("sibson_interpolate_2d: determinism",
          "[geometry-delaunay][nni][v8f][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{0, 0});
    pts.push_back(Vec2<f64>{3, 0});
    pts.push_back(Vec2<f64>{0, 3});
    pts.push_back(Vec2<f64>{3, 3});
    pts.push_back(Vec2<f64>{1, 1});
    pts.push_back(Vec2<f64>{2, 1});
    pts.push_back(Vec2<f64>{1, 2});
    pts.push_back(Vec2<f64>{2, 2});

    crd::containers::Array<f64> vals(&f.alloc);
    for (u32 i = 0; i < 8; ++i) { vals.push_back(static_cast<f64>(i) * 0.5); }

    NniInterpolator2<f64> interp{
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<f64>{vals.data(), vals.size()},
        &f.alloc};
    REQUIRE(interp.build_status() == NniStatus::Ok);

    const Vec2<f64> q{1.5, 1.5};
    auto r1 = interp.interpolate(q);
    auto r2 = interp.interpolate(q);
    auto r3 = interp.interpolate(q);
    REQUIRE(r1.ok());
    REQUIRE(r2.ok());
    REQUIRE(r3.ok());
    CHECK(r1.value == r2.value);
    CHECK(r2.value == r3.value);
}

TEST_CASE("sibson_interpolate_2d: f64 + f32 precision tiers",
          "[geometry-delaunay][nni][v8f][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts32(&f.alloc);
    pts32.push_back(Vec2<f32>{0, 0});
    pts32.push_back(Vec2<f32>{2, 0});
    pts32.push_back(Vec2<f32>{0, 2});
    pts32.push_back(Vec2<f32>{2, 2});
    pts32.push_back(Vec2<f32>{1, 1});
    crd::containers::Array<f32> vals32(&f.alloc);
    vals32.push_back(0.0F);
    vals32.push_back(2.0F);
    vals32.push_back(2.0F);
    vals32.push_back(4.0F);
    vals32.push_back(2.0F);
    auto r32 = sibson_interpolate_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts32.data(), pts32.size()},
        crd::containers::ConstSpan<f32>{vals32.data(), vals32.size()},
        Vec2<f32>{0.5F, 0.5F}, &f.alloc);
    CHECK(r32.ok());
    // f(x,y) = x + y; linear reproduction => f(0.5, 0.5) = 1.0
    CHECK(std::abs(r32.value - 1.0F) < 1.0e-3F);
}
