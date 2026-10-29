#pragma once

// One row of the boards against the Boost.Math peer (examples/peers/boost_peer.hpp): both sides evaluate the same
// workload into an array of their own, each timed as the fastest of `rounds` passes, and the row compares the two
// arrays (PRINCIPLES §3). A Cerid value further from Boost's than the row's bound, the gate of the function's own
// reference test, prints !MISMATCH! and fails the row, so a speed figure on wrong values cannot stand.

#include "boost_peer.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace boost_rows
{
using crd_peer::boost_math::Arguments;
using crd_peer::boost_math::Index;
using crd_peer::boost_math::Special;

// |cerid - boost| <= abs + rel * |boost|: the tolerances of the function's reference test in tests/numerics/.
struct Bound
{
    double rel;
    double abs;
};

// The fastest of `rounds` passes of pass(), each over `passes_per_round` repetitions of n elements, in ns per element.
template <typename Pass> double best_ns(const Pass& pass, Index n, int rounds, int passes_per_round)
{
    double best = 1e300;
    for (int round = 0; round < rounds; ++round)
    {
        const auto start = std::chrono::steady_clock::now();
        for (int p = 0; p < passes_per_round; ++p)
        {
            pass();
        }
        const auto stop = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(stop - start).count() /
                          (static_cast<double>(passes_per_round) * static_cast<double>(n));
        best = ns < best ? ns : best;
    }
    return best;
}

// Times cerid(out_cerid) against peer(out_boost), each writing n values, prints the row and returns whether every
// value keeps the bound.
template <typename Cerid, typename Peer>
bool compare(const char* name, const Cerid& cerid, const Peer& peer, Index n, double* out_cerid, double* out_boost,
             Bound bound, int rounds, int passes_per_round)
{
    const double t_cerid = best_ns([&] { cerid(out_cerid); }, n, rounds, passes_per_round);
    const double t_boost = best_ns([&] { peer(out_boost); }, n, rounds, passes_per_round);
    double worst = 0.0;
    bool ok = true;
    for (Index i = 0; i < n; ++i)
    {
        const double difference = std::fabs(out_cerid[i] - out_boost[i]);
        const double scale = std::fabs(out_boost[i]);
        if (!(difference <= bound.abs + bound.rel * scale))
        {
            ok = false;
        }
        const double relative = difference / (scale > 0.0 ? scale : 1.0);
        worst = relative > worst ? relative : worst;
    }
    std::printf("%-16s %9.2f  %9.2f   %6.2fx %s  max rel %.1e%s\n", name, t_cerid, t_boost, t_boost / t_cerid,
                t_boost > t_cerid ? "WIN " : "lose", worst, ok ? "" : "  !MISMATCH!");
    return ok;
}

// compare() against the peer's special function f over the same arguments.
template <typename Cerid>
bool row(const char* name, const Cerid& cerid, Special f, const Arguments& arguments, Index n, double* out_cerid,
         double* out_boost, Bound bound, int rounds, int passes_per_round)
{
    const auto peer = [&](double* out)
    {
        crd_peer::boost_math::eval(f, arguments, out, n);
    };
    return compare(name, cerid, peer, n, out_cerid, out_boost, bound, rounds, passes_per_round);
}

inline void header(const char* title)
{
    std::printf("# %s\n# %s\n", title, crd_peer::boost_math::isa());
    std::printf("function          Cerid(ns)  Boost(ns)   speedup\n");
}
} // namespace boost_rows
