// v12-d transcendental tail — Cerid vs Boost.Math (its peer library, special_peer_rows.hpp, values compared on every
// row), single-thread in-cache ns/element (the Boost-overlapping subset). Honest: reports wins AND losses. Plain C
// arrays only (no STL). Each side takes its own convention's arguments, prepared outside the timed passes: the
// elliptic integrals take the parameter m on Cerid's side and the modulus k = √m on Boost's.
//
// Build and run at every CPU level: bash scripts/run_bench_special.sh. The argument is the passes per round (default
// 256) for a smoke run.

#include "special_peer_rows.hpp"

#include <crd/hesap/special/elliptic.hpp>
#include <crd/hesap/special/expint.hpp>
#include <crd/hesap/special/lambertw.hpp>
#include <crd/hesap/special/zeta.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sf = crd::hesap::special;
using special_peer::Special;

namespace
{
const double kBase[] = {0.3, 0.7, 1.2, 2.5, 4.0, 6.0, 9.0, 13.0};
constexpr int kBaseN = static_cast<int>(sizeof(kBase) / sizeof(double));
constexpr int kN = kBaseN * 256;

double g_x[kN]; // x
double g_s[kN]; // zeta's s = x + 1.5
double g_m[kN]; // the elliptic parameter m = x / 14
double g_k[kN]; // its modulus √m
double g_y[kN]; // Carlson's y = x + 1
double g_z[kN]; // Carlson's z = x + 2
double g_cerid[kN];
double g_boost[kN];
int g_passes = 256;

template <class F>
bool tail_row(const char* name, F f, const double* cerid_in, Special peer, const double* peer_in,
              special_peer::Bound bound)
{
    const auto cerid = [&](double* out)
    {
        for (int i = 0; i < kN; ++i)
        {
            out[i] = f(cerid_in[i]);
        }
    };
    return special_peer::row(name, cerid, peer, {peer_in, nullptr, nullptr}, kN, g_cerid, g_boost, bound, 5, g_passes);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        g_passes = static_cast<int>(std::strtol(argv[1], nullptr, 10));
        if (g_passes < 1)
        {
            std::fprintf(stderr, "usage: bench_special_v12d [passes per round >= 1]\n");
            return 2;
        }
    }
    for (int i = 0; i < kN; ++i)
    {
        g_x[i] = kBase[i % kBaseN];
        g_s[i] = g_x[i] + 1.5;
        g_m[i] = g_x[i] / 14.0;
        g_k[i] = std::sqrt(g_m[i]);
        g_y[i] = g_x[i] + 1.0;
        g_z[i] = g_x[i] + 2.0;
    }
    special_peer::header("v12-d transcendental tail, Cerid vs Boost.Math, ns/element (single-thread, in-cache)");
    bool ok = true;
    ok &= tail_row("Ei", [](double x) { return sf::expint_ei(x); }, g_x, Special::Ei, g_x, {1e-12, 1e-13});
    ok &= tail_row("E1", [](double x) { return sf::expint_e1(x); }, g_x, Special::E1, g_x, {1e-12, 1e-13});
    ok &= tail_row("zeta", [](double s) { return sf::riemann_zeta(s); }, g_s, Special::Zeta, g_s, {1e-13, 1e-14});
    ok &=
        tail_row("lambertW0", [](double x) { return sf::lambert_w0(x); }, g_x, Special::LambertW0, g_x, {1e-12, 1e-13});
    ok &= tail_row("ellint_E", [](double m) { return sf::ellint_e(m); }, g_m, Special::Ellint2, g_k, {1e-12, 1e-13});
    ok &= tail_row("ellint_K", [](double m) { return sf::ellint_k(m); }, g_m, Special::Ellint1, g_k, {1e-12, 1e-13});
    {
        const auto cerid = [](double* out)
        {
            for (int i = 0; i < kN; ++i)
            {
                out[i] = sf::elliprf(g_x[i], g_y[i], g_z[i]);
            }
        };
        ok &= special_peer::row("Carlson_RF", cerid, Special::EllintRf, {g_x, g_y, g_z}, kN, g_cerid, g_boost,
                                {1e-12, 1e-13}, 5, g_passes);
    }
    return ok ? 0 : 1;
}
