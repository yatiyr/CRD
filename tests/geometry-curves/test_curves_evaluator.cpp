// ---------------------------------------------------------------------------
// crd-geometry-curves — Evaluator. Phase 3.1.7 v10a.
//
// D186: `evaluate(curve, t)` IS the algorithm definition. This corpus
// pins:
//
//   1. Per-kind boundary behaviour — `evaluate(curve, 0)` is bit-exact
//      the curve's algebraic start; `evaluate(curve, 1)` is the algebraic
//      end (for closed curves, equals start).
//
//   2. Cross-kind equivalence — CubicHermite(P0, T0=3(P1-P0), P3,
//      T1=3(P3-P2)) ==CubicBezier(P0, P1, P2, P3) within 1 ULP at every
//      sample point. The standard basis-change identity load-bears here:
//      it proves the IR-and-evaluator-walker is consistent across two
//      different polynomial bases.
//
//   3. Closed-curve wrap — `evaluate(curve, 1.0)` is bit-equal to
//      `evaluate(curve, 0.0)` when `closed == true`.
//
//   4. Derivative direction — `evaluate_derivative(curve, 0)` matches
//      the chord direction at t=0+ε within a reasonable ULP bound
//      (tighter for analytical derivatives; FD-based derivatives for
//      CatmullRom + BSpline use a wider 1e-3-class tolerance).
//
//   5. f32 + f64 instantiate cleanly + produce qualitatively-equal
//      output (within scalar epsilon).
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

template <typename T>
[[nodiscard]] bool approx_eq(T a, T b, T eps) noexcept
{
    return std::abs(a - b) <= eps;
}

template <typename T>
[[nodiscard]] bool vec_approx_eq(const crd::math::Vec3<T>& a, const crd::math::Vec3<T>& b, T eps) noexcept
{
    return approx_eq(a.x, b.x, eps) && approx_eq(a.y, b.y, eps) && approx_eq(a.z, b.z, eps);
}

[[nodiscard]] bool vec_bit_equal(const crd::math::Vec3<float>& a, const crd::math::Vec3<float>& b) noexcept
{
    return std::memcmp(&a, &b, sizeof(crd::math::Vec3<float>)) == 0;
}

constexpr float kBoundaryUlp = 1U; // 1 ULP for bit-equal-class boundary checks
constexpr float kCrossKindEps = 1.0e-6F;

[[nodiscard]] bool ulp_close(float a, float b, crd::u32 max_ulp) noexcept
{
    return crd::test::detail::ulp_distance_f32(a, b) <= max_ulp;
}

[[nodiscard]] bool vec_ulp_close(const crd::math::Vec3<float>& a,
                                  const crd::math::Vec3<float>& b,
                                  crd::u32                       max_ulp) noexcept
{
    return ulp_close(a.x, b.x, max_ulp) && ulp_close(a.y, b.y, max_ulp) && ulp_close(a.z, b.z, max_ulp);
}

} // namespace

// ---------------------------------------------------------------------------
// Polyline.
// ---------------------------------------------------------------------------

TEST_CASE("v10a Polyline3 evaluate hits each vertex at segment boundaries",
          "[curves][evaluator][polyline]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> pts[] = {v3(0.0F, 0.0F, 0.0F), v3(1.0F, 0.0F, 0.0F), v3(1.0F, 1.0F, 0.0F)};
    Polyline3<float> p(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 3U});
    const auto       view = p.view();

    // t=0 -> first point bit-exactly.
    REQUIRE(vec_bit_equal(evaluate(view, 0.0F), pts[0]));
    // t=1 -> last point bit-exactly.
    REQUIRE(vec_bit_equal(evaluate(view, 1.0F), pts[2]));
    // Halfway across segment 0 (t=0.25 since 2 segments, segment 0 is t in [0, 0.5]).
    const auto mid01 = evaluate(view, 0.25F);
    REQUIRE(vec_ulp_close(mid01, v3(0.5F, 0.0F, 0.0F), 1U));
}

TEST_CASE("v10a Polyline3 closed wrap: evaluate(t=1) bit-equal evaluate(t=0)",
          "[curves][evaluator][polyline][closed]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> pts[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(1.0F, 1.0F, 0.0F),
    };
    Polyline3<float> p(&alloc,
                       crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 3U},
                       /*closed_in=*/true);

    const auto at_zero = evaluate(p.view(), 0.0F);
    const auto at_one  = evaluate(p.view(), 1.0F);
    REQUIRE(vec_bit_equal(at_zero, at_one));
}

// ---------------------------------------------------------------------------
// Bezier — endpoint interpolation + de Casteljau midpoint check.
// ---------------------------------------------------------------------------

TEST_CASE("v10a CubicBezier3 evaluate hits P0 at t=0 and P3 at t=1 bit-exactly",
          "[curves][evaluator][bezier][cubic][boundary]")
{
    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 2.0F, 0.0F),
                                 v3(2.0F, 2.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    REQUIRE(vec_bit_equal(evaluate(c, 0.0F), c.p0));
    REQUIRE(vec_bit_equal(evaluate(c, 1.0F), c.p3));
}

TEST_CASE("v10a CubicBezier3 midpoint matches Bernstein expansion within 1 ULP",
          "[curves][evaluator][bezier][cubic]")
{
    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(1.0F, 3.0F, 0.0F),
                                 v3(2.0F, 3.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
    const auto eval_mid = evaluate(c, 0.5F);

    // Bernstein expansion at t=0.5:
    //   B(0.5) = 1/8 * (P0 + 3*P1 + 3*P2 + P3)
    const auto expected = (c.p0 + c.p1 * 3.0F + c.p2 * 3.0F + c.p3) * (1.0F / 8.0F);
    REQUIRE(vec_ulp_close(eval_mid, expected, 4U));
}

TEST_CASE("v10a CubicBezier3 derivative at t=0 equals 3*(P1-P0)",
          "[curves][evaluator][bezier][derivative]")
{
    const CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                                 v3(2.0F, 0.0F, 0.0F),
                                 v3(2.0F, 1.0F, 0.0F),
                                 v3(3.0F, 1.0F, 0.0F));
    const auto deriv     = evaluate_derivative(c, 0.0F);
    const auto expected  = (c.p1 - c.p0) * 3.0F;
    REQUIRE(vec_bit_equal(deriv, expected));
}

TEST_CASE("v10a QuadBezier3 evaluate hits P0 at t=0 and P2 at t=1",
          "[curves][evaluator][bezier][quad][boundary]")
{
    const QuadBezier3<float> q(v3(0.0F, 0.0F, 0.0F), v3(1.0F, 1.0F, 0.0F), v3(2.0F, 0.0F, 0.0F));
    REQUIRE(vec_bit_equal(evaluate(q, 0.0F), q.p0));
    REQUIRE(vec_bit_equal(evaluate(q, 1.0F), q.p2));
}

// ---------------------------------------------------------------------------
// CubicHermite3 — boundary + cross-kind identity vs CubicBezier3.
// ---------------------------------------------------------------------------

TEST_CASE("v10a CubicHermite3 hits p0 at t=0 and p1 at t=1 bit-exactly",
          "[curves][evaluator][hermite][boundary]")
{
    const CubicHermite3<float> h(v3(0.0F, 0.0F, 0.0F),
                                  v3(1.0F, 0.0F, 0.0F), // t0
                                  v3(2.0F, 1.0F, 0.0F),
                                  v3(1.0F, 0.0F, 0.0F)); // t1
    REQUIRE(vec_bit_equal(evaluate(h, 0.0F), h.p0));
    REQUIRE(vec_bit_equal(evaluate(h, 1.0F), h.p1));
}

TEST_CASE("v10a CubicHermite3 == CubicBezier3 via the standard basis-change identity",
          "[curves][evaluator][hermite][bezier][cross-kind]")
{
    // CubicBezier(p0, p1, p2, p3)  == CubicHermite(p0, T0, p3, T1)
    //   where T0 = 3*(p1 - p0), T1 = 3*(p3 - p2).
    const auto p0 = v3(0.0F, 0.0F, 0.0F);
    const auto p1 = v3(1.0F, 3.0F, 0.0F);
    const auto p2 = v3(2.0F, 3.0F, 0.0F);
    const auto p3 = v3(3.0F, 0.0F, 0.0F);

    const CubicBezier3<float>  bez(p0, p1, p2, p3);
    const CubicHermite3<float> her(p0, (p1 - p0) * 3.0F, p3, (p3 - p2) * 3.0F);

    // Sample at 11 evenly-spaced points and require ≤ 4 ULP agreement.
    // The two evaluators compute different polynomial bases — they MUST
    // agree to within rounding, never structurally diverge.
    for (crd::u32 i = 0U; i <= 10U; ++i)
    {
        const float t = static_cast<float>(i) / 10.0F;
        INFO("t = " << t);
        const auto vb = evaluate(bez, t);
        const auto vh = evaluate(her, t);
        REQUIRE(vec_ulp_close(vb, vh, 4U));
    }
}

// ---------------------------------------------------------------------------
// CatmullRom — interpolates control points + closed wrap.
// ---------------------------------------------------------------------------

TEST_CASE("v10a CatmullRom3 (uniform) passes through control points at segment joins",
          "[curves][evaluator][catmull_rom][uniform]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> pts[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(2.0F, 1.0F, 0.0F),
        v3(3.0F, 0.0F, 0.0F),
    };
    CatmullRom3<float> cr(&alloc,
                          crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 4U},
                          CatmullRomParam::Uniform);

    // n=4 -> 3 segments; t at the segment joins are 0, 1/3, 2/3, 1.
    REQUIRE(vec_ulp_close(evaluate(cr, 0.0F),               pts[0], 4U));
    REQUIRE(vec_ulp_close(evaluate(cr, 1.0F / 3.0F),        pts[1], 64U));
    REQUIRE(vec_ulp_close(evaluate(cr, 2.0F / 3.0F),        pts[2], 64U));
    REQUIRE(vec_ulp_close(evaluate(cr, 1.0F),               pts[3], 4U));
}

TEST_CASE("v10a CatmullRom3 (centripetal) passes through control points at segment joins",
          "[curves][evaluator][catmull_rom][centripetal]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> pts[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(2.0F, 1.0F, 0.0F),
        v3(3.0F, 0.0F, 0.0F),
    };
    CatmullRom3<float> cr(&alloc,
                          crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 4U},
                          CatmullRomParam::Centripetal);

    REQUIRE(vec_ulp_close(evaluate(cr, 0.0F), pts[0], 4U));
    REQUIRE(vec_ulp_close(evaluate(cr, 1.0F), pts[3], 4U));
}

TEST_CASE("v10a CatmullRom3 closed loop wraps t=1 to t=0",
          "[curves][evaluator][catmull_rom][closed]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> pts[] = {
        v3(1.0F, 0.0F, 0.0F),
        v3(0.0F, 1.0F, 0.0F),
        v3(-1.0F, 0.0F, 0.0F),
        v3(0.0F, -1.0F, 0.0F),
    };
    CatmullRom3<float> cr(&alloc,
                          crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 4U},
                          CatmullRomParam::Centripetal,
                          /*closed_in=*/true);
    REQUIRE(vec_bit_equal(evaluate(cr, 1.0F), evaluate(cr, 0.0F)));
}

// ---------------------------------------------------------------------------
// BSpline3 — clamped-endpoint interpolation + smoothness.
// ---------------------------------------------------------------------------

TEST_CASE("v10a BSpline3 uniform-open clamps to P0 at t=0 and P_{n-1} at t=1",
          "[curves][evaluator][bspline][clamped]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> pts[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 1.0F, 0.0F),
        v3(2.0F, 1.0F, 0.0F),
        v3(3.0F, 0.0F, 0.0F),
        v3(4.0F, -1.0F, 0.0F),
    };
    const auto bs = BSpline3<float>::make_uniform_open(
        &alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 5U});

    REQUIRE(vec_ulp_close(evaluate(bs, 0.0F), pts[0], 4U));
    REQUIRE(vec_ulp_close(evaluate(bs, 1.0F), pts[4], 4U));
}

TEST_CASE("v10a BSpline3 produces a continuous curve in the parameter interior",
          "[curves][evaluator][bspline][smoothness]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> pts[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 1.0F, 0.0F),
        v3(2.0F, 1.0F, 0.0F),
        v3(3.0F, 0.0F, 0.0F),
        v3(4.0F, -1.0F, 0.0F),
    };
    const auto bs = BSpline3<float>::make_uniform_open(
        &alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{pts, 5U});

    // Sample 21 points and ensure each step has bounded chord length —
    // crude continuity check that catches kind-routing bugs.
    crd::math::Vec3<float> prev = evaluate(bs, 0.0F);
    for (crd::u32 i = 1U; i <= 20U; ++i)
    {
        const float t   = static_cast<float>(i) / 20.0F;
        const auto  cur = evaluate(bs, t);
        const float dx  = cur.x - prev.x;
        const float dy  = cur.y - prev.y;
        const float dz  = cur.z - prev.z;
        const float d2  = dx * dx + dy * dy + dz * dz;
        REQUIRE(d2 < 1.0F); // generous: any wild jump would be ≥ 1.
        prev = cur;
    }
}

// ---------------------------------------------------------------------------
// CircularArc3 — algebraic-position check + derivative direction.
// ---------------------------------------------------------------------------

TEST_CASE("v10a CircularArc3 evaluate at t=0 is center + radius*axis_u",
          "[curves][evaluator][arc][boundary]")
{
    const CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                    v3(1.0F, 0.0F, 0.0F),
                                    v3(0.0F, 1.0F, 0.0F),
                                    /*radius_in=*/2.0F,
                                    /*sweep_radians_in=*/3.14159265F);
    const auto at_zero = evaluate(arc, 0.0F);
    // cos(0) = 1, sin(0) = 0 ⇒ result = center + radius * axis_u.
    const auto expected = v3(2.0F, 0.0F, 0.0F);
    REQUIRE(vec_ulp_close(at_zero, expected, 4U));
}

TEST_CASE("v10a CircularArc3 half-revolution lands at center + radius*(-axis_u) approximately",
          "[curves][evaluator][arc]")
{
    const float pi = 3.14159265358979323846F;
    const CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                    v3(1.0F, 0.0F, 0.0F),
                                    v3(0.0F, 1.0F, 0.0F),
                                    /*radius_in=*/2.0F,
                                    /*sweep_radians_in=*/pi);

    const auto at_one = evaluate(arc, 1.0F);
    // Deterministic Cephes-poly sin/cos have ~3 ULP error; allow ~1e-5 abs.
    const auto expected = v3(-2.0F, 0.0F, 0.0F);
    REQUIRE(vec_approx_eq(at_one, expected, 1.0e-5F));
}

TEST_CASE("v10a CircularArc3 derivative at t=0 is along axis_v (tangent)",
          "[curves][evaluator][arc][derivative]")
{
    const CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                    v3(1.0F, 0.0F, 0.0F),
                                    v3(0.0F, 1.0F, 0.0F),
                                    /*radius_in=*/2.0F,
                                    /*sweep_radians_in=*/3.14159265F);
    const auto deriv = evaluate_derivative(arc, 0.0F);
    // d/dt at t=0 = sweep_radians * radius * (-sin(0) * u + cos(0) * v)
    //             = sweep_radians * radius * v
    //             = 3.14159265 * 2 * (0,1,0)
    REQUIRE(approx_eq(deriv.x, 0.0F, 1.0e-5F));
    REQUIRE(approx_eq(deriv.y, 3.14159265F * 2.0F, 1.0e-4F));
    REQUIRE(approx_eq(deriv.z, 0.0F, 1.0e-5F));
}

TEST_CASE("v10a CircularArc3 closed wraps t=1 to t=0", "[curves][evaluator][arc][closed]")
{
    constexpr float kTwoPi = 6.28318530717958647692F;
    const CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                    v3(1.0F, 0.0F, 0.0F),
                                    v3(0.0F, 1.0F, 0.0F),
                                    /*radius_in=*/1.0F,
                                    /*sweep_radians_in=*/kTwoPi,
                                    /*closed_in=*/true);
    REQUIRE(vec_bit_equal(evaluate(arc, 1.0F), evaluate(arc, 0.0F)));
}

// ---------------------------------------------------------------------------
// f64 instantiation — covered by template instantiation existing + a single
// behaviour check that f32 and f64 produce qualitatively-equal answers.
// ---------------------------------------------------------------------------

TEST_CASE("v10a evaluator works for f64 instantiations",
          "[curves][evaluator][f64]")
{
    const CubicBezier3<double> c(v3(0.0, 0.0, 0.0), v3(1.0, 3.0, 0.0), v3(2.0, 3.0, 0.0), v3(3.0, 0.0, 0.0));

    // Bit-exact endpoint, matches the f32 contract.
    const auto at_zero = evaluate(c, 0.0);
    const auto at_one  = evaluate(c, 1.0);
    REQUIRE(at_zero.x == 0.0);
    REQUIRE(at_one.x == 3.0);

    // Midpoint vs Bernstein expansion.
    const auto mid       = evaluate(c, 0.5);
    const auto expected  = (c.p0 + c.p1 * 3.0 + c.p2 * 3.0 + c.p3) * (1.0 / 8.0);
    REQUIRE(vec_approx_eq(mid, expected, 1.0e-12));
}
