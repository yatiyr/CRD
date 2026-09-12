// crd-hesap-stats v12-i — the 12 univariate DISCRETE distributions, gated vs scipy.stats (discrete_refs.inc):
// pmf/logpmf/cdf/sf <1e-9, integer ppf exact, moments + entropy (NaN ref ⇒ not gated) + rvs moments &
// {seed}-determinism + the DiscreteDistribution concept.

#include <crd/hesap/stats/stats.hpp>

#include "discrete_refs.inc"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace st = crd::hesap::stats;
using crd::i64;

namespace
{
[[nodiscard]] bool close(double got, double want, double tol) noexcept
{
    if (std::isnan(want))
    {
        return true;
    }
    if (std::isinf(want))
    {
        return std::isinf(got) && ((got > 0.0) == (want > 0.0));
    }
    return std::fabs(got - want) <= tol * std::fabs(want) + 1e-12;
}

template <class D, std::size_t NK, std::size_t NP>
void gate(const char* name, const D& d, const long long (&k)[NK], const double (&pmf)[NK], const double (&logpmf)[NK],
          const double (&cdf)[NK], const double (&sfv)[NK], const double (&p)[NP], const long long (&ppf)[NP],
          const double (&mvsk)[4], double entropy)
{
    INFO("distribution = " << name);
    for (std::size_t i = 0; i < NK; ++i)
    {
        INFO("k = " << k[i]);
        CHECK(close(d.pmf(k[i]), pmf[i], 1e-9));
        CHECK(close(d.logpmf(k[i]), logpmf[i], 1e-9));
        CHECK(close(d.cdf(k[i]), cdf[i], 1e-9));
        CHECK(close(d.sf(k[i]), sfv[i], 1e-9));
    }
    for (std::size_t i = 0; i < NP; ++i)
    {
        INFO("p = " << p[i]);
        CHECK(static_cast<long long>(d.ppf(p[i])) == ppf[i]);
    }
    CHECK(close(d.mean(), mvsk[0], 1e-9));
    CHECK(close(d.var(), mvsk[1], 1e-8));
    CHECK(close(d.skewness(), mvsk[2], 1e-7));
    CHECK(close(d.kurtosis(), mvsk[3], 1e-7));
    CHECK(close(d.entropy(), entropy, 1e-7));
}
} // namespace

#define GATE(NM, DIST)                                                                                                 \
    gate(#NM, DIST, ref_##NM##_k, ref_##NM##_pmf, ref_##NM##_logpmf, ref_##NM##_cdf, ref_##NM##_sf, ref_##NM##_p,      \
         ref_##NM##_ppf, ref_##NM##_mvsk, ref_##NM##_entropy)

TEST_CASE("discrete: all 12 distributions vs scipy.stats", "[v12-i][stats][dist]")
{
    GATE(bernoulli, st::Bernoulli<double>(0.3));
    GATE(binomial, st::Binomial<double>(20, 0.35));
    GATE(poisson, st::Poisson<double>(4.5));
    GATE(geometric, st::Geometric<double>(0.3));
    GATE(negbinom, st::NegativeBinomial<double>(5.0, 0.4));
    GATE(discreteuniform, st::DiscreteUniform<double>(2, 10));
    GATE(hypergeom, st::Hypergeometric<double>(30, 12, 10));
    GATE(skellam, st::Skellam<double>(4.0, 2.0));
    GATE(zipf, st::Zipf<double>(3.5));
    GATE(yulesimon, st::YuleSimon<double>(3.0));
    GATE(betabinom, st::BetaBinomial<double>(15, 2.0, 3.0));
    GATE(logarithmic, st::Logarithmic<double>(0.6));
}

TEST_CASE("discrete: ppf/cdf consistency (cdf(ppf(p)) >= p > cdf(ppf(p)-1))", "[v12-i][stats][dist]")
{
    auto consistent = [](auto d) {
        for (double p : {0.1, 0.3, 0.5, 0.7, 0.9})
        {
            const i64 k = d.ppf(p);
            CHECK(d.cdf(k) >= p - 1e-12);
            if (k > 0)
            {
                CHECK(d.cdf(k - 1) < p + 1e-12);
            }
        }
    };
    consistent(st::Poisson<double>(6.0));
    consistent(st::Binomial<double>(30, 0.4));
    consistent(st::NegativeBinomial<double>(4.0, 0.5));
}

TEST_CASE("discrete: rvs sample moments match + determinism", "[v12-i][stats][dist][moat]")
{
    constexpr int n = 200000;
    auto mean_var = [](auto dist, std::uint64_t seed) {
        st::Pcg64Dxsm g(seed);
        double s = 0;
        double s2 = 0;
        for (int i = 0; i < n; ++i)
        {
            const double v = static_cast<double>(dist.rvs(g));
            s += v;
            s2 += v * v;
        }
        const double m = s / n;
        return std::pair<double, double>{m, s2 / n - m * m};
    };
    {
        const st::Poisson<double> d(6.0);
        const auto [m, v] = mean_var(d, 3);
        CHECK(std::fabs(m - d.mean()) < 0.03);
        CHECK(std::fabs(v - d.var()) < 0.1);
    }
    {
        const st::NegativeBinomial<double> d(5.0, 0.4);
        const auto [m, v] = mean_var(d, 5);
        CHECK(std::fabs(m - d.mean()) < 0.05);
        CHECK(std::fabs(v - d.var()) < 0.5);
    }
    {
        const st::Geometric<double> d(0.25);
        const auto [m, v] = mean_var(d, 9);
        CHECK(std::fabs(m - d.mean()) < 0.05);
        CHECK(std::fabs(v - d.var()) < 0.6);
    }
    // determinism.
    st::Xoshiro256ss a(404);
    st::Xoshiro256ss b(404);
    const st::Poisson<double> pd(10.0);
    for (int i = 0; i < 3000; ++i)
    {
        REQUIRE(pd.rvs(a) == pd.rvs(b));
    }
}

TEST_CASE("discrete: DiscreteDistribution concept holds", "[v12-i][stats][dist]")
{
    static_assert(st::DiscreteDistribution<st::Poisson<double>>);
    static_assert(st::DiscreteDistribution<st::Binomial<double>>);
    static_assert(st::DiscreteDistribution<st::Skellam<double>>);
    static_assert(st::DiscreteDistribution<st::Zipf<float>>);
    SUCCEED();
}
