// v12-p KDE — Gaussian gated bit-for-bit vs scipy.stats.gaussian_kde; bandwidth selectors vs scipy/statsmodels;
// Epanechnikov vs the textbook formula; CV bandwidth vs a leave-one-out-ML grid search.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/stats/kde.hpp>

#include <crd/core/types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::stats;
using crd::containers::ConstSpan;

namespace
{
constexpr double kA[] = {2.1, 3.4, 1.9, 5.2, 4.1, 2.8, 6.3, 3.0};

[[nodiscard]] bool close(double a, double b, double tol = 1e-9)
{
    const double d = a < b ? b - a : a - b;
    return d <= tol + tol * (b < 0 ? -b : b);
}
} // namespace

TEST_CASE("v12-p: Gaussian/Epanechnikov KDE + bandwidth selectors vs scipy/statsmodels", "[v12-p][stats][kde]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 18);
    const auto a = ConstSpan<double>{kA, 8};
    constexpr double pts[] = {2.0, 3.0, 4.0, 5.0};

    CHECK(close(kde_scott_factor<double>(8), 0.659753955386447));
    CHECK(close(kde_silverman_factor<double>(8), 0.698827118771579));

    { // Gaussian KDE (scott + silverman) vs scipy.stats.gaussian_kde
        const double hs = kde_bandwidth_scott(a);
        constexpr double scott_ref[] = {0.189772769218556, 0.236473547180447, 0.187309278842805, 0.129942701953649};
        for (int i = 0; i < 4; ++i)
        {
            CHECK(close(kde_eval(a, pts[i], hs, KdeKernel::Gaussian), scott_ref[i]));
        }
        const double hv = kde_bandwidth_silverman(a);
        constexpr double silver_ref[] = {0.185593847226366, 0.230015797946663, 0.187119404237937, 0.13156518839016};
        for (int i = 0; i < 4; ++i)
        {
            CHECK(close(kde_eval(a, pts[i], hv, KdeKernel::Gaussian), silver_ref[i]));
        }
    }

    { // Epanechnikov (h=2.0) vs the textbook formula
        constexpr double epa_ref[] = {0.191953125, 0.2410546875, 0.189140625, 0.127734375};
        for (int i = 0; i < 4; ++i)
        {
            CHECK(close(kde_eval(a, pts[i], 2.0, KdeKernel::Epanechnikov), epa_ref[i]));
        }
    }

    CHECK(close(bw_silverman_rot(a, &alloc), 0.77028352834222));         // statsmodels bw_silverman
    CHECK(close(bw_cv(a, 0.3, 2.0, 341, &alloc), 1.255, 1e-9));          // leave-one-out-ML grid max
}
