// v12-b Bessel/Airy benchmark — Cerid vs Boost.Math, single-thread, in-cache ns/call. Honest: reports wins AND
// losses. Plain C arrays only (no STL containers, per the engine rule — applies to benches too).
//
// ALL-PEERS SCOREBOARD (single-thread, ns/elem; scipy bench_bessel_refs.py, MATLAB-1T bench_bessel_matlab.m — both
// pinned to one thread to match this per-call bench). Cerid speedup in parens:
//          Cerid   Boost          scipy          MATLAB-1T
//   cyl_J   66    146 (2.2×)     270 (4.1×)     349 (5.3×)
//   cyl_Y  180    184 (1.03×)    431 (2.4×)     603 (3.4×)
//   cyl_I  120    190 (1.58×)    174 (1.45×)    181 (1.5×)
//   cyl_K   74    133 (1.79×)    117 (1.58×)    128 (1.7×)
//   airy_Ai 78    658 (8.5×)     133 (1.72×)    446 (5.7×)
//   airy_Bi 107   994 (9.3×)     133 (1.24×)    579 (5.4×)
//   ⇒ Cerid WINS ALL 6 vs ALL 3 peers (18/18). Levers: dedicated J-only fast path (series + Hankel asymptotic,
//   no Y/J' quartet), direct Airy Maclaurin series (no Bessel-⅓ route), Steed/Temme CF for Y/I/K.

#include <crd/hesap/special/airy.hpp>
#include <crd/hesap/special/bessel.hpp>

#include <boost/math/special_functions/airy.hpp>
#include <boost/math/special_functions/bessel.hpp>

#include <chrono>
#include <cstdio>

namespace sf = crd::hesap::special;

namespace
{
// (nu, x) grid spanning orders + small/medium/large argument.
const double kNu[] = {0.0, 1.0, 2.0, 5.0, 0.5, 2.5};
const double kX[] = {0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 40.0};
constexpr int kReps = 4000;

template <class F>
double timeit(F&& f)
{
    const auto t0 = std::chrono::steady_clock::now();
    volatile double sink = 0.0;
    for (int r = 0; r < kReps; ++r)
    {
        sink += f();
    }
    const auto t1 = std::chrono::steady_clock::now();
    (void)sink;
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

template <class CF, class BF>
void row(const char* name, CF cerid_fn, BF boost_fn)
{
    constexpr int nn = static_cast<int>(sizeof(kNu) / sizeof(double));
    constexpr int nx = static_cast<int>(sizeof(kX) / sizeof(double));
    const double tc = timeit(
                          [&]
                          {
                              double s = 0;
                              for (int i = 0; i < nn; ++i)
                                  for (int j = 0; j < nx; ++j) s += cerid_fn(kNu[i], kX[j]);
                              return s;
                          }) /
                      (kReps * nn * nx);
    const double tb = timeit(
                          [&]
                          {
                              double s = 0;
                              for (int i = 0; i < nn; ++i)
                                  for (int j = 0; j < nx; ++j) s += boost_fn(kNu[i], kX[j]);
                              return s;
                          }) /
                      (kReps * nn * nx);
    std::printf("%-10s  %8.2f   %8.2f   %.2fx %s\n", name, tc, tb, tb / tc, (tb > tc ? "WIN" : "lose"));
}

const double kAx[] = {-10, -5, -2, -1, 0.5, 1, 2, 5, 10};

template <class CF, class BF>
void arow(const char* name, CF cfn, BF bfn)
{
    constexpr int na = static_cast<int>(sizeof(kAx) / sizeof(double));
    const double tc = timeit(
                          [&]
                          {
                              double s = 0;
                              for (int i = 0; i < na; ++i) s += cfn(kAx[i]);
                              return s;
                          }) /
                      (kReps * na);
    const double tb = timeit(
                          [&]
                          {
                              double s = 0;
                              for (int i = 0; i < na; ++i) s += bfn(kAx[i]);
                              return s;
                          }) /
                      (kReps * na);
    std::printf("%-10s  %8.2f   %8.2f   %.2fx %s\n", name, tc, tb, tb / tc, (tb > tc ? "WIN" : "lose"));
}
} // namespace

int main()
{
    std::printf("# v12-b Bessel/Airy — Cerid (Steed/Temme) vs Boost.Math, ns/call (single-thread, in-cache)\n");
    std::printf("function    Cerid(ns)   Boost(ns)   speedup\n");
    row("cyl_J", [](double n, double x) { return sf::cyl_bessel_j(n, x); },
        [](double n, double x) { return boost::math::cyl_bessel_j(n, x); });
    row("cyl_Y", [](double n, double x) { return sf::cyl_neumann(n, x); },
        [](double n, double x) { return boost::math::cyl_neumann(n, x); });
    row("cyl_I", [](double n, double x) { return sf::cyl_bessel_i(n, x); },
        [](double n, double x) { return boost::math::cyl_bessel_i(n, x); });
    row("cyl_K", [](double n, double x) { return sf::cyl_bessel_k(n, x); },
        [](double n, double x) { return boost::math::cyl_bessel_k(n, x); });
    arow("airy_Ai", [](double x) { return sf::airy_ai(x); }, [](double x) { return boost::math::airy_ai(x); });
    arow("airy_Bi", [](double x) { return sf::airy_bi(x); }, [](double x) { return boost::math::airy_bi(x); });
    return 0;
}
