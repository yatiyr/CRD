// v12-b Bessel/Airy benchmark — Cerid vs Boost.Math (its peer library, special_peer_rows.hpp, values compared on every
// row), single-thread, in-cache ns/element. Honest: reports wins AND losses. Plain C arrays only (no STL containers,
// per the engine rule — applies to benches too).
//
// ALL-PEERS SCOREBOARD (2026-06; Boost then compiled inside this unit; single-thread, ns/elem; scipy
// bench_bessel_refs.py, MATLAB-1T bench_bessel_matlab.m — both pinned to one thread to match this per-call bench).
// Cerid speedup in parens:
//          Cerid   Boost          scipy          MATLAB-1T
//   cyl_J   66    146 (2.2×)     270 (4.1×)     349 (5.3×)
//   cyl_Y  180    184 (1.03×)    431 (2.4×)     603 (3.4×)
//   cyl_I  120    190 (1.58×)    174 (1.45×)    181 (1.5×)
//   cyl_K   74    133 (1.79×)    117 (1.58×)    128 (1.7×)
//   airy_Ai 78    658 (8.5×)     133 (1.72×)    446 (5.7×)
//   airy_Bi 107   994 (9.3×)     133 (1.24×)    579 (5.4×)
//   ⇒ Cerid WINS ALL 6 vs ALL 3 peers (18/18). Levers: dedicated J-only fast path (series + Hankel asymptotic,
//   no Y/J' quartet), direct Airy Maclaurin series (no Bessel-⅓ route), Steed/Temme CF for Y/I/K.
//
// Build and run at every CPU level: bash scripts/run_bench_special.sh. The argument is the passes per round (default
// 64) for a smoke run.

#include "special_peer_rows.hpp"

#include <crd/hesap/special/airy.hpp>
#include <crd/hesap/special/bessel.hpp>

#include <cstdio>
#include <cstdlib>

namespace sf = crd::hesap::special;
using special_peer::Special;

namespace
{
// (nu, x) grid spanning orders + small/medium/large argument, and the Airy arguments; each tiled kTiles times.
const double kNu[] = {0.0, 1.0, 2.0, 5.0, 0.5, 2.5};
const double kX[] = {0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 40.0};
const double kAx[] = {-10, -5, -2, -1, 0.5, 1, 2, 5, 10};
constexpr int kOrders = static_cast<int>(sizeof(kNu) / sizeof(double));
constexpr int kArguments = static_cast<int>(sizeof(kX) / sizeof(double));
constexpr int kAiry = static_cast<int>(sizeof(kAx) / sizeof(double));
constexpr int kTiles = 64;
constexpr int kPairs = kOrders * kArguments * kTiles;
constexpr int kAiryN = kAiry * kTiles;

double g_nu[kPairs];
double g_x[kPairs];
double g_ax[kAiryN];
double g_cerid[kPairs];
double g_boost[kPairs];
int g_passes = 64;

template <class F> bool pair_row(const char* name, F f, Special peer, special_peer::Bound bound)
{
    const auto cerid = [&](double* out)
    {
        for (int i = 0; i < kPairs; ++i)
        {
            out[i] = f(g_nu[i], g_x[i]);
        }
    };
    return special_peer::row(name, cerid, peer, {g_nu, g_x, nullptr}, kPairs, g_cerid, g_boost, bound, 5, g_passes);
}

template <class F> bool airy_row(const char* name, F f, Special peer, special_peer::Bound bound)
{
    const auto cerid = [&](double* out)
    {
        for (int i = 0; i < kAiryN; ++i)
        {
            out[i] = f(g_ax[i]);
        }
    };
    return special_peer::row(name, cerid, peer, {g_ax, nullptr, nullptr}, kAiryN, g_cerid, g_boost, bound, 5, g_passes);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        g_passes = static_cast<int>(std::strtol(argv[1], nullptr, 10));
        if (g_passes < 1)
        {
            std::fprintf(stderr, "usage: bench_bessel_vs_refs [passes per round >= 1]\n");
            return 2;
        }
    }
    for (int i = 0; i < kPairs; ++i)
    {
        const int pair = i % (kOrders * kArguments);
        g_nu[i] = kNu[pair / kArguments];
        g_x[i] = kX[pair % kArguments];
    }
    for (int i = 0; i < kAiryN; ++i)
    {
        g_ax[i] = kAx[i % kAiry];
    }
    special_peer::header("v12-b Bessel/Airy, Cerid (Steed/Temme) vs Boost.Math, ns/element (single-thread, in-cache)");
    bool ok = true;
    ok &= pair_row("cyl_J", [](double n, double x) { return sf::cyl_bessel_j(n, x); }, Special::CylBesselJ,
                   {1e-11, 1e-12});
    ok &= pair_row("cyl_Y", [](double n, double x) { return sf::cyl_neumann(n, x); }, Special::CylNeumann,
                   {1e-11, 1e-12});
    ok &= pair_row("cyl_I", [](double n, double x) { return sf::cyl_bessel_i(n, x); }, Special::CylBesselI,
                   {1e-11, 1e-12});
    ok &= pair_row("cyl_K", [](double n, double x) { return sf::cyl_bessel_k(n, x); }, Special::CylBesselK,
                   {1e-12, 1e-300});
    ok &= airy_row("airy_Ai", [](double x) { return sf::airy_ai(x); }, Special::AiryAi, {1e-11, 1e-13});
    ok &= airy_row("airy_Bi", [](double x) { return sf::airy_bi(x); }, Special::AiryBi, {1e-11, 1e-13});
    return ok ? 0 : 1;
}
