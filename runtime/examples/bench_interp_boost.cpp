// bench_interp_boost.cpp — the hesap-interp interpolants against Boost.Math's (its peer library, boost_peer_rows.hpp),
// single thread, in cache: PCHIP and makima on n = 1000 non-uniform knots (100k sorted and 100k random queries),
// Floater-Hormann of order 3 on n = 30 (10k queries), the 1-D interpolating cubic B-spline on 1000 uniform samples and
// the bilinear blend of a 100 x 100 field (100k random queries each). Every evaluation row compares the two sides'
// values and a mismatch prints !MISMATCH! and fails the run.
//
// The rows compare one definition on both sides. Boost's default PCHIP and makima end slopes are its own estimate,
// not the three-point shape-limited formula of scipy and MATLAB that Cerid builds, so Boost is handed Cerid's two
// end slopes (taken outside the timed passes); the interior slopes are each side's own. Boost's cardinal cubic
// B-spline with zero end derivatives is the interpolating spline of Cerid's mirror-boundary prefilter (the mirrored
// extension is even about each end). A Boost build copies its samples into the containers its API takes and the
// interpolant is destroyed after each timed build; Cerid's interpolants reference the caller's samples and reuse
// their storage.
//
// Build and run at every CPU level: bash scripts/run_bench_boost.sh. The argument is the passes per round (default
// 16) for a smoke run.

#include "boost_peer_rows.hpp"

#include <crd/hesap/interp/interp.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace ip = crd::hesap::interp;
using crd::containers::ConstSpan;
using crd_peer::boost_math::Curve;
using crd_peer::boost_math::Field;
using crd_peer::boost_math::Interpolant;
using crd_peer::boost_math::Interpolator;

namespace
{
constexpr crd::usize kKnots = 1000;
constexpr crd::usize kQueries = 100000;
constexpr crd::usize kRationalKnots = 30;
constexpr crd::usize kRationalQueries = 10000;
constexpr crd::usize kSide = 100;
constexpr auto kPeerKnots = static_cast<crd_peer::boost_math::Index>(kKnots);
constexpr auto kPeerRationalKnots = static_cast<crd_peer::boost_math::Index>(kRationalKnots);
constexpr auto kPeerSide = static_cast<crd_peer::boost_math::Index>(kSide);
// The hesap-interp reference gates against scipy (tests/numerics/hesap-interp: test_piecewise.cpp,
// test_akima_bary.cpp, test_grid.cpp).
constexpr boost_rows::Bound kPiecewiseGate{1e-12, 1e-12};
constexpr boost_rows::Bound kRationalGate{1e-10, 1e-10};
constexpr boost_rows::Bound kSplineGate{1e-11, 1e-11};

double g_x[kKnots];
double g_y[kKnots];
double g_sorted[kQueries];
double g_random[kQueries];
double g_rx[kRationalKnots];
double g_ry[kRationalKnots];
double g_rq[kRationalQueries];
double g_samples[kKnots];
double g_line[kQueries];
double g_field[kSide * kSide];
double g_qx[kQueries];
double g_qy[kQueries];
double g_point[2 * kQueries]; // (row coordinate, column coordinate) per query: Cerid's axis order
double g_cerid[kQueries];
double g_boost[kQueries];
int g_passes = 16;
crd::u64 g_state = 12345;

double rnd()
{
    g_state = g_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(g_state >> 11) / static_cast<double>(static_cast<crd::u64>(1) << 53);
}

void require_built(ip::InterpStatus status, const char* what)
{
    if (status != ip::InterpStatus::Ok)
    {
        std::fprintf(stderr, "%s: build failed (status %d)\n", what, static_cast<int>(status));
        std::exit(1);
    }
}

// The fastest of five rounds of g_passes builds on each side, in microseconds per build.
template <typename Cerid, typename Peer> void build_row(const char* name, const Cerid& cerid, const Peer& peer)
{
    const auto best_us = [](const auto& build)
    {
        double best = 1e300;
        for (int round = 0; round < 5; ++round)
        {
            const auto start = std::chrono::steady_clock::now();
            for (int p = 0; p < g_passes; ++p)
            {
                build();
            }
            const auto stop = std::chrono::steady_clock::now();
            const double us = std::chrono::duration<double, std::micro>(stop - start).count() / g_passes;
            best = us < best ? us : best;
        }
        return best;
    };
    const double t_cerid = best_us(cerid);
    const double t_boost = best_us([&] { crd_peer::boost_math::destroy(peer()); });
    std::printf("%-16s %9.2f  %9.2f   %6.2fx %s  (us per build)\n", name, t_cerid, t_boost, t_boost / t_cerid,
                t_boost > t_cerid ? "WIN " : "lose");
}

// The evaluation row of a built Boost interpolant against cerid(q) over n queries (y: a field's second coordinate).
template <typename Cerid>
bool eval_row(const char* name, const Cerid& cerid, const Interpolator& boost, const double* x, const double* y,
              crd::usize n, boost_rows::Bound bound)
{
    const auto cerid_pass = [&](double* out)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            out[i] = cerid(i);
        }
    };
    const auto boost_pass = [&](double* out)
    {
        crd_peer::boost_math::eval(boost, x, y, out, static_cast<crd_peer::boost_math::Index>(n));
    };
    return boost_rows::compare(name, cerid_pass, boost_pass, static_cast<crd_peer::boost_math::Index>(n), g_cerid,
                               g_boost, bound, 5, g_passes);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        g_passes = static_cast<int>(std::strtol(argv[1], nullptr, 10));
        if (g_passes < 1)
        {
            std::fprintf(stderr, "usage: bench_interp_boost [passes per round >= 1]\n");
            return 2;
        }
    }
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 26);
    double at = 0.0;
    for (crd::usize i = 0; i < kKnots; ++i)
    {
        at += 0.5 + rnd();
        g_x[i] = at;
        g_y[i] = rnd() * 10.0 - 5.0;
    }
    const double lo = g_x[0];
    const double hi = g_x[kKnots - 1];
    for (crd::usize i = 0; i < kQueries; ++i)
    {
        g_sorted[i] = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(kQueries);
        g_random[i] = lo + (hi - lo) * rnd();
    }
    at = 0.0;
    for (crd::usize i = 0; i < kRationalKnots; ++i)
    {
        at += 0.5 + rnd();
        g_rx[i] = at;
        g_ry[i] = rnd() * 10.0 - 5.0;
    }
    for (crd::usize i = 0; i < kRationalQueries; ++i)
    {
        g_rq[i] = g_rx[0] +
                  (g_rx[kRationalKnots - 1] - g_rx[0]) * static_cast<double>(i) / static_cast<double>(kRationalQueries);
    }
    for (crd::usize i = 0; i < kKnots; ++i)
    {
        g_samples[i] = rnd();
    }
    for (crd::usize i = 0; i < kQueries; ++i)
    {
        g_line[i] = rnd() * static_cast<double>(kKnots - 1);
    }
    for (crd::usize i = 0; i < kSide * kSide; ++i)
    {
        g_field[i] = rnd();
    }
    for (crd::usize i = 0; i < kQueries; ++i)
    {
        g_qy[i] = rnd() * static_cast<double>(kSide - 1);
        g_qx[i] = rnd() * static_cast<double>(kSide - 1);
        g_point[2 * i] = g_qy[i];
        g_point[2 * i + 1] = g_qx[i];
    }

    boost_rows::header("v13 interpolation, Cerid vs Boost.Math, ns/query (single-thread, in-cache)");
    bool ok = true;
    const ConstSpan<double> xs{g_x, kKnots};
    const ConstSpan<double> ys{g_y, kKnots};

    // PCHIP and makima: Boost takes Cerid's end slopes, so both sides build the same interpolant.
    ip::PchipInterpolant<double> pchip(&alloc);
    require_built(pchip.build(xs, ys), "PCHIP");
    const Curve pchip_curve{g_x, g_y, kPeerKnots, pchip.slopes()[0], pchip.slopes()[kKnots - 1], 0};
    build_row(
        "PCHIP n1000", [&] { require_built(pchip.build(xs, ys), "PCHIP"); },
        [&] { return crd_peer::boost_math::build(Interpolant::Pchip, pchip_curve); });
    {
        const crd_peer::Owned<Interpolator> boost(crd_peer::boost_math::build(Interpolant::Pchip, pchip_curve));
        ok &= eval_row(
            "PCHIP sorted", [&](crd::usize i) { return pchip.eval(g_sorted[i]); }, *boost.get(), g_sorted, nullptr,
            kQueries, kPiecewiseGate);
        ok &= eval_row(
            "PCHIP random", [&](crd::usize i) { return pchip.eval(g_random[i]); }, *boost.get(), g_random, nullptr,
            kQueries, kPiecewiseGate);
    }
    ip::AkimaInterpolant<double> makima(&alloc);
    require_built(makima.build(xs, ys, true), "makima");
    const Curve makima_curve{g_x, g_y, kPeerKnots, makima.slopes()[0], makima.slopes()[kKnots - 1], 0};
    build_row(
        "makima n1000", [&] { require_built(makima.build(xs, ys, true), "makima"); },
        [&] { return crd_peer::boost_math::build(Interpolant::Makima, makima_curve); });
    {
        const crd_peer::Owned<Interpolator> boost(crd_peer::boost_math::build(Interpolant::Makima, makima_curve));
        ok &= eval_row(
            "makima sorted", [&](crd::usize i) { return makima.eval(g_sorted[i]); }, *boost.get(), g_sorted, nullptr,
            kQueries, kPiecewiseGate);
        ok &= eval_row(
            "makima random", [&](crd::usize i) { return makima.eval(g_random[i]); }, *boost.get(), g_random, nullptr,
            kQueries, kPiecewiseGate);
    }

    // Floater-Hormann of order 3 (Boost's barycentric_rational).
    const ConstSpan<double> rxs{g_rx, kRationalKnots};
    const ConstSpan<double> rys{g_ry, kRationalKnots};
    ip::FloaterHormannInterpolant<double> rational(&alloc);
    require_built(rational.build(rxs, rys, 3), "Floater-Hormann");
    const Curve rational_curve{g_rx, g_ry, kPeerRationalKnots, 0.0, 0.0, 3};
    build_row(
        "FloaterHorm n30", [&] { require_built(rational.build(rxs, rys, 3), "Floater-Hormann"); },
        [&] { return crd_peer::boost_math::build(Interpolant::FloaterHormann, rational_curve); });
    {
        const crd_peer::Owned<Interpolator> boost(
            crd_peer::boost_math::build(Interpolant::FloaterHormann, rational_curve));
        ok &= eval_row(
            "FloaterHorm n30", [&](crd::usize i) { return rational.eval(g_rq[i]); }, *boost.get(), g_rq, nullptr,
            kRationalQueries, kRationalGate);
    }

    // The 1-D interpolating cubic B-spline on unit-spaced samples from 0.
    constexpr double origin_1d[1] = {0.0};
    constexpr double spacing_1d[1] = {1.0};
    constexpr crd::usize count_1d[1] = {kKnots};
    constexpr double axis[2] = {0.0, 1.0}; // Boost reads the left end and the spacing from x[0] and x[1]
    ip::RegularGridInterpolant<double> line(&alloc);
    const auto build_line = [&]
    {
        require_built(line.build(ConstSpan<double>{origin_1d, 1}, ConstSpan<double>{spacing_1d, 1},
                                 ConstSpan<crd::usize>{count_1d, 1}, 1, ConstSpan<double>{g_samples, kKnots}),
                      "grid 1D");
        require_built(line.build_bspline(), "grid-bspline 1D");
    };
    build_line();
    const Curve spline_curve{axis, g_samples, kPeerKnots, 0.0, 0.0, 0};
    build_row("cubic B-spline", build_line,
              [&] { return crd_peer::boost_math::build(Interpolant::CardinalCubic, spline_curve); });
    {
        const crd_peer::Owned<Interpolator> boost(
            crd_peer::boost_math::build(Interpolant::CardinalCubic, spline_curve));
        ok &= eval_row(
            "cubic B-spline", [&](crd::usize i) { return line.eval_bspline(ConstSpan<double>{&g_line[i], 1}); },
            *boost.get(), g_line, nullptr, kQueries, kSplineGate);
    }

    // The bilinear blend of a 100 x 100 field: Cerid's axis 0 is Boost's y (rows), axis 1 its x (columns).
    constexpr double origin_2d[2] = {0.0, 0.0};
    constexpr double spacing_2d[2] = {1.0, 1.0};
    constexpr crd::usize count_2d[2] = {kSide, kSide};
    ip::RegularGridInterpolant<double> grid(&alloc);
    const auto build_grid = [&]
    {
        require_built(grid.build(ConstSpan<double>{origin_2d, 2}, ConstSpan<double>{spacing_2d, 2},
                                 ConstSpan<crd::usize>{count_2d, 2}, 2, ConstSpan<double>{g_field, kSide * kSide}),
                      "grid 2D");
    };
    build_grid();
    const Field field{g_field, kPeerSide, kPeerSide, 1.0, 1.0, 0.0, 0.0};
    build_row("bilinear 100x100", build_grid, [&] { return crd_peer::boost_math::build(field); });
    {
        const crd_peer::Owned<Interpolator> boost(crd_peer::boost_math::build(field));
        ok &= eval_row(
            "bilinear 2D", [&](crd::usize i) { return grid.eval_linear(ConstSpan<double>{&g_point[2 * i], 2}); },
            *boost.get(), g_qx, g_qy, kQueries, kPiecewiseGate);
    }
    return ok ? 0 : 1;
}
