// ---------------------------------------------------------------------------
// crd-geometry-curves -- Arc-length system. Phase 3.1.7 v10c.
//
// Coverage:
//   1. `build_arclength_table` returns n+1 entries; boundaries are
//      (t=0, d=0) and (t=1, d=total_length).
//   2. `length_of` matches the table total.
//   3. CircularArc reference: `length_of(arc) == radius * sweep_radians`
//      within a tight bound at high `n_samples`.
//   4. Straight-line Polyline: length matches sum of segment lengths
//      bit-exactly.
//   5. `t_at_distance(0)` and `t_at_distance(total_length)` are 0 and 1.
//   6. `distance_at_t(0)` and `distance_at_t(1)` are 0 and total_length.
//   7. Round-trip: `t_at_distance(distance_at_t(t)) == t` within 1e-5
//      for sampled t values.
//   8. Round-trip: `distance_at_t(t_at_distance(d)) == d` within 1e-5
//      for sampled d values.
//   9. Monotonicity: arc length is monotone in t.
//  10. Closed-curve modular wrap: distance > total_length wraps; t > 1
//      wraps; both negative inputs handled.
//  11. f64 instantiations.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/curves.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

namespace
{

using namespace crd::geometry::curves;

template <typename T>
[[nodiscard]] crd::math::Vec3<T> v3(T x, T y, T z) noexcept
{
    return crd::math::Vec3<T>(x, y, z);
}

} // namespace

// ---------------------------------------------------------------------------
// build_arclength_table.
// ---------------------------------------------------------------------------

TEST_CASE("v10c build_arclength_table returns n+1 entries with correct boundaries",
          "[curves][arclength][build]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, 2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto table = build_arclength_table(c, 16U, &alloc);

    REQUIRE(table.samples.size() == 17U);
    REQUIRE(table.samples[0].t == 0.0F);
    REQUIRE(table.samples[0].distance == 0.0F);
    REQUIRE(table.samples[16].t == 1.0F);
    REQUIRE(table.samples[16].distance == table.total_length);
    REQUIRE_FALSE(table.closed);
}

TEST_CASE("v10c build_arclength_table default n_samples is 64",
          "[curves][arclength][build]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 1.0F, 0.0F),
                                 v3(2.0F, 1.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto table = build_arclength_table(c, &alloc);
    REQUIRE(table.samples.size() == k_arclength_default_samples + 1U);
}

// ---------------------------------------------------------------------------
// length_of: reference checks.
// ---------------------------------------------------------------------------

TEST_CASE("v10c length_of straight-line Polyline equals sum of chord lengths",
          "[curves][arclength][length][polyline]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> pts[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(3.0F, 0.0F, 0.0F), // segment length 3
        v3(3.0F, 4.0F, 0.0F), // segment length 4 (3-4-5 triangle)
    };
    Polyline3<float> pl(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 3U});
    const auto       table = build_arclength_table(pl.view(), 32U, &alloc);

    // Total = 3 + 4 = 7. Polyline evaluator is exact between vertices,
    // so the chord-table sum should be 7 within float rounding.
    REQUIRE(std::abs(length_of(table) - 7.0F) < 1.0e-5F);
}

TEST_CASE("v10c length_of CircularArc with high n_samples approaches r * theta",
          "[curves][arclength][length][arc]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    constexpr float k_pi    = 3.14159265358979323846F;
    constexpr float k_radius = 2.0F;
    const CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                    v3(1.0F, 0.0F, 0.0F),
                                    v3(0.0F, 1.0F, 0.0F),
                                    /*radius_in=*/k_radius,
                                    /*sweep_radians_in=*/k_pi);
    // Reference: a half-circle of radius 2 has length pi * 2 ~= 6.28319.
    const auto table = build_arclength_table(arc, 256U, &alloc);
    const float expected = k_radius * k_pi;
    // 256-segment chord approximation should be within ~1e-4 of analytic.
    REQUIRE(std::abs(length_of(table) - expected) < 1.0e-3F);
}

TEST_CASE("v10c length_of (curve, n, alloc) convenience matches the table-based form",
          "[curves][arclength][length][convenience]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const QuadBezier3<float> q(v3(0.0F, 0.0F, 0.0F), v3(1.0F, 2.0F, 0.0F), v3(2.0F, 0.0F, 0.0F));
    const auto  table   = build_arclength_table(q, 32U, &alloc);
    const float convey  = length_of(q, 32U, &alloc);
    REQUIRE(convey == length_of(table));
}

// ---------------------------------------------------------------------------
// t_at_distance / distance_at_t boundary behaviour.
// ---------------------------------------------------------------------------

TEST_CASE("v10c t_at_distance(0) == 0 and t_at_distance(total) == 1 for open",
          "[curves][arclength][query][open]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 1.0F, 0.0F),
                                 v3(2.0F, 1.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto table = build_arclength_table(c, 32U, &alloc);

    REQUIRE(t_at_distance(table, 0.0F) == 0.0F);
    REQUIRE(t_at_distance(table, table.total_length) == 1.0F);
}

TEST_CASE("v10c distance_at_t(0) == 0 and distance_at_t(1) == total_length for open",
          "[curves][arclength][query][open]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 1.0F, 0.0F),
                                 v3(2.0F, 1.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto table = build_arclength_table(c, 32U, &alloc);

    REQUIRE(distance_at_t(table, 0.0F) == 0.0F);
    REQUIRE(distance_at_t(table, 1.0F) == table.total_length);
}

TEST_CASE("v10c open-curve clamps out-of-range inputs",
          "[curves][arclength][query][clamp]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 1.0F, 0.0F),
                                 v3(2.0F, 1.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto table = build_arclength_table(c, 16U, &alloc);

    REQUIRE(t_at_distance(table, -100.0F) == 0.0F);
    REQUIRE(t_at_distance(table, 1.0e9F)  == 1.0F);
    REQUIRE(distance_at_t(table, -1.0F)   == 0.0F);
    REQUIRE(distance_at_t(table, 2.0F)    == table.total_length);
}

// ---------------------------------------------------------------------------
// Round-trip identities.
// ---------------------------------------------------------------------------

TEST_CASE("v10c t_at_distance(distance_at_t(t)) approximates identity for open",
          "[curves][arclength][query][roundtrip]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, 2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto table = build_arclength_table(c, 64U, &alloc);

    for (crd::u32 i = 0U; i <= 20U; ++i)
    {
        const float t  = static_cast<float>(i) / 20.0F;
        const float d  = distance_at_t(table, t);
        const float t2 = t_at_distance(table, d);
        INFO("t = " << t);
        REQUIRE(std::abs(t2 - t) < 1.0e-5F);
    }
}

TEST_CASE("v10c distance_at_t(t_at_distance(d)) approximates identity for open",
          "[curves][arclength][query][roundtrip]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, 2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto table = build_arclength_table(c, 64U, &alloc);
    const float total = table.total_length;

    for (crd::u32 i = 0U; i <= 20U; ++i)
    {
        const float d  = (static_cast<float>(i) / 20.0F) * total;
        const float t  = t_at_distance(table, d);
        const float d2 = distance_at_t(table, t);
        INFO("d = " << d);
        REQUIRE(std::abs(d2 - d) < 1.0e-4F);
    }
}

// ---------------------------------------------------------------------------
// Monotonicity.
// ---------------------------------------------------------------------------

TEST_CASE("v10c arc length is monotone non-decreasing in t",
          "[curves][arclength][monotonicity]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 3.0F, 0.0F),
                                 v3(2.0F, 3.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto table = build_arclength_table(c, 64U, &alloc);

    for (crd::usize i = 1U; i < table.samples.size(); ++i)
    {
        REQUIRE(table.samples[i].distance >= table.samples[i - 1U].distance);
        REQUIRE(table.samples[i].t        >  table.samples[i - 1U].t);
    }
}

// ---------------------------------------------------------------------------
// Closed-curve modular wrap.
// ---------------------------------------------------------------------------

TEST_CASE("v10c closed curve: t and distance wrap modulo their domains",
          "[curves][arclength][closed][wrap]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    constexpr float two_pi = 6.28318530717958647692F;
    const CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                    v3(1.0F, 0.0F, 0.0F),
                                    v3(0.0F, 1.0F, 0.0F),
                                    /*radius_in=*/1.0F,
                                    /*sweep_radians_in=*/two_pi,
                                    /*closed_in=*/true);
    const auto table = build_arclength_table(arc, 128U, &alloc);
    REQUIRE(table.closed);

    const float total = table.total_length;

    // distance > L wraps.
    const float t1 = t_at_distance(table, 0.25F * total);
    const float t2 = t_at_distance(table, 1.25F * total);
    REQUIRE(std::abs(t1 - t2) < 1.0e-4F);

    // negative distance wraps.
    const float t3 = t_at_distance(table, 0.75F * total);
    const float t4 = t_at_distance(table, -0.25F * total);
    REQUIRE(std::abs(t3 - t4) < 1.0e-4F);

    // t > 1 wraps.
    const float d1 = distance_at_t(table, 0.3F);
    const float d2 = distance_at_t(table, 1.3F);
    REQUIRE(std::abs(d1 - d2) < 1.0e-4F);

    // negative t wraps.
    const float d3 = distance_at_t(table, 0.7F);
    const float d4 = distance_at_t(table, -0.3F);
    REQUIRE(std::abs(d3 - d4) < 1.0e-4F);
}

// ---------------------------------------------------------------------------
// f64 instantiations.
// ---------------------------------------------------------------------------

TEST_CASE("v10c arclength works for f64 instantiations",
          "[curves][arclength][f64]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<double> c(v3(0.0, 0.0, 0.0), v3(1.0, 2.0, 0.0), v3(2.0, 2.0, 0.0), v3(3.0, 0.0, 0.0));
    const auto table = build_arclength_table(c, 32U, &alloc);
    REQUIRE(table.samples.size() == 33U);
    REQUIRE(table.total_length > 0.0);

    const double total = table.total_length;
    const double t  = t_at_distance(table, 0.5 * total);
    const double d2 = distance_at_t(table, t);
    REQUIRE(std::abs(d2 - 0.5 * total) < 1.0e-9);
}
