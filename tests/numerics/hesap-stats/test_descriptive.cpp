// v12-m — descriptive sample statistics gated bit-for-bit against numpy/scipy (the moment conventions match:
// skew = Fisher-Pearson g1 biased, kurtosis = Fisher excess biased). Reference values from numpy/scipy.stats on
// the fixed 12-point dataset below.

#include <crd/hesap/stats/descriptive.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace crd::hesap::stats;

[[nodiscard]] bool close(double a, double b) noexcept
{
    return crd::math::fabs(a - b) <= 1e-12 + 1e-12 * crd::math::fabs(b);
}
[[nodiscard]] bool close(double a, double b, double tol) noexcept
{
    return crd::math::fabs(a - b) <= tol + tol * crd::math::fabs(b);
}

constexpr double kData[] = {2.1, 3.4, 1.9, 5.2, 4.1, 2.8, 6.3, 3.0, 4.7, 2.2, 5.9, 3.7};

[[nodiscard]] crd::containers::Span<const double> data() noexcept
{
    return crd::containers::Span<const double>{kData, 12};
}
} // namespace

TEST_CASE("v12-m: sample moments vs numpy/scipy", "[v12-m][stats][descriptive]")
{
    CHECK(close(mean(data()), 3.775));
    CHECK(close(variance(data(), 0), 2.031875));         // np.var(ddof=0)
    CHECK(close(variance(data(), 1), 2.21659090909091)); // np.var(ddof=1)
    CHECK(close(stddev(data(), 0), 1.42543852901484));   // np.std(ddof=0)
    CHECK(close(skewness(data()), 0.367417569388383));   // scipy.stats.skew (g1, biased)
    CHECK(close(kurtosis(data()), -1.11120492845066));   // scipy.stats.kurtosis (Fisher excess, biased)
}

TEST_CASE("v12-m: quantiles R types 1-9 vs numpy", "[v12-m][stats][descriptive]")
{
    constexpr double k_sorted[] = {1.9, 2.1, 2.2, 2.8, 3.0, 3.4, 3.7, 4.1, 4.7, 5.2, 5.9, 6.3};
    constexpr double k_p[] = {0.1, 0.25, 0.5, 0.75, 0.9};
    constexpr double k_ref[9][5] = {
        {2.1, 2.2, 3.4, 4.7, 5.9},                                          // 1 inverted_cdf
        {2.1, 2.5, 3.55, 4.95, 5.9},                                        // 2 averaged_inverted_cdf
        {1.9, 2.2, 3.4, 4.7, 5.9},                                          // 3 closest_observation
        {1.94, 2.2, 3.4, 4.7, 5.76},                                        // 4 interpolated_inverted_cdf
        {2.04, 2.5, 3.55, 4.95, 6.02},                                      // 5 hazen
        {1.96, 2.35, 3.55, 5.075, 6.18},                                    // 6 weibull
        {2.11, 2.65, 3.55, 4.825, 5.83},                                    // 7 linear (default)
        {2.01333333333333, 2.45, 3.55, 4.99166666666667, 6.07333333333333}, // 8 median_unbiased
        {2.02, 2.4625, 3.55, 4.98125, 6.06},                                // 9 normal_unbiased
    };
    const auto sorted = crd::containers::Span<const double>{k_sorted, 12};
    for (int t = 1; t <= 9; ++t)
    {
        for (int pi = 0; pi < 5; ++pi)
        {
            CHECK(close(quantile_sorted(sorted, k_p[pi], t), k_ref[t - 1][pi], 1e-9));
        }
    }
}

TEST_CASE("v12-m: Harrell-Davis quantiles vs scipy.mstats", "[v12-m][stats][descriptive]")
{
    constexpr double k_sorted[] = {1.9, 2.1, 2.2, 2.8, 3.0, 3.4, 3.7, 4.1, 4.7, 5.2, 5.9, 6.3};
    const auto s = crd::containers::Span<const double>{k_sorted, 12};
    CHECK(close(harrell_davis_sorted(s, 0.25), 2.53139437915965, 1e-9));
    CHECK(close(harrell_davis_sorted(s, 0.5), 3.59406083410632, 1e-9));
    CHECK(close(harrell_davis_sorted(s, 0.75), 4.99052097699467, 1e-9));
}

TEST_CASE("v12-m: covariance/correlation matrices vs numpy", "[v12-m][stats][descriptive]")
{
    // 3 variables × 8 observations, row-major.
    constexpr double k_mat[24] = {2.1, 3.4, 1.9, 5.2, 4.1, 2.8, 6.3, 3.0, 1.0, 2.0, 1.5, 3.0,
                                  2.5, 2.0, 3.5, 1.8, 5.0, 4.0, 6.0, 2.0, 3.0, 4.5, 1.0, 4.2};
    constexpr double k_cov[9] = {
        2.32571428571429,  1.19571428571429, -2.46, 1.19571428571429, 0.654107142857143, -1.23803571428571, -2.46,
        -1.23803571428571, 2.66125};
    constexpr double k_cor[9] = {1, 0.969448609096053,  -0.988812822693879, 0.969448609096053,
                                 1, -0.938352229356669, -0.988812822693879, -0.938352229356669,
                                 1};
    double cov[9];
    double cor[9];
    covariance_matrix(crd::containers::Span<const double>{k_mat, 24}, 3, 8, crd::containers::Span<double>{cov, 9}, 1);
    correlation_matrix(crd::containers::Span<const double>{k_mat, 24}, 3, 8, crd::containers::Span<double>{cor, 9});
    for (int i = 0; i < 9; ++i)
    {
        CHECK(close(cov[i], k_cov[i], 1e-9));
        CHECK(close(cor[i], k_cor[i], 1e-9));
    }
}

TEST_CASE("v12-m: robust / weighted / ECDF / histogram bins vs numpy/scipy", "[v12-m][stats][descriptive]")
{
    constexpr double k_sorted[] = {1.9, 2.1, 2.2, 2.8, 3.0, 3.4, 3.7, 4.1, 4.7, 5.2, 5.9, 6.3};
    constexpr double k_raw[] = {2.1, 3.4, 1.9, 5.2, 4.1, 2.8, 6.3, 3.0, 4.7, 2.2, 5.9, 3.7};
    constexpr double k_w[] = {1.0, 2.0, 1.0, 3.0, 1.0, 2.0, 1.0, 1.0, 2.0, 1.0, 3.0, 1.0};
    const auto s = crd::containers::Span<const double>{k_sorted, 12};
    const auto raw = crd::containers::Span<const double>{k_raw, 12};
    const auto w = crd::containers::Span<const double>{k_w, 12};
    CHECK(close(median_sorted(s), 3.55, 1e-9));
    CHECK(close(iqr_sorted(s), 2.175, 1e-9));                    // scipy.stats.iqr
    CHECK(close(trimmed_mean_sorted(s, 0.1), 3.71, 1e-9));       // scipy.stats.trim_mean
    CHECK(close(weighted_mean(raw, w), 4.12631578947368, 1e-9)); // np.average
    CHECK(close(weighted_variance(raw, w), 1.95141274238227, 1e-9));
    CHECK(close(ecdf_sorted(s, 3.5), 0.5, 1e-12));
    CHECK(sturges_bins(12) == 5);
    CHECK(close(fd_bin_width_sorted(s), 1.90003751080145, 1e-9)); // 2·IQR/n^(1/3)
    CHECK(close(scott_bin_width(s), 2.17345242061961, 1e-9));     // (24√π/n)^(1/3)·σ
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 16);
    CHECK(close(median_abs_deviation(s, &alloc), 1.25, 1e-9)); // scipy.stats.median_abs_deviation
}
