// crd-hesap-interp v13-a — 1-D piecewise interpolation gated ≤1e-12 vs scipy (interp1d/CubicHermiteSpline/
// PchipInterpolator), plus the PCHIP no-overshoot invariant on monotone data, the Tier-2 linear error bound, and the
// run-twice bit-identity (determinism moat).

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/interp/interp.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <limits>

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
constexpr double kD[] = {0.5, 2.0, 3.0, -1.0, -2.0, -1.0, 2.0, 5.0};
constexpr double kXq[] = {0.3, 1.7, 2.4, 3.9, 4.4, 5.6, 6.2, 6.8};
constexpr usize kN = 8;
constexpr usize kNq = 8;
} // namespace

TEST_CASE("v13-a: linear / nearest / Hermite vs scipy", "[v13-a][interp]")
{
    const auto x = ConstSpan<double>{kX, kN};
    const auto y = ConstSpan<double>{kY, kN};
    const auto d = ConstSpan<double>{kD, kN};
    constexpr double lin[] = {0.29999999999999999, 3.0999999999999996, 6.0,
                              8.0999999999999996, 6.7999999999999989, 4.4000000000000004,
                              5.2000000000000011, 8.7999999999999989};
    constexpr double nea[] = {0, 4, 4, 8, 8, 4, 4, 10};
    constexpr double her[] = {0.16350000000000001, 3.0369999999999999, 6.2879999999999994,
                              8.1810000000000009, 6.751999999999998,  3.9680000000000004,
                              4.7200000000000006, 8.7999999999999972};
    usize c = 0;
    for (usize i = 0; i < kNq; ++i)
    {
        CHECK(close(interp_linear(x, y, kXq[i], c), lin[i]));
        CHECK(close(interp_nearest(x, y, kXq[i], c), nea[i]));
        CHECK(close(interp_hermite(x, y, d, kXq[i], c), her[i]));
    }
}

TEST_CASE("v13-a: PCHIP slopes + eval + extrapolation vs scipy", "[v13-a][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    const auto x = ConstSpan<double>{kX, kN};
    const auto y = ConstSpan<double>{kY, kN};
    PchipInterpolant<double> p(&alloc);
    REQUIRE(p.build(x, y) == InterpStatus::Ok);

    constexpr double slope_ref[] = {0, 1.5, 3.75, 0, -1.5, -1.5, 0, 9.5};
    const auto sl = p.slopes();
    for (usize i = 0; i < kN; ++i)
    {
        CHECK(close(sl[i], slope_ref[i]));
    }
    constexpr double pchip_ref[] = {0.12150000000000001, 2.8952499999999999, 6.3,
                                    8.1494999999999997, 6.8719999999999981, 4.2080000000000002,
                                    4.3200000000000003, 8.1599999999999984};
    for (usize i = 0; i < kNq; ++i)
    {
        CHECK(close(p.eval(kXq[i]), pchip_ref[i]));
    }
    // scipy PCHIP extrapolates on the boundary cubic — match it
    CHECK(close(p.eval(-0.5), 0.4375));
    CHECK(close(p.eval(7.5), 14.6875));
}

TEST_CASE("v13-a: PCHIP no-overshoot invariant on monotone data", "[v13-a][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    constexpr double xm[] = {0, 1, 2, 3, 4};
    constexpr double ym[] = {0.0, 0.1, 0.5, 5.0, 5.1}; // monotone; a naive cubic spline would overshoot the steep step
    PchipInterpolant<double> p(&alloc);
    REQUIRE(p.build(ConstSpan<double>{xm, 5}, ConstSpan<double>{ym, 5}) == InterpStatus::Ok);
    bool monotone = true;
    bool bounded = true;
    double prev = p.eval(0.0);
    for (int k = 0; k <= 200; ++k)
    {
        const double xx = 4.0 * static_cast<double>(k) / 200.0;
        const double v = p.eval(xx);
        if (v < prev - 1e-12) // non-decreasing (no overshoot)
        {
            monotone = false;
        }
        if (v < -1e-12 || v > 5.1 + 1e-12) // stays within the data range
        {
            bounded = false;
        }
        prev = v;
    }
    CHECK(monotone);
    CHECK(bounded);
}

TEST_CASE("v13-a: Tier-2 certified linear error bound", "[v13-a][interp]")
{
    // f(x)=x², f″=2 ⇒ the bound h²/8·max|f″| = h²/4 is TIGHT (equals the true max error).
    constexpr double h = 0.5;
    const double bound = linear_worst_case_error(0.5, 2.0);
    CHECK(close(bound, h * h / 4.0));
    constexpr double xs[] = {0, 0.5, 1.0, 1.5, 2.0};
    constexpr double ys[] = {0, 0.25, 1.0, 2.25, 4.0};
    usize c = 0;
    bool within = true;
    for (int k = 0; k <= 400; ++k)
    {
        const double xx = 2.0 * static_cast<double>(k) / 400.0;
        const double v = interp_linear(ConstSpan<double>{xs, 5}, ConstSpan<double>{ys, 5}, xx, c);
        const double f = xx * xx;
        const double e = (v - f) < 0 ? f - v : v - f;
        if (e > bound + 1e-12) // the certified bound holds
        {
            within = false;
        }
    }
    CHECK(within);
}

TEST_CASE("v13-a: determinism (run-twice bit-identity) + status validation", "[v13-a][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    const auto x = ConstSpan<double>{kX, kN};
    const auto y = ConstSpan<double>{kY, kN};
    PchipInterpolant<double> a(&alloc);
    PchipInterpolant<double> b(&alloc);
    REQUIRE(a.build(x, y) == InterpStatus::Ok);
    REQUIRE(b.build(x, y) == InterpStatus::Ok);
    bool same = true;
    for (usize i = 0; i < kNq; ++i)
    {
        if (a.eval(kXq[i]) != b.eval(kXq[i])) // exact bit-identity (not approximate)
        {
            same = false;
        }
    }
    CHECK(same);

    // defensive validation: status, not exception/garbage
    crd::containers::Array<double> d(&alloc);
    d.resize(kN);
    double bad_x[kN];
    for (usize i = 0; i < kN; ++i)
    {
        bad_x[i] = kX[i];
    }
    bad_x[2] = bad_x[1]; // duplicate ⇒ not strictly increasing
    CHECK(pchip_slopes(ConstSpan<double>{bad_x, kN}, y, crd::containers::Span<double>{d.data(), kN}) ==
          InterpStatus::NotIncreasing);
    double nan_y[kN];
    for (usize i = 0; i < kN; ++i)
    {
        nan_y[i] = kY[i];
    }
    nan_y[7] = std::numeric_limits<double>::quiet_NaN();
    CHECK(pchip_slopes(x, ConstSpan<double>{nan_y, kN}, crd::containers::Span<double>{d.data(), kN}) ==
          InterpStatus::BadInput);
}
