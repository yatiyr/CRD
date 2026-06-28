// v12-p robust estimators — Theil-Sen vs scipy.stats.theilslopes; Hodges-Lehmann vs the Walsh-average median; Huber &
// Tukey-biweight M-estimators vs statsmodels RLM.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/stats/cov_robust.hpp>
#include <crd/hesap/stats/robust.hpp>

#include <crd/core/types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::stats;
using crd::containers::ConstSpan;

namespace
{
constexpr double kA[] = {2.1, 3.4, 1.9, 5.2, 4.1, 2.8, 6.3, 3.0};
constexpr double kX[] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr double kY[] = {1.0, 2.1, 2.9, 4.2, 5.1, 5.8, 7.3, 7.9};

[[nodiscard]] bool close(double a, double b, double tol = 1e-9)
{
    const double d = a < b ? b - a : a - b;
    return d <= tol + tol * (b < 0 ? -b : b);
}
} // namespace

TEST_CASE("v12-p: Theil-Sen / Hodges-Lehmann / Huber / Tukey vs scipy/statsmodels", "[v12-p][stats][robust]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 18);
    const auto a = ConstSpan<double>{kA, 8};
    {
        const auto ts = theil_sen(ConstSpan<double>{kX, 8}, ConstSpan<double>{kY, 8}, &alloc);
        CHECK(close(ts.slope, 1.0));
        CHECK(close(ts.intercept, 1.15));
    }
    CHECK(close(hodges_lehmann(a, &alloc), 3.5));
    CHECK(close(huber_location(a, &alloc), 3.51873888535863, 1e-6)); // statsmodels RLM HuberT
    CHECK(close(tukey_location(a, &alloc), 3.52242270911844, 1e-6)); // statsmodels RLM TukeyBiweight
    {
        const auto r = huber_proposal2(a, &alloc); // statsmodels robust.scale.huber
        CHECK(close(r.location, 3.57041307689454, 1e-6));
        CHECK(close(r.scale, 1.66192770512457, 1e-6));
    }
}

TEST_CASE("v12-p: Ledoit-Wolf / OAS / exact MCD vs sklearn", "[v12-p][stats][robust]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    constexpr double X[] = {1.0, 2.1, 2.0, 3.9, 3.0, 6.2, 4.0, 7.8,  5.0, 10.1,
                            1.5, 3.0, 2.5, 5.1, 3.5, 6.9, 4.5, 9.2,  0.5, 1.1};
    const auto xs = ConstSpan<double>{X, 20};
    {
        const auto r = ledoit_wolf(xs, 10, 2, &alloc);
        CHECK(close(r.shrinkage, 0.158005628479773));
        CHECK(close(r.cov[0], 2.55104550297803));
        CHECK(close(r.cov[1], 3.46901681066333));
        CHECK(close(r.cov[3], 7.75785449702196));
    }
    {
        const auto r = oas(xs, 10, 2, &alloc);
        CHECK(close(r.shrinkage, 0.363983766556322));
        CHECK(close(r.cov[0], 3.18791960700382));
        CHECK(close(r.cov[1], 2.62038688178795));
        CHECK(close(r.cov[3], 7.12098039299618));
    }
    {
        const auto r = mcd_exact(xs, 10, 2, &alloc);
        CHECK(close(r.location[0], 2.08333333333333));
        CHECK(close(r.location[1], 4.13333333333333));
        CHECK(close(r.determinant, 0.00393333333333423, 1e-7));
        CHECK(close(r.cov[0], 1.94166666666667));
        CHECK(close(r.cov[1], 3.72666666666667));
        CHECK(close(r.cov[3], 7.15466666666667));
    }
}
