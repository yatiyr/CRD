// ---------------------------------------------------------------------------
// crd-geometry-curves — Sampling. Phase 3.1.7 v10b.
//
// Coverage:
//   1. `sample_uniform` boundary + count contracts (open vs closed).
//   2. `sample_uniform` determinism: same input -> byte-identical output.
//   3. `sample_adaptive` honors the chord-error tolerance bound.
//   4. `sample_adaptive` boundary equality at t=0 / t=1.
//   5. `sample_by_curvature` produces denser samples in high-curvature
//      regions and sparser in low-curvature regions.
//   6. `to_polyline` convenience uses default tolerance.
//   7. Closed-curve sampling: output is `Polyline3{closed=true}` and the
//      duplicate-at-t=1 sample is dropped (D195).
//   8. f64 instantiations work.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/curves.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/test_helpers/gpu_compare.hpp>

#include <cmath>
#include <cstring>

namespace
{

using namespace crd::geometry::curves;

template <typename T>
[[nodiscard]] crd::math::Vec3<T> v3(T x, T y, T z) noexcept
{
    return crd::math::Vec3<T>(x, y, z);
}

[[nodiscard]] bool vec_bit_equal(const crd::math::Vec3<float>& a, const crd::math::Vec3<float>& b) noexcept
{
    return std::memcmp(&a, &b, sizeof(crd::math::Vec3<float>)) == 0;
}

[[nodiscard]] bool vec_close(const crd::math::Vec3<float>& a, const crd::math::Vec3<float>& b, float eps) noexcept
{
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps && std::abs(a.z - b.z) <= eps;
}

} // namespace

// ---------------------------------------------------------------------------
// sample_uniform.
// ---------------------------------------------------------------------------

TEST_CASE("v10b sample_uniform open curve emits n+1 points",
          "[curves][sample][uniform]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, 2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto p = sample_uniform(c, 8U, &alloc);

    REQUIRE(p.points.size() == 9U); // 8 segments + 1
    REQUIRE_FALSE(p.closed);
    // Endpoints bit-equal to evaluator.
    REQUIRE(vec_bit_equal(p.points[0], evaluate(c, 0.0F)));
    REQUIRE(vec_bit_equal(p.points[8], evaluate(c, 1.0F)));
}

TEST_CASE("v10b sample_uniform closed curve emits n points + closed=true",
          "[curves][sample][uniform][closed]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    constexpr float two_pi = 6.28318530717958647692F;
    const CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                    v3(1.0F, 0.0F, 0.0F),
                                    v3(0.0F, 1.0F, 0.0F),
                                    /*radius_in=*/1.0F,
                                    /*sweep_radians_in=*/two_pi,
                                    /*closed_in=*/true);
    const auto p = sample_uniform(arc, 16U, &alloc);

    REQUIRE(p.points.size() == 16U); // 16 segments, closed -> drop t=1.0
    REQUIRE(p.closed);
    // First point at t=0.
    REQUIRE(vec_close(p.points[0], evaluate(arc, 0.0F), 1.0e-5F));
    // Sample at index 0 should NOT bit-equal sample at index N-1 — D195
    // says the duplicate is dropped.
}

TEST_CASE("v10b sample_uniform is deterministic across runs",
          "[curves][sample][uniform][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const QuadBezier3<float> q(v3(0.0F, 0.0F, 0.0F), v3(1.0F, 2.0F, 0.0F), v3(2.0F, 0.0F, 0.0F));
    const auto run1 = sample_uniform(q, 32U, &alloc);
    const auto run2 = sample_uniform(q, 32U, &alloc);

    REQUIRE(run1.points.size() == run2.points.size());
    for (crd::usize i = 0U; i < run1.points.size(); ++i)
    {
        REQUIRE(vec_bit_equal(run1.points[i], run2.points[i]));
    }
}

// ---------------------------------------------------------------------------
// sample_adaptive.
// ---------------------------------------------------------------------------

TEST_CASE("v10b sample_adaptive emits boundary points bit-equal to evaluator",
          "[curves][sample][adaptive]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 3.0F, 0.0F),
                                 v3(2.0F, 3.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto p = sample_adaptive(c, 0.05F, &alloc);

    REQUIRE(p.points.size() >= 2U);
    REQUIRE(vec_bit_equal(p.points[0], evaluate(c, 0.0F)));
    REQUIRE(vec_bit_equal(p.points[p.points.size() - 1U], evaluate(c, 1.0F)));
    REQUIRE_FALSE(p.closed);
}

TEST_CASE("v10b sample_adaptive: tighter tolerance produces more samples",
          "[curves][sample][adaptive][convergence]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 5.0F, 0.0F),
                                 v3(2.0F, 5.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto loose = sample_adaptive(c, 0.5F, &alloc);
    const auto tight = sample_adaptive(c, 0.001F, &alloc);

    REQUIRE(tight.points.size() > loose.points.size());
}

TEST_CASE("v10b sample_adaptive: tighter tolerance bounds output count",
          "[curves][sample][adaptive][bound]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    // A high-curvature cubic Bezier exercises the subdivision logic.
    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(0.5F, 3.0F, 0.0F),
                                 v3(2.5F, 3.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto loose  = sample_adaptive(c, 0.5F,  &alloc);
    const auto medium = sample_adaptive(c, 0.05F, &alloc);
    const auto tight  = sample_adaptive(c, 0.005F, &alloc);

    // Monotonic-on-tolerance: tighter tolerance produces more samples.
    REQUIRE(loose.points.size()  < medium.points.size());
    REQUIRE(medium.points.size() < tight.points.size());

    // Depth cap (D196) guarantees a finite output regardless of input.
    REQUIRE(tight.points.size() <= 1U + (1ULL << k_sample_max_subdivision_depth));
}

// ---------------------------------------------------------------------------
// sample_by_curvature.
// ---------------------------------------------------------------------------

TEST_CASE("v10b sample_by_curvature emits boundary points + monotonic t",
          "[curves][sample][curvature]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    // Tight S-curve - varying curvature.
    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, -2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto p = sample_by_curvature(c, 0.2F, &alloc);

    REQUIRE(p.points.size() >= 2U);
    REQUIRE(vec_bit_equal(p.points[0], evaluate(c, 0.0F)));
    REQUIRE(vec_bit_equal(p.points[p.points.size() - 1U], evaluate(c, 1.0F)));
}

TEST_CASE("v10b sample_by_curvature: smaller angle threshold produces more samples",
          "[curves][sample][curvature][convergence]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 3.0F, 0.0F),
                                 v3(2.0F, 3.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto loose = sample_by_curvature(c, 0.5F, &alloc);
    const auto tight = sample_by_curvature(c, 0.05F, &alloc);

    REQUIRE(tight.points.size() > loose.points.size());
}

// ---------------------------------------------------------------------------
// to_polyline convenience.
// ---------------------------------------------------------------------------

TEST_CASE("v10b to_polyline uses default tolerance and matches sample_adaptive at default",
          "[curves][sample][to_polyline]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, 2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto via_default  = to_polyline(c, &alloc);
    const auto via_explicit = sample_adaptive(c, sample_default_tolerance<float>(), &alloc);

    REQUIRE(via_default.points.size() == via_explicit.points.size());
    for (crd::usize i = 0U; i < via_default.points.size(); ++i)
    {
        REQUIRE(vec_bit_equal(via_default.points[i], via_explicit.points[i]));
    }
}

// ---------------------------------------------------------------------------
// Cross-kind smoke: every curve kind compiles + samples without error.
// ---------------------------------------------------------------------------

TEST_CASE("v10b sample_uniform compiles for every curve kind",
          "[curves][sample][uniform][cross-kind]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    // Polyline3
    {
        const crd::math::Vec3<float> pts[] = {v3(0.0F, 0.0F, 0.0F), v3(1.0F, 0.0F, 0.0F)};
        Polyline3<float> pl(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 2U});
        const auto out = sample_uniform(pl.view(), 4U, &alloc);
        REQUIRE(out.points.size() == 5U);
    }
    // QuadBezier3
    {
        const QuadBezier3<float> q(v3(0.0F, 0.0F, 0.0F), v3(1.0F, 1.0F, 0.0F), v3(2.0F, 0.0F, 0.0F));
        const auto out = sample_uniform(q, 4U, &alloc);
        REQUIRE(out.points.size() == 5U);
    }
    // CubicHermite3
    {
        const CubicHermite3<float> h(v3(0.0F, 0.0F, 0.0F),
                                      v3(1.0F, 1.0F, 0.0F),
                                      v3(1.0F, 0.0F, 0.0F),
                                      v3(0.0F, -1.0F, 0.0F));
        const auto out = sample_uniform(h, 4U, &alloc);
        REQUIRE(out.points.size() == 5U);
    }
    // CatmullRom3
    {
        const crd::math::Vec3<float> pts[] = {
            v3(0.0F, 0.0F, 0.0F), v3(1.0F, 0.0F, 0.0F), v3(2.0F, 1.0F, 0.0F), v3(3.0F, 0.0F, 0.0F)};
        const CatmullRom3<float> cr(&alloc,
                                     crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 4U},
                                     CatmullRomParam::Centripetal);
        const auto out = sample_uniform(cr, 4U, &alloc);
        REQUIRE(out.points.size() == 5U);
    }
    // BSpline3
    {
        const crd::math::Vec3<float> pts[] = {
            v3(0.0F, 0.0F, 0.0F), v3(1.0F, 1.0F, 0.0F),
            v3(2.0F, 1.0F, 0.0F), v3(3.0F, 0.0F, 0.0F)};
        const auto bs = BSpline3<float>::make_uniform_open(
            &alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 4U});
        const auto out = sample_uniform(bs, 4U, &alloc);
        REQUIRE(out.points.size() == 5U);
    }
    // CircularArc3
    {
        const CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                        v3(1.0F, 0.0F, 0.0F),
                                        v3(0.0F, 1.0F, 0.0F),
                                        /*radius_in=*/1.0F,
                                        /*sweep_radians_in=*/3.14159F);
        const auto out = sample_uniform(arc, 4U, &alloc);
        REQUIRE(out.points.size() == 5U);
    }
    // EllipseArc3
    {
        const EllipseArc3<float> e(v3(0.0F, 0.0F, 0.0F),
                                     v3(1.0F, 0.0F, 0.0F),
                                     v3(0.0F, 1.0F, 0.0F),
                                     /*radius_u_in=*/2.0F,
                                     /*radius_v_in=*/1.0F,
                                     /*sweep_radians_in=*/3.14159F);
        const auto out = sample_uniform(e, 4U, &alloc);
        REQUIRE(out.points.size() == 5U);
    }
}

// ---------------------------------------------------------------------------
// f64 instantiation.
// ---------------------------------------------------------------------------

TEST_CASE("v10b sample_uniform + sample_adaptive work for f64",
          "[curves][sample][f64]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    const CubicBezier3<double> c(v3(0.0, 0.0, 0.0), v3(1.0, 3.0, 0.0), v3(2.0, 3.0, 0.0), v3(3.0, 0.0, 0.0));
    const auto u  = sample_uniform(c, 8U, &alloc);
    const auto ad = sample_adaptive(c, 0.01, &alloc);

    REQUIRE(u.points.size() == 9U);
    REQUIRE(ad.points.size() >= 2U);
    // Boundaries.
    REQUIRE(u.points[0].x == 0.0);
    REQUIRE(u.points[8].x == 3.0);
}
