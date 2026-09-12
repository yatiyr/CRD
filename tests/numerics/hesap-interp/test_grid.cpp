// crd-hesap-interp v13-f — gridded N-D N-linear. Gated ≤1e-12 vs scipy.RegularGridInterpolator('linear') (2-D + 3-D),
// exact bilinear reproduction, edge-clamp returns a finite value (no NaN), + determinism.

#include <crd/containers/span.hpp>
#include <crd/hesap/interp/interp.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::hesap::interp;
using crd::usize;
using crd::containers::ConstSpan;

namespace
{
[[nodiscard]] bool close(double a, double b, double tol = 1e-12)
{
    const double d = a < b ? b - a : a - b;
    return d <= tol + tol * (b < 0 ? -b : b);
}
[[nodiscard]] bool is_fin(double v)
{
    return detail::is_finite(v);
}
} // namespace

TEST_CASE("v13-f: N-linear 2-D vs scipy + bilinear reproduction", "[v13-f][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    constexpr double origin[2] = {0.0, 0.0};
    constexpr double spacing[2] = {1.0, 1.0};
    constexpr usize count[2] = {5, 4};
    constexpr double v2[20] = {1, 2.5, 6, 11.5, 0, 2.5, 7, 13.5, 1, 4.5, 10, 17.5, 4, 8.5, 15, 23.5, 9, 14.5, 22, 31.5};
    RegularGridInterpolant<double> g(&alloc);
    REQUIRE(g.build(ConstSpan<double>{origin, 2}, ConstSpan<double>{spacing, 2}, ConstSpan<usize>{count, 2}, 2,
                    ConstSpan<double>{v2, 20}) == InterpStatus::Ok);
    constexpr double q2[4][2] = {{1.3, 0.7}, {2.6, 2.1}, {3.4, 1.5}, {0.5, 2.8}};
    constexpr double ref2[4] = {2.2599999999999998, 13.810000000000002, 14.349999999999998, 11.299999999999999};
    for (int i = 0; i < 4; ++i)
    {
        CHECK(close(g.eval_linear(ConstSpan<double>{q2[i], 2}), ref2[i]));
    }

    // bilinear reproduction: f = 1 + 2x − 1.5y + 0.7xy is reproduced EXACTLY by N-linear
    double vb[20];
    for (usize xi = 0; xi < 5; ++xi)
    {
        for (usize yj = 0; yj < 4; ++yj)
        {
            vb[xi * 4 + yj] = 1.0 + 2.0 * static_cast<double>(xi) - 1.5 * static_cast<double>(yj) +
                              0.7 * static_cast<double>(xi) * static_cast<double>(yj);
        }
    }
    RegularGridInterpolant<double> gb(&alloc);
    REQUIRE(gb.build(ConstSpan<double>{origin, 2}, ConstSpan<double>{spacing, 2}, ConstSpan<usize>{count, 2}, 2,
                     ConstSpan<double>{vb, 20}) == InterpStatus::Ok);
    for (auto& q : q2)
    {
        const double fb = 1.0 + 2.0 * q[0] - 1.5 * q[1] + 0.7 * q[0] * q[1];
        CHECK(close(gb.eval_linear(ConstSpan<double>{q, 2}), fb));
    }

    // edge-clamp: a far out-of-range query returns a finite value (no NaN)
    constexpr double far[2] = {100.0, -50.0};
    CHECK(is_fin(g.eval_linear(ConstSpan<double>{far, 2})));
}

TEST_CASE("v13-f: N-linear 3-D vs scipy + determinism", "[v13-f][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    constexpr double origin[3] = {0.0, 0.0, 0.0};
    constexpr double spacing[3] = {1.0, 1.0, 1.0};
    constexpr usize count[3] = {4, 4, 4};
    constexpr double v3[64] = {2,  2,    2,  2,    1,  2,    3,  4,    0, 2,    4,  6,    -1, 2,    5,  8,
                               3,  3.5,  4,  4.5,  2,  3.5,  5,  6.5,  1, 3.5,  6,  8.5,  0,  3.5,  7,  10.5,
                               6,  7,    8,  9,    5,  7,    9,  11,   4, 7,    10, 13,   3,  7,    11, 15,
                               11, 12.5, 14, 15.5, 10, 12.5, 15, 17.5, 9, 12.5, 16, 19.5, 8,  12.5, 17, 21.5};
    RegularGridInterpolant<double> g(&alloc);
    REQUIRE(g.build(ConstSpan<double>{origin, 3}, ConstSpan<double>{spacing, 3}, ConstSpan<usize>{count, 3}, 3,
                    ConstSpan<double>{v3, 64}) == InterpStatus::Ok);
    constexpr double q3[3][3] = {{1.3, 0.7, 2.1}, {2.6, 1.1, 0.4}, {0.5, 2.8, 1.9}};
    constexpr double ref3[3] = {6.0350000000000001, 8.8600000000000012, 5.4949999999999992};
    for (int i = 0; i < 3; ++i)
    {
        CHECK(close(g.eval_linear(ConstSpan<double>{q3[i], 3}), ref3[i]));
    }
    // determinism: bit-identical across evals
    CHECK(g.eval_linear(ConstSpan<double>{q3[0], 3}) == g.eval_linear(ConstSpan<double>{q3[0], 3}));
}

TEST_CASE("v13-f: bicubic vs MATLAB interpn('cubic') + reproduction", "[v13-f][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    constexpr double origin[2] = {0.0, 0.0};
    constexpr double spacing[2] = {1.0, 1.0};
    constexpr usize count[2] = {6, 6};
    constexpr double vc[36] = {1, 2.5, 6,  11.5, 19, 28.5, 0, 2.5,  7,  13.5, 22, 32.5, 1,  4.5,  10, 17.5, 27, 38.5,
                               4, 8.5, 15, 23.5, 34, 46.5, 9, 14.5, 22, 31.5, 43, 56.5, 16, 22.5, 31, 41.5, 54, 68.5};
    RegularGridInterpolant<double> g(&alloc);
    REQUIRE(g.build(ConstSpan<double>{origin, 2}, ConstSpan<double>{spacing, 2}, ConstSpan<usize>{count, 2}, 2,
                    ConstSpan<double>{vc, 36}) == InterpStatus::Ok);
    constexpr double qc[4][2] = {{1.3, 1.7}, {2.7, 2.2}, {3.4, 3.8}, {2.1, 1.4}};
    constexpr double refc[4] = {6.0400000000000018, 14.770000000000003, 35.020000000000003, 6.8100000000000005};
    for (int i = 0; i < 4; ++i)
    {
        CHECK(close(g.eval_cubic(ConstSpan<double>{qc[i], 2}), refc[i], 1e-9)); // vs MATLAB interpn('cubic')
    }
    // interpolation property: exact at every grid node (incl. clamped boundary)
    for (usize xi = 0; xi < 6; ++xi)
    {
        for (usize yj = 0; yj < 6; ++yj)
        {
            const double q[2] = {static_cast<double>(xi), static_cast<double>(yj)};
            CHECK(close(g.eval_cubic(ConstSpan<double>{q, 2}), vc[xi * 6 + yj], 1e-10));
        }
    }
    // linear reproduction: f = 1 + 2x − 1.5y reproduced exactly (Keys reproduces linear)
    double vl[36];
    for (usize xi = 0; xi < 6; ++xi)
    {
        for (usize yj = 0; yj < 6; ++yj)
        {
            vl[xi * 6 + yj] = 1.0 + 2.0 * static_cast<double>(xi) - 1.5 * static_cast<double>(yj);
        }
    }
    RegularGridInterpolant<double> gl(&alloc);
    REQUIRE(gl.build(ConstSpan<double>{origin, 2}, ConstSpan<double>{spacing, 2}, ConstSpan<usize>{count, 2}, 2,
                     ConstSpan<double>{vl, 36}) == InterpStatus::Ok);
    for (auto& q : qc)
    {
        const double fl = 1.0 + 2.0 * q[0] - 1.5 * q[1];
        CHECK(close(gl.eval_cubic(ConstSpan<double>{q, 2}), fl, 1e-10));
    }
}

TEST_CASE("v13-f: cubic B-spline (Unser prefilter) vs scipy.ndimage", "[v13-f][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    // 1-D prefilter coefficients vs scipy.ndimage.spline_filter1d(order=3, mode='mirror')
    constexpr double v1[8] = {1, 3, 2, 5, 4, 6, 2, 1};
    constexpr double c1ref[8] = {-0.84335279972518029, 4.6867055994503612, 0.096530401923737894, 6.9271727928546891,
                                 2.1947784266575061,   8.2937135005152882, 0.63036757128134668,  1.184816214359327};
    constexpr double o1[1] = {0.0};
    constexpr double s1[1] = {1.0};
    constexpr usize n1[1] = {8};
    RegularGridInterpolant<double> g1(&alloc);
    REQUIRE(g1.build(ConstSpan<double>{o1, 1}, ConstSpan<double>{s1, 1}, ConstSpan<usize>{n1, 1}, 1,
                     ConstSpan<double>{v1, 8}) == InterpStatus::Ok);
    REQUIRE(g1.build_bspline() == InterpStatus::Ok);
    for (int i = 0; i < 8; ++i)
    {
        CHECK(close(g1.coefficients()[i], c1ref[i], 1e-11)); // exact Unser prefilter
    }

    // 2-D eval vs scipy.ndimage.map_coordinates(order=3, mode='mirror') on the 6×6 grid
    constexpr double origin[2] = {0.0, 0.0};
    constexpr double spacing[2] = {1.0, 1.0};
    constexpr usize count[2] = {6, 6};
    constexpr double vc[36] = {1, 2.5, 6,  11.5, 19, 28.5, 0, 2.5,  7,  13.5, 22, 32.5, 1,  4.5,  10, 17.5, 27, 38.5,
                               4, 8.5, 15, 23.5, 34, 46.5, 9, 14.5, 22, 31.5, 43, 56.5, 16, 22.5, 31, 41.5, 54, 68.5};
    RegularGridInterpolant<double> g2(&alloc);
    REQUIRE(g2.build(ConstSpan<double>{origin, 2}, ConstSpan<double>{spacing, 2}, ConstSpan<usize>{count, 2}, 2,
                     ConstSpan<double>{vc, 36}) == InterpStatus::Ok);
    REQUIRE(g2.build_bspline() == InterpStatus::Ok);
    constexpr double qc[4][2] = {{1.3, 1.7}, {2.7, 2.2}, {3.4, 3.8}, {2.1, 1.4}};
    constexpr double bs2ref[4] = {6.0219504497607668, 14.919717374968529, 34.09760176479476, 6.9204726587761272};
    for (int i = 0; i < 4; ++i)
    {
        CHECK(close(g2.eval_bspline(ConstSpan<double>{qc[i], 2}), bs2ref[i], 1e-9));
    }
    // interpolation property: exact at every grid node (the defining B-spline property)
    for (usize xi = 0; xi < 6; ++xi)
    {
        for (usize yj = 0; yj < 6; ++yj)
        {
            const double q[2] = {static_cast<double>(xi), static_cast<double>(yj)};
            CHECK(close(g2.eval_bspline(ConstSpan<double>{q, 2}), vc[xi * 6 + yj], 1e-9));
        }
    }
    // determinism
    CHECK(g2.eval_bspline(ConstSpan<double>{qc[0], 2}) == g2.eval_bspline(ConstSpan<double>{qc[0], 2}));
}
