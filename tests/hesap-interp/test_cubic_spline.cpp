// crd-hesap-interp v13-b — cubic spline (natural/clamped/not-a-knot/periodic) gated ≤1e-12 vs scipy.CubicSpline
// (slopes + eval, all BCs + the n==2/n==3 special cases), plus C² continuity, the Hall-Meyer (5/384)h⁴ Tier-2 bound,
// and run-twice bit-identity (determinism moat).

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/interp/interp.hpp>

#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::interp;
using crd::containers::ConstSpan;
using crd::usize;

namespace
{
[[nodiscard]] bool close(double a, double b, double tol = 1e-12)
{
    const double d = a < b ? b - a : a - b;
    return d <= tol + tol * (b < 0 ? -b : b);
}
constexpr double kX[] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr double kY[] = {0, 1, 4, 9, 8, 5, 4, 10};
constexpr double kXq[] = {0.3, 1.7, 2.4, 3.9, 4.4, 5.6, 6.2, 6.8};
constexpr usize kN = 8;
constexpr usize kNq = 8;

void check_bc(crd::memory::IAllocator* alloc, SplineBC bc, const double (&slope_ref)[8], const double (&eval_ref)[8],
              double cl = 0.0, double cr = 0.0)
{
    CubicSplineInterpolant<double> sp(alloc);
    REQUIRE(sp.build(ConstSpan<double>{kX, kN}, ConstSpan<double>{kY, kN}, bc, cl, cr) == InterpStatus::Ok);
    const auto sl = sp.slopes();
    for (usize i = 0; i < kN; ++i)
    {
        CHECK(close(sl[i], slope_ref[i]));
    }
    for (usize i = 0; i < kNq; ++i)
    {
        CHECK(close(sp.eval(kXq[i]), eval_ref[i]));
    }
}
} // namespace

TEST_CASE("v13-b: cubic spline natural / not-a-knot / clamped vs scipy", "[v13-b][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    constexpr double nat_sl[] = {0.71246994160082444, 1.5750601167983511,  4.9872895912057711, 2.4757815183785641,
                                 -2.8904156647200274, -2.9141188594984544, 2.5468911027138446, 7.7265544486430766};
    constexpr double nat_ev[] = {0.22150429405702507, 2.7180972174510476, 6.2404946753692885, 8.2844057025077298,
                                 6.8075355547921665,  3.7054922706973548, 4.7027523187907949, 8.4685015458605282};
    check_bc(&alloc, SplineBC::Natural, nat_sl, nat_ev);

    constexpr double nak_sl[] = {1.003189792663477,   1.4984051036682615,  5.003189792663477,  2.4888357256778311,
                                 -2.9585326953748008, -2.6547049441786283, 1.5773524720893142, 11.345295055821373};
    constexpr double nak_ev[] = {0.26906937799043062, 2.710930622009569,  6.2415311004784684, 8.2900406698564595,
                                 6.7728229665071753,  3.870009569377991,  4.4628516746411497, 7.97427751196172};
    check_bc(&alloc, SplineBC::NotAKnot, nak_sl, nak_ev);

    constexpr double cl_sl[] = {1,                   1.4950188938509104,  5.0199244245963586, 2.4252834077636551,
                                -2.7210580556509787, -3.541051185159739,  4.8852627962899344, -1};
    constexpr double cl_ev[] = {0.2688138096873926, 2.7082572998969425, 6.2500419099965638, 8.2702332531776008,
                                6.8921085537615934, 3.308581243558915,  5.2813136379251127, 9.6603284094812771};
    check_bc(&alloc, SplineBC::Clamped, cl_sl, cl_ev, 1.0, -1.0);
}

TEST_CASE("v13-b: cubic spline periodic + n==3 not-a-knot + n==2 vs scipy", "[v13-b][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    constexpr double xp[] = {0, 1, 2, 3, 4};
    constexpr double yp[] = {1, 3, 2, 4, 1}; // y[0]==y[4]
    CubicSplineInterpolant<double> sp(&alloc);
    REQUIRE(sp.build(ConstSpan<double>{xp, 5}, ConstSpan<double>{yp, 5}, SplineBC::Periodic) == InterpStatus::Ok);
    constexpr double per_sl[] = {-0.75, 0.75000000000000011, 0.75, -0.74999999999999989, -0.75};
    const auto sl = sp.slopes();
    for (usize i = 0; i < 5; ++i)
    {
        CHECK(close(sl[i], per_sl[i]));
    }
    constexpr double xqp[] = {0.5, 1.5, 2.5, 3.5};
    constexpr double per_ev[] = {1.8125, 2.5, 3.1875, 2.5};
    for (usize i = 0; i < 4; ++i)
    {
        CHECK(close(sp.eval(xqp[i]), per_ev[i]));
    }

    // n==3 not-a-knot (scipy parabola)
    constexpr double x3[] = {0, 1, 3};
    constexpr double y3[] = {0, 2, 1};
    CubicSplineInterpolant<double> s3(&alloc);
    REQUIRE(s3.build(ConstSpan<double>{x3, 3}, ConstSpan<double>{y3, 3}, SplineBC::NotAKnot) == InterpStatus::Ok);
    constexpr double nak3_sl[] = {2.8333333333333335, 1.1666666666666665, -2.1666666666666665};
    const auto s3sl = s3.slopes();
    for (usize i = 0; i < 3; ++i)
    {
        CHECK(close(s3sl[i], nak3_sl[i]));
    }
    CHECK(close(s3.eval(0.5), 1.2083333333333335));
    CHECK(close(s3.eval(2.0), 2.333333333333333));

    // n==2 ⇒ both slopes = the secant
    constexpr double x2[] = {0, 2};
    constexpr double y2[] = {1, 5};
    CubicSplineInterpolant<double> s2(&alloc);
    REQUIRE(s2.build(ConstSpan<double>{x2, 2}, ConstSpan<double>{y2, 2}, SplineBC::NotAKnot) == InterpStatus::Ok);
    CHECK(close(s2.slopes()[0], 2.0));
    CHECK(close(s2.slopes()[1], 2.0));
}

TEST_CASE("v13-b: C² continuity + Tier-2 Hall-Meyer bound + determinism", "[v13-b][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    // C² continuity (analytic, exact): the cubic-Hermite second derivative from the left segment equals the right one
    // at every interior knot. S″(x_i⁺)=(6δ_i−4m_i−2m_{i+1})/h_i, S″(x_{i+1}⁻)=(−6δ_i+2m_i+4m_{i+1})/h_i.
    CubicSplineInterpolant<double> sp(&alloc);
    REQUIRE(sp.build(ConstSpan<double>{kX, kN}, ConstSpan<double>{kY, kN}, SplineBC::NotAKnot) == InterpStatus::Ok);
    const auto m = sp.slopes();
    const auto hh = [&](usize k) { return kX[k + 1] - kX[k]; };
    const auto sec = [&](usize k) { return (kY[k + 1] - kY[k]) / (kX[k + 1] - kX[k]); };
    bool c2 = true;
    for (usize k = 1; k + 1 < kN; ++k) // interior knots
    {
        const double s2l = (-6 * sec(k - 1) + 2 * m[k - 1] + 4 * m[k]) / hh(k - 1); // S″ from the left segment
        const double s2r = (6 * sec(k) - 4 * m[k] - 2 * m[k + 1]) / hh(k);          // S″ from the right segment
        if (!close(s2l, s2r, 1e-10))
        {
            c2 = false;
        }
    }
    CHECK(c2);

    // Tier-2: complete cubic spline of f=x⁴ (f⁗=24), clamped with exact end slopes ⇒ ‖f−S‖∞ ≤ (5/384)h⁴·24.
    double xs[9];
    double ys[9];
    for (usize i = 0; i < 9; ++i)
    {
        xs[i] = 0.25 * static_cast<double>(i);
        ys[i] = xs[i] * xs[i] * xs[i] * xs[i];
    }
    CubicSplineInterpolant<double> q(&alloc);
    REQUIRE(q.build(ConstSpan<double>{xs, 9}, ConstSpan<double>{ys, 9}, SplineBC::Clamped, 0.0, 32.0) ==
            InterpStatus::Ok);
    const double bound = cubic_spline_worst_case_error(0.25, 24.0);
    CHECK(close(bound, 5.0 / 384.0 * 0.25 * 0.25 * 0.25 * 0.25 * 24.0));
    bool within = true;
    for (int k = 0; k <= 400; ++k)
    {
        const double xx = 2.0 * static_cast<double>(k) / 400.0;
        const double v = q.eval(xx);
        const double f = xx * xx * xx * xx;
        const double err = (v - f) < 0 ? f - v : v - f;
        if (err > bound + 1e-12)
        {
            within = false;
        }
    }
    CHECK(within);

    // determinism: same data ⇒ bit-identical slopes
    CubicSplineInterpolant<double> a(&alloc);
    CubicSplineInterpolant<double> b(&alloc);
    REQUIRE(a.build(ConstSpan<double>{kX, kN}, ConstSpan<double>{kY, kN}, SplineBC::NotAKnot) == InterpStatus::Ok);
    REQUIRE(b.build(ConstSpan<double>{kX, kN}, ConstSpan<double>{kY, kN}, SplineBC::NotAKnot) == InterpStatus::Ok);
    bool same = true;
    for (usize i = 0; i < kN; ++i)
    {
        if (a.slopes()[i] != b.slopes()[i])
        {
            same = false;
        }
    }
    CHECK(same);
}
