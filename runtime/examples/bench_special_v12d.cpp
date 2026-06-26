// v12-d transcendental tail — Cerid vs Boost.Math, single-thread in-cache ns/call (the Boost-overlapping subset).
// Honest: reports wins AND losses. Plain C arrays only (no STL). Header-only special fns ⇒ no lib link.

#include <crd/hesap/special/elliptic.hpp>
#include <crd/hesap/special/expint.hpp>
#include <crd/hesap/special/lambertw.hpp>
#include <crd/hesap/special/zeta.hpp>

#include <boost/math/special_functions/ellint_1.hpp>
#include <boost/math/special_functions/ellint_2.hpp>
#include <boost/math/special_functions/ellint_rf.hpp>
#include <boost/math/special_functions/expint.hpp>
#include <boost/math/special_functions/lambert_w.hpp>
#include <boost/math/special_functions/zeta.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

namespace sf = crd::hesap::special;

namespace
{
constexpr int kReps = 20000;
// Runtime-filled (via a volatile seed) so the compiler can NOT constant-fold f(kX[i]) into a compile-time constant —
// otherwise whichever impl folds best wins a phantom (a fixed-const input array invites exactly that). See SANITY #4.
double kX[8];
void fill_inputs() noexcept
{
    const double base[] = {0.3, 0.7, 1.2, 2.5, 4.0, 6.0, 9.0, 13.0};
    volatile double jitter = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        kX[i] = base[i] + static_cast<double>(jitter);
    }
}

template <class F>
double per_call(F f)
{
    constexpr int n = static_cast<int>(sizeof(kX) / sizeof(double));
    f(); // warm
    const auto t0 = std::chrono::steady_clock::now();
    volatile double s = 0.0;
    for (int r = 0; r < kReps; ++r)
    {
        s += f();
    }
    const auto t1 = std::chrono::steady_clock::now();
    (void)s;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / (static_cast<double>(kReps) * n);
}

template <class CF, class BF>
void row(const char* name, CF cf, BF bf)
{
    constexpr int n = static_cast<int>(sizeof(kX) / sizeof(double));
    const double tc = per_call(
        [&]
        {
            double s = 0;
            for (int i = 0; i < n; ++i) s += cf(kX[i]);
            return s;
        });
    const double tb = per_call(
        [&]
        {
            double s = 0;
            for (int i = 0; i < n; ++i) s += bf(kX[i]);
            return s;
        });
    std::printf("%-10s  %8.2f   %8.2f   %.2fx %s\n", name, tc, tb, tb / tc, (tb > tc ? "WIN" : "lose"));
}
} // namespace

int main()
{
    fill_inputs();
    std::printf("# v12-d transcendental tail — Cerid vs Boost.Math, ns/call (single-thread, in-cache)\n");
    std::printf("function    Cerid(ns)   Boost(ns)   speedup\n");
    row("Ei", [](double x) { return sf::expint_ei(x); }, [](double x) { return boost::math::expint(x); });
    row("E1", [](double x) { return sf::expint_e1(x); }, [](double x) { return boost::math::expint(1U, x); });
    row("zeta", [](double x) { return sf::riemann_zeta(x + 1.5); },
        [](double x) { return boost::math::zeta(x + 1.5); });
    row("lambertW0", [](double x) { return sf::lambert_w0(x); }, [](double x) { return boost::math::lambert_w0(x); });
    // elliptic on m∈(0,1): Cerid takes parameter m, Boost takes modulus k=√m.
    row("ellint_E", [](double x) { const double m = x / 14.0; return sf::ellint_e(m); },
        [](double x) { const double m = x / 14.0; return boost::math::ellint_2(std::sqrt(m)); });
    row("ellint_K", [](double x) { const double m = x / 14.0; return sf::ellint_k(m); },
        [](double x) { const double m = x / 14.0; return boost::math::ellint_1(std::sqrt(m)); });
    row("Carlson_RF", [](double x) { return sf::elliprf(x, x + 1.0, x + 2.0); },
        [](double x) { return boost::math::ellint_rf(x, x + 1.0, x + 2.0); });
    return 0;
}
