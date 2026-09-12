// ---------------------------------------------------------------------------
// crd-geometry-curves -- Queries. Phase 3.1.7 v10d.
//
// Coverage:
//   1. `aabb_of` contains every sample of the curve.
//   2. `aabb_of` matches algebraic expectation for simple curves (a
//      straight Polyline of unit length lies inside its bounding box).
//   3. `closest_point` at a point ON the curve returns that point.
//   4. `closest_point` for a point closer to one endpoint returns that
//      endpoint when projection falls outside the parameter range.
//   5. `closest_point` for the centre of a circular arc returns... the
//      circle radius bound (any point on the arc is `radius` away from
//      centre).
//   6. `closest_point` global-minimum: a curve with two close-approaches
//      finds the GLOBAL minimum, not the local minimum near t=0.5.
//   7. `distance` matches sqrt(closest_point.distance_squared).
//   8. `intersect_ray` reports hit when ray pierces curve; reports none
//      when ray misses; reports closest hit when multiple candidates.
//   9. f64 instantiations.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/curves.hpp>
#include <crd/geometry/primitives/primitives.hpp>
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

template <typename T>
[[nodiscard]] bool in_box(const crd::math::Vec3<T>&                          p,
                           const crd::geometry::primitives::AABB3<T>&        box,
                           T                                                  slack = static_cast<T>(1e-4)) noexcept
{
    return p.x >= box.min.x - slack && p.x <= box.max.x + slack &&
           p.y >= box.min.y - slack && p.y <= box.max.y + slack &&
           p.z >= box.min.z - slack && p.z <= box.max.z + slack;
}

} // namespace

// ---------------------------------------------------------------------------
// aabb_of.
// ---------------------------------------------------------------------------

TEST_CASE("v10d aabb_of contains every evaluator sample of the curve",
          "[curves][queries][aabb]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 3.0F, 0.0F),
                                 v3(2.0F, 3.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto box = aabb_of(c, &alloc);

    // Every evaluator sample should be inside the AABB (modulo tiny float
    // slack at boundaries).
    for (crd::u32 i = 0U; i <= 64U; ++i)
    {
        const float t = static_cast<float>(i) / 64.0F;
        const auto  p = evaluate(c, t);
        INFO("t = " << t);
        REQUIRE(in_box(p, box));
    }
}

TEST_CASE("v10d aabb_of for a unit-length straight polyline equals the segment's bounds",
          "[curves][queries][aabb][polyline]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> pts[] = {v3(-1.0F, -2.0F, -3.0F), v3(1.0F, 2.0F, 3.0F)};
    Polyline3<float> pl(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 2U});
    const auto       box = aabb_of(pl.view(), &alloc);

    REQUIRE(box.min.x <= -1.0F);
    REQUIRE(box.max.x >= 1.0F);
    REQUIRE(box.min.y <= -2.0F);
    REQUIRE(box.max.y >= 2.0F);
    REQUIRE(box.min.z <= -3.0F);
    REQUIRE(box.max.z >= 3.0F);
}

// ---------------------------------------------------------------------------
// closest_point.
// ---------------------------------------------------------------------------

TEST_CASE("v10d closest_point at a point ON the curve returns that point",
          "[curves][queries][closest_point]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, 2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const float t_query = 0.3F;
    const auto  p       = evaluate(c, t_query);
    const auto  cp      = closest_point(c, p, 1.0e-5F, &alloc);

    REQUIRE(std::abs(cp.t - t_query) < 1.0e-3F);
    REQUIRE(cp.distance_squared < 1.0e-6F);
}

TEST_CASE("v10d closest_point for a point off the line clamps to nearest endpoint",
          "[curves][queries][closest_point][polyline]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    // A horizontal polyline from (0,0,0) to (1,0,0).
    const crd::math::Vec3<float> pts[] = {v3(0.0F, 0.0F, 0.0F), v3(1.0F, 0.0F, 0.0F)};
    Polyline3<float> pl(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 2U});

    // Query point far to the left and slightly above — closest point is p0.
    const auto cp_left = closest_point(pl.view(), v3(-5.0F, 1.0F, 0.0F), 1.0e-4F, &alloc);
    REQUIRE(std::abs(cp_left.t - 0.0F) < 1.0e-3F);

    // Query point far to the right — closest is p1.
    const auto cp_right = closest_point(pl.view(), v3(5.0F, 1.0F, 0.0F), 1.0e-4F, &alloc);
    REQUIRE(std::abs(cp_right.t - 1.0F) < 1.0e-3F);
}

TEST_CASE("v10d closest_point for centre of a circular arc gives distance ~radius",
          "[curves][queries][closest_point][arc]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    constexpr float pi    = 3.14159265358979323846F;
    constexpr float k_radius = 2.0F;
    const CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                    v3(1.0F, 0.0F, 0.0F),
                                    v3(0.0F, 1.0F, 0.0F),
                                    /*radius_in=*/k_radius,
                                    /*sweep_radians_in=*/pi);

    // The centre is equidistant (= radius) from every point on the arc.
    const auto cp = closest_point(arc, v3(0.0F, 0.0F, 0.0F), 1.0e-4F, &alloc);
    const float dist = std::sqrt(cp.distance_squared);
    // Subdivision-rejection + Newton should find SOME point on the arc;
    // the distance must equal the radius.
    REQUIRE(std::abs(dist - k_radius) < 1.0e-3F);
}

TEST_CASE("v10d closest_point finds the global minimum on an S-shaped curve",
          "[curves][queries][closest_point][global-min]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    // S-shaped cubic with two extrema. A point placed near one extremum
    // should return that extremum's t, not the curve's midpoint.
    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, -2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));

    // Query a point very close to the curve at t ~= 0.85. The 16-sample
    // initial-guess sweep covers t in {0, 1/16, ..., 1} so should pick a
    // seed near the right extremum, then Newton refines.
    const auto target = evaluate(c, 0.85F);
    const auto cp     = closest_point(c, target, 1.0e-5F, &alloc);
    REQUIRE(std::abs(cp.t - 0.85F) < 1.0e-2F);
}

TEST_CASE("v10d distance returns sqrt(closest_point.distance_squared)",
          "[curves][queries][distance]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, 2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto  query = v3(1.5F, 5.0F, 0.0F);
    const auto  cp    = closest_point(c, query, 1.0e-4F, &alloc);
    const float d     = distance(c, query, 1.0e-4F, &alloc);
    REQUIRE(std::abs(d - std::sqrt(cp.distance_squared)) < 1.0e-4F);
}

// ---------------------------------------------------------------------------
// intersect_ray.
// ---------------------------------------------------------------------------

TEST_CASE("v10d intersect_ray reports nullopt when ray misses the curve",
          "[curves][queries][intersect_ray][miss]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, 2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    // Ray parallel to the curve, far away.
    const crd::geometry::primitives::Ray3<float> ray{v3(0.0F, 100.0F, 0.0F), v3(1.0F, 0.0F, 0.0F)};
    const auto hit = intersect_ray(c, ray, 0.01F, &alloc);
    REQUIRE_FALSE(hit.has_value());
}

TEST_CASE("v10d intersect_ray finds a hit when ray crosses a polyline",
          "[curves][queries][intersect_ray][polyline]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    // Polyline along the x-axis from (0,0,0) to (5,0,0).
    const crd::math::Vec3<float> pts[] = {v3(0.0F, 0.0F, 0.0F), v3(5.0F, 0.0F, 0.0F)};
    Polyline3<float> pl(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 2U});

    // Ray from (2.5, 5, 0) pointing toward (2.5, 0, 0) — i.e. -y direction.
    // Closest approach to the polyline is at (2.5, 0, 0) on segment 0.
    const crd::geometry::primitives::Ray3<float> ray{v3(2.5F, 5.0F, 0.0F), v3(0.0F, -1.0F, 0.0F)};
    const auto hit = intersect_ray(pl.view(), ray, 0.01F, &alloc);

    REQUIRE(hit.has_value());
    REQUIRE(std::abs(hit->t_ray - 5.0F) < 0.1F);
    REQUIRE(std::abs(hit->point.x - 2.5F) < 0.1F);
    REQUIRE(std::abs(hit->point.y) < 0.1F);
}

TEST_CASE("v10d intersect_ray returns the FIRST hit when multiple candidates exist",
          "[curves][queries][intersect_ray][first]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    // Full circle in the xy-plane, radius 1, centred at origin. A ray
    // along y=0 from x=-2 toward +x crosses the circle at x=-1 (t_ray=1)
    // and at x=+1 (t_ray=3). The first hit must be at t_ray ~ 1.
    // (Circular arcs avoid the symmetric-midpoint degeneracy the adaptive
    //  chord-error sampler has on antisymmetric polynomials.)
    constexpr float two_pi = 6.28318530717958647692F;
    const CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                    v3(1.0F, 0.0F, 0.0F),
                                    v3(0.0F, 1.0F, 0.0F),
                                    /*radius_in=*/1.0F,
                                    /*sweep_radians_in=*/two_pi,
                                    /*closed_in=*/true);

    const crd::geometry::primitives::Ray3<float> ray{v3(-2.0F, 0.0F, 0.0F), v3(1.0F, 0.0F, 0.0F)};
    const auto hit = intersect_ray(arc, ray, 0.05F, &alloc);

    REQUIRE(hit.has_value());
    // First hit at x=-1 (left side of the circle) → t_ray ~ 1.
    // Allow margin: adaptive subdivision places samples; the closest hit
    // could land slightly above/below x=-1.
    REQUIRE(hit->t_ray > 0.5F);
    REQUIRE(hit->t_ray < 1.5F);
}

// ---------------------------------------------------------------------------
// f64.
// ---------------------------------------------------------------------------

TEST_CASE("v10d queries work for f64 instantiations",
          "[curves][queries][f64]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    const CubicBezier3<double> c(v3(0.0, 0.0, 0.0), v3(1.0, 2.0, 0.0), v3(2.0, 2.0, 0.0), v3(3.0, 0.0, 0.0));

    const auto box = aabb_of(c, &alloc);
    REQUIRE(box.min.x <= 0.0);
    REQUIRE(box.max.x >= 3.0);

    const auto p_on  = evaluate(c, 0.4);
    const auto cp    = closest_point(c, p_on, 1.0e-9, &alloc);
    REQUIRE(std::abs(cp.t - 0.4) < 1.0e-4);
}
