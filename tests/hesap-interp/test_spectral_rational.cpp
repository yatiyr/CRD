// crd-hesap-interp v13-d — spectral + rational. Chebyshev (EXPONENTIAL convergence to exp, coefficient decay),
// trigonometric (band-limited EXACTNESS), Padé (≤1e-10 vs scipy.interpolate.pade + the spurious-pole guard),
// + determinism.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/interp/interp.hpp>

#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using namespace crd::hesap::interp;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::usize;

namespace
{
[[nodiscard]] bool close(double a, double b, double tol = 1e-12)
{
    const double d = a < b ? b - a : a - b;
    return d <= tol + tol * (b < 0 ? -b : b);
}
constexpr double kPi = 3.14159265358979323846;
[[nodiscard]] double ftrig(double x)
{
    return 2.0 + std::cos(x) - 0.5 * std::sin(2.0 * x) + 0.3 * std::cos(3.0 * x);
}
} // namespace

TEST_CASE("v13-d: Chebyshev interpolation -- exponential convergence", "[v13-d][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    constexpr usize n = 16;
    double nodes[n];
    double yv[n];
    chebyshev_nodes<double>(n, -1.0, 1.0, Span<double>{nodes, n});
    for (usize j = 0; j < n; ++j)
    {
        yv[j] = std::exp(nodes[j]);
    }
    ChebyshevInterpolant<double> cheb(&alloc);
    REQUIRE(cheb.build(ConstSpan<double>{yv, n}, -1.0, 1.0) == InterpStatus::Ok);
    double maxerr = 0.0;
    for (int k = 0; k <= 200; ++k)
    {
        const double xx = -1.0 + 2.0 * static_cast<double>(k) / 200.0;
        const double e = std::abs(cheb.eval(xx) - std::exp(xx));
        if (e > maxerr)
        {
            maxerr = e;
        }
    }
    CHECK(maxerr < 1e-12);                              // exp converges to machine precision at N=16 Chebyshev pts
    CHECK(std::abs(cheb.coefficients()[n - 1]) < 1e-12); // coefficient decay

    // eval_batch (the vectorized resampling path) is BIT-IDENTICAL to eval
    double bq[5] = {-0.9, -0.3, 0.1, 0.5, 0.95};
    double bo[5];
    double sb1[5];
    double sb2[5];
    cheb.eval_batch(ConstSpan<double>{bq, 5}, Span<double>{bo, 5}, Span<double>{sb1, 5}, Span<double>{sb2, 5});
    for (int i = 0; i < 5; ++i)
    {
        CHECK(bo[i] == cheb.eval(bq[i]));
    }

    // determinism: bit-identical coefficients across builds
    ChebyshevInterpolant<double> a(&alloc);
    ChebyshevInterpolant<double> b(&alloc);
    REQUIRE(a.build(ConstSpan<double>{yv, n}, -1.0, 1.0) == InterpStatus::Ok);
    REQUIRE(b.build(ConstSpan<double>{yv, n}, -1.0, 1.0) == InterpStatus::Ok);
    bool same = true;
    for (usize j = 0; j < n; ++j)
    {
        if (a.coefficients()[j] != b.coefficients()[j])
        {
            same = false;
        }
    }
    CHECK(same);
}

TEST_CASE("v13-d: trigonometric interpolation -- band-limited exactness", "[v13-d][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    constexpr usize n = 8;
    const double period = 2.0 * kPi;
    double yv[n];
    for (usize k = 0; k < n; ++k)
    {
        yv[k] = ftrig(period * static_cast<double>(k) / static_cast<double>(n));
    }
    TrigInterpolant<double> tr(&alloc);
    REQUIRE(tr.build(ConstSpan<double>{yv, n}, 0.0, period) == InterpStatus::Ok);
    constexpr double xt[] = {0.7, 1.3, 2.9, 4.5, 5.9};
    for (double x : xt)
    {
        CHECK(close(tr.eval(x), ftrig(x), 1e-12)); // exact: f is band-limited (freqs 1,2,3 < N/2=4)
    }
}

TEST_CASE("v13-d: Pade approximation vs scipy + spurious-pole guard", "[v13-d][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    constexpr double c[] = {1.0, 1.0, 0.5, 1.0 / 6, 1.0 / 24, 1.0 / 120, 1.0 / 720}; // exp Taylor c_0..c_6
    RationalPade<double> pade(&alloc);
    REQUIRE(pade.build(ConstSpan<double>{c, 7}, 3, 3) == InterpStatus::Ok); // [3/3]
    constexpr double xq[] = {0.2, 0.5, 1.0, 1.5, 2.0};
    constexpr double ref[] = {1.22140275831551, 1.6487213997308208, 2.7183098591549295,
                              4.4825174825174834, 7.4000000000000004};
    for (int i = 0; i < 5; ++i)
    {
        CHECK(close(pade.eval(xq[i]), ref[i], 1e-10));
    }
    // [3/3] Padé of exp has no real pole ⇒ the denominator stays bounded away from 0 on [0,2]
    CHECK(pade.min_denominator_abs(0.0, 2.0, 200) > 1e-2);
}
