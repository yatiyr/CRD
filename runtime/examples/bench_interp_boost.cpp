// Boost.Math interpolators peer bench — the peer I omitted across v13. Mirrors bench_interp.cpp's setup
// (n=1000 knots / 100k queries for the 1-D local interpolants; n=30 for the rational). Boost's API mandates
// std::vector (this is the PEER measurement, like bench_interp_refs.py uses numpy — not Cerid engine code).
//
// Boost has direct peers for: PCHIP (v13-a), makima (v13-c), barycentric_rational = Floater-Hormann (v13-c).
// It has NO scattered N-D RBF (v13-e N/A) and its cubic B-spline is UNIFORM-knot only (noted, not benched here).

#include <boost/math/interpolators/barycentric_rational.hpp>
#include <boost/math/interpolators/bilinear_uniform.hpp>
#include <boost/math/interpolators/cardinal_cubic_b_spline.hpp>
#include <boost/math/interpolators/makima.hpp>
#include <boost/math/interpolators/pchip.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using clk = std::chrono::steady_clock;
using boost::math::interpolators::barycentric_rational;
using boost::math::interpolators::makima;
using boost::math::interpolators::pchip;

int main()
{
    const std::size_t n = 1000;
    const std::size_t nq = 100000;
    std::uint64_t s = 12345;
    auto rnd = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return double(s >> 11) / double(1ULL << 53);
    };
    std::vector<double> x(n);
    std::vector<double> y(n);
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        acc += 0.5 + rnd();
        x[i] = acc;
        y[i] = rnd() * 10.0 - 5.0;
    }
    std::vector<double> qs(nq);
    std::vector<double> qr(nq);
    for (std::size_t i = 0; i < nq; ++i)
    {
        qs[i] = x[0] + (x[n - 1] - x[0]) * double(i) / double(nq); // sorted (monotone) queries
        qr[i] = x[0] + (x[n - 1] - x[0]) * rnd();                  // random queries
    }
    double chk = 0.0;

    auto bench_build = [&](const char* name, int reps, auto make) {
        const auto t0 = clk::now();
        for (int r = 0; r < reps; ++r)
        {
            std::vector<double> xc = x;
            std::vector<double> yc = y;
            auto interp = make(std::move(xc), std::move(yc));
            chk += interp(x[1]);
        }
        const auto t1 = clk::now();
        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / reps;
        std::printf("%-18s build  %8.2f us/fit  (incl. Boost's mandatory data move)\n", name, us);
    };
    auto bench_eval = [&](const char* name, const char* mode, auto& interp, const std::vector<double>& q) {
        const auto t0 = clk::now();
        for (double v : q)
        {
            chk += interp(v);
        }
        const auto t1 = clk::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / double(q.size());
        std::printf("%-18s eval-%-7s %8.2f ns/pt\n", name, mode, ns);
    };

    // PCHIP (v13-a) — non-uniform knots
    bench_build("PCHIP", 20000, [](std::vector<double>&& a, std::vector<double>&& b) {
        return pchip(std::move(a), std::move(b));
    });
    {
        auto interp = pchip(std::vector<double>(x), std::vector<double>(y));
        bench_eval("PCHIP", "sorted", interp, qs);
        bench_eval("PCHIP", "random", interp, qr);
    }
    // makima (v13-c)
    bench_build("makima", 20000, [](std::vector<double>&& a, std::vector<double>&& b) {
        return makima(std::move(a), std::move(b));
    });
    {
        auto interp = makima(std::vector<double>(x), std::vector<double>(y));
        bench_eval("makima", "sorted", interp, qs);
        bench_eval("makima", "random", interp, qr);
    }
    // barycentric_rational == Floater-Hormann (order 3), n=30
    const std::size_t ng = 30;
    const std::size_t ngq = 10000;
    std::vector<double> gx(ng);
    std::vector<double> gy(ng);
    double ga = 0.0;
    for (std::size_t i = 0; i < ng; ++i)
    {
        ga += 0.5 + rnd();
        gx[i] = ga;
        gy[i] = rnd() * 10.0 - 5.0;
    }
    std::vector<double> gq(ngq);
    for (std::size_t i = 0; i < ngq; ++i)
    {
        gq[i] = gx[0] + (gx[ng - 1] - gx[0]) * double(i) / double(ngq);
    }
    {
        const auto t0 = clk::now();
        for (int r = 0; r < 50000; ++r)
        {
            std::vector<double> a = gx;
            std::vector<double> b = gy;
            barycentric_rational<double> interp(std::move(a), std::move(b), 3);
            chk += interp(gx[1]);
        }
        const auto t1 = clk::now();
        std::printf("%-18s build  %8.2f us/fit  (incl. Boost's mandatory data move)\n", "FloaterHorm n30",
                    std::chrono::duration<double, std::micro>(t1 - t0).count() / 50000.0);
    }
    {
        barycentric_rational<double> interp(std::vector<double>(gx), std::vector<double>(gy), 3);
        bench_eval("FloaterHorm n30", "n30", interp, gq);
    }
    // v13-f: bilinear_uniform (100x100 grid, 100k queries)
    {
        const std::size_t rows = 100;
        const std::size_t cols = 100;
        std::vector<double> field(rows * cols);
        for (double& f : field)
        {
            f = rnd();
        }
        boost::math::interpolators::bilinear_uniform<std::vector<double>> bl(std::move(field), rows, cols, 1.0, 1.0,
                                                                             0.0, 0.0);
        std::vector<double> bqx(100000);
        std::vector<double> bqy(100000);
        for (std::size_t i = 0; i < 100000; ++i)
        {
            bqx[i] = rnd() * double(rows - 1);
            bqy[i] = rnd() * double(cols - 1);
        }
        const auto t0 = clk::now();
        for (std::size_t i = 0; i < 100000; ++i)
        {
            chk += bl(bqx[i], bqy[i]);
        }
        const auto t1 = clk::now();
        std::printf("%-18s eval-grid  %8.2f ns/pt\n", "bilinear 2D",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / 100000.0);
    }
    // v13-f: cardinal_cubic_b_spline (1-D uniform cubic, n=1000, 100k queries) — Boost's only cubic interpolator
    {
        const std::size_t g1n = 1000;
        std::vector<double> g1v(g1n);
        for (double& f : g1v)
        {
            f = rnd();
        }
        boost::math::interpolators::cardinal_cubic_b_spline<double> sp(g1v.data(), g1v.size(), 0.0, 1.0);
        std::vector<double> q1(100000);
        for (double& q : q1)
        {
            q = rnd() * double(g1n - 1);
        }
        const auto t0 = clk::now();
        for (double q : q1)
        {
            chk += sp(q);
        }
        const auto t1 = clk::now();
        std::printf("%-18s eval-1d    %8.2f ns/pt\n", "cardinal_cubic 1D",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / 100000.0);
    }
    std::printf("# chk %.3f\n", chk);
    return 0;
}
