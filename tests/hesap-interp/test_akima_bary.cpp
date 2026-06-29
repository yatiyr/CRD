// crd-hesap-interp v13-c — Akima/makima (slopes+eval ≤1e-12 vs scipy), barycentric Lagrange + Floater-Hormann
// (≤1e-10 vs scipy), Newton divided-differences (cross-checks barycentric + exact cubic), and the Chebyshev-Runge
// conditioning win (barycentric on Chebyshev nodes does NOT blow up where naive monomial interpolation does).

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/interp/interp.hpp>

#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

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
constexpr double kPi = 3.14159265358979323846;
constexpr double kX[] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr double kY[] = {0, 1, 4, 9, 8, 5, 4, 10};
constexpr double kXq[] = {0.3, 1.7, 2.4, 3.9, 4.4, 5.6, 6.2, 6.8};
constexpr usize kN = 8;
constexpr usize kNq = 8;
} // namespace

TEST_CASE("v13-c: Akima + makima slopes + eval vs scipy", "[v13-c][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    const auto x = ConstSpan<double>{kX, kN};
    const auto y = ConstSpan<double>{kY, kN};

    constexpr double ak_sl[] = {0, 2, 3.5, 2, -2.5, -2.5555555555555554, 0.55555555555555536, 9.5};
    constexpr double ak_ev[] = {0.089999999999999997, 2.9634999999999998, 6.0719999999999992, 8.2484999999999999,
                                6.8293333333333317,   4.0266666666666673, 4.3911111111111127, 8.1777777777777771};
    AkimaInterpolant<double> ak(&alloc);
    REQUIRE(ak.build(x, y, false) == InterpStatus::Ok);
    for (usize i = 0; i < kN; ++i)
    {
        CHECK(close(ak.slopes()[i], ak_sl[i]));
    }
    for (usize i = 0; i < kNq; ++i)
    {
        CHECK(close(ak.eval(kXq[i]), ak_ev[i]));
    }

    constexpr double mk_sl[] = {0, 1.5, 3.6666666666666665, 1.4000000000000004, -2.333333333333333,
                                -2.4074074074074074, 0.36585365853658547, 8.0151515151515174};
    constexpr double mk_ev[] = {0.12150000000000001, 2.9074999999999998, 6.1536,             8.2295999999999996,
                                6.8391111111111105,  4.0682059620596203, 4.4143444198078354, 8.3617679231337743};
    AkimaInterpolant<double> mk(&alloc);
    REQUIRE(mk.build(x, y, true) == InterpStatus::Ok);
    for (usize i = 0; i < kN; ++i)
    {
        CHECK(close(mk.slopes()[i], mk_sl[i]));
    }
    for (usize i = 0; i < kNq; ++i)
    {
        CHECK(close(mk.eval(kXq[i]), mk_ev[i]));
    }
}

TEST_CASE("v13-c: barycentric + Newton + Floater-Hormann vs scipy", "[v13-c][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    const auto x = ConstSpan<double>{kX, kN};
    const auto y = ConstSpan<double>{kY, kN};

    constexpr double bary_ev[] = {2.6037699187499959, 2.2655990312499994, 6.5007615999999988, 8.3065244437499999,
                                  6.664345599999999,  4.1715711999999998, 4.0850559999999998, 6.9135872000000003};
    BarycentricInterpolant<double> bc(&alloc);
    REQUIRE(bc.build(x, y) == InterpStatus::Ok);
    for (usize i = 0; i < kNq; ++i)
    {
        CHECK(close(bc.eval(kXq[i]), bary_ev[i], 1e-10));
    }

    // Newton: the SAME polynomial as barycentric (cross-check), + exact reproduction of a cubic.
    NewtonInterpolant<double> nw(&alloc);
    REQUIRE(nw.build(x, y) == InterpStatus::Ok);
    for (usize i = 0; i < kNq; ++i)
    {
        CHECK(close(nw.eval(kXq[i]), bary_ev[i], 1e-7));
    }
    constexpr double xc[] = {0, 1, 2, 3, 4, 5};
    double yc[6];
    for (usize i = 0; i < 6; ++i)
    {
        const double t = xc[i];
        yc[i] = 2 + 3 * t - t * t + 0.5 * t * t * t; // a cubic
    }
    NewtonInterpolant<double> nwc(&alloc);
    REQUIRE(nwc.build(ConstSpan<double>{xc, 6}, ConstSpan<double>{yc, 6}) == InterpStatus::Ok);
    for (int k = 0; k <= 50; ++k)
    {
        const double xx = 5.0 * static_cast<double>(k) / 50.0;
        const double f = 2 + 3 * xx - xx * xx + 0.5 * xx * xx * xx;
        CHECK(close(nwc.eval(xx), f, 1e-9)); // exact for degree ≤ n−1
    }

    // Floater-Hormann (d=3), pole-free rational.
    constexpr double fh_ev[] = {0.711706018132922,  2.5699397177999148, 6.285223575897267,  8.329243619576463,
                                6.6361039022654484, 4.0490079328044803, 4.3051741160773851, 7.6630793267185169};
    FloaterHormannInterpolant<double> fh(&alloc);
    REQUIRE(fh.build(x, y, 3) == InterpStatus::Ok);
    for (usize i = 0; i < kNq; ++i)
    {
        CHECK(close(fh.eval(kXq[i]), fh_ev[i], 1e-10));
    }
}

TEST_CASE("v13-c: barycentric on Chebyshev nodes beats Runge", "[v13-c][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    constexpr usize nch = 21;
    double xc[nch];
    double yc[nch];
    for (usize i = 0; i < nch; ++i)
    {
        xc[i] = std::cos(kPi * static_cast<double>(nch - 1 - i) / static_cast<double>(nch - 1)); // increasing Chebyshev
        yc[i] = 1.0 / (1.0 + 25.0 * xc[i] * xc[i]);                                               // Runge's function
    }
    BarycentricInterpolant<double> bc(&alloc);
    REQUIRE(bc.build(ConstSpan<double>{xc, nch}, ConstSpan<double>{yc, nch}) == InterpStatus::Ok);
    double maxerr = 0.0;
    for (int k = 0; k <= 500; ++k)
    {
        const double xx = -1.0 + 2.0 * static_cast<double>(k) / 500.0;
        const double v = bc.eval(xx);
        const double f = 1.0 / (1.0 + 25.0 * xx * xx);
        const double e = (v - f) < 0 ? f - v : v - f;
        if (e > maxerr)
        {
            maxerr = e;
        }
    }
    CHECK(maxerr < 0.02); // NO Runge blow-up (naive monomial/equispaced interpolation would be ~O(10²))
    CHECK(close(bc.eval(-0.5), 0.12839144305709008, 1e-9));
    CHECK(close(bc.eval(0.0), 1.0, 1e-9));
    CHECK(close(bc.eval(0.5), 0.12839144305709008, 1e-9));

    // determinism: Akima slopes bit-identical across builds
    AkimaInterpolant<double> a(&alloc);
    AkimaInterpolant<double> b(&alloc);
    REQUIRE(a.build(ConstSpan<double>{kX, kN}, ConstSpan<double>{kY, kN}, true) == InterpStatus::Ok);
    REQUIRE(b.build(ConstSpan<double>{kX, kN}, ConstSpan<double>{kY, kN}, true) == InterpStatus::Ok);
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
