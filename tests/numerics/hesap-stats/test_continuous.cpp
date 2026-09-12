// crd-hesap-stats v12-h — the ~25 univariate CONTINUOUS distributions, gated vs scipy.stats (continuous_refs.inc):
// pdf/logpdf/cdf/sf <1e-9, ppf <1e-7, moments + entropy (NaN ref ⇒ not gated) + ppf∘cdf round-trip + rvs moments &
// {seed}-determinism + the Distribution<T> concept + f32 smoke.

#include <crd/hesap/stats/stats.hpp>

#include "continuous_refs.inc"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace st = crd::hesap::stats;

namespace
{
// Gate helper. NaN want ⇒ skip (undefined moment / not-closed-form). ±inf want ⇒ require matching ±inf.
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

template <class D, std::size_t NX, std::size_t NP>
void gate(const char* name, const D& d, const double (&x)[NX], const double (&pdf)[NX], const double (&logpdf)[NX],
          const double (&cdf)[NX], const double (&sfv)[NX], const double (&p)[NP], const double (&ppf)[NP],
          const double (&mvsk)[4], double entropy)
{
    INFO("distribution = " << name);
    for (std::size_t i = 0; i < NX; ++i)
    {
        INFO("x = " << x[i]);
        CHECK(close(d.pdf(x[i]), pdf[i], 1e-9));
        CHECK(close(d.logpdf(x[i]), logpdf[i], 1e-9));
        CHECK(close(d.cdf(x[i]), cdf[i], 1e-9));
        CHECK(close(d.sf(x[i]), sfv[i], 1e-9));
    }
    for (std::size_t i = 0; i < NP; ++i)
    {
        INFO("p = " << p[i]);
        CHECK(close(d.ppf(p[i]), ppf[i], 1e-7));
    }
    CHECK(close(d.mean(), mvsk[0], 1e-9));
    CHECK(close(d.var(), mvsk[1], 1e-8));
    CHECK(close(d.skewness(), mvsk[2], 1e-7));
    CHECK(close(d.kurtosis(), mvsk[3], 1e-7));
    CHECK(close(d.entropy(), entropy, 1e-8));
}
} // namespace

#define GATE(NM, DIST)                                                                                                 \
    gate(#NM, DIST, ref_##NM##_x, ref_##NM##_pdf, ref_##NM##_logpdf, ref_##NM##_cdf, ref_##NM##_sf, ref_##NM##_p,      \
         ref_##NM##_ppf, ref_##NM##_mvsk, ref_##NM##_entropy)

TEST_CASE("continuous: all 25 distributions vs scipy.stats", "[v12-h][stats][dist]")
{
    GATE(normal, st::Normal<double>(0.5, 2.0));
    GATE(lognormal, st::LogNormal<double>(0.2, 0.7));
    GATE(exponential, st::Exponential<double>(2.0));
    GATE(gamma, st::Gamma<double>(2.5, 1.5));
    GATE(beta, st::Beta<double>(2.0, 5.0));
    GATE(chisquared, st::ChiSquared<double>(4.0));
    GATE(studentt, st::StudentT<double>(5.0));
    GATE(fisherf, st::FisherF<double>(5.0, 10.0));
    GATE(cauchy, st::Cauchy<double>(0.0, 1.0));
    GATE(laplace, st::Laplace<double>(0.0, 1.5));
    GATE(logistic, st::Logistic<double>(0.0, 1.2));
    GATE(weibull, st::Weibull<double>(1.8, 2.0));
    GATE(gumbel, st::Gumbel<double>(0.5, 1.3));
    GATE(pareto, st::Pareto<double>(5.0, 1.5));
    GATE(rayleigh, st::Rayleigh<double>(1.4));
    GATE(maxwell, st::Maxwell<double>(1.2));
    GATE(uniform, st::Uniform<double>(-1.0, 3.0));
    GATE(halfnormal, st::HalfNormal<double>(1.5));
    GATE(halfcauchy, st::HalfCauchy<double>(1.3));
    GATE(triangular, st::Triangular<double>(0.0, 0.7, 2.0));
    GATE(invgamma, st::InverseGamma<double>(5.0, 2.0));
    GATE(nakagami, st::Nakagami<double>(1.5, 1.2));
    GATE(wald, st::Wald<double>(1.0, 2.0));
    GATE(vonmises, st::VonMises<double>(0.3, 2.0));
    GATE(rice, st::Rice<double>(1.0, 1.0));
}

TEST_CASE("continuous: ppf is the inverse cdf (round-trip)", "[v12-h][stats][dist]")
{
    const st::Normal<double> nrm(1.0, 2.0);
    const st::Gamma<double> gam(3.0, 2.0);
    const st::Beta<double> bet(2.0, 3.0);
    const st::StudentT<double> stt(7.0);
    const st::Weibull<double> wbl(1.5, 2.0);
    for (double p : {0.05, 0.2, 0.5, 0.8, 0.95})
    {
        CHECK(nrm.cdf(nrm.ppf(p)) == Catch::Approx(p).epsilon(1e-9));
        CHECK(gam.cdf(gam.ppf(p)) == Catch::Approx(p).epsilon(1e-9));
        CHECK(bet.cdf(bet.ppf(p)) == Catch::Approx(p).epsilon(1e-9));
        CHECK(stt.cdf(stt.ppf(p)) == Catch::Approx(p).epsilon(1e-8));
        CHECK(wbl.cdf(wbl.ppf(p)) == Catch::Approx(p).epsilon(1e-9));
    }
}

TEST_CASE("continuous: rvs sample moments match + determinism", "[v12-h][stats][dist][moat]")
{
    constexpr int n = 200000;
    auto sample_mean_var = [](auto dist, std::uint64_t seed) {
        st::Pcg64Dxsm g(seed);
        double s = 0;
        double s2 = 0;
        for (int i = 0; i < n; ++i)
        {
            const double v = dist.rvs(g);
            s += v;
            s2 += v * v;
        }
        const double m = s / n;
        return std::pair<double, double>{m, s2 / n - m * m};
    };
    {
        const auto [m, v] = sample_mean_var(st::Normal<double>(1.0, 2.0), 7);
        CHECK(std::fabs(m - 1.0) < 0.02);
        CHECK(std::fabs(v - 4.0) < 0.08);
    }
    {
        const auto [m, v] = sample_mean_var(st::Gamma<double>(2.5, 1.5), 11);
        CHECK(std::fabs(m - 3.75) < 0.03);          // a*scale
        CHECK(std::fabs(v - 5.625) < 0.2);          // a*scale^2
    }
    {
        const auto [m, v] = sample_mean_var(st::Weibull<double>(1.8, 2.0), 13);
        const st::Weibull<double> w(1.8, 2.0);
        CHECK(std::fabs(m - w.mean()) < 0.03);
        CHECK(std::fabs(v - w.var()) < 0.1);
    }
    // determinism: same seed ⇒ identical draws.
    st::Xoshiro256ss a(2025);
    st::Xoshiro256ss b(2025);
    const st::Gamma<double> gd(2.0, 1.0);
    for (int i = 0; i < 3000; ++i)
    {
        REQUIRE(gd.rvs(a) == gd.rvs(b));
    }
}

TEST_CASE("continuous: Distribution<T> concept holds", "[v12-h][stats][dist]")
{
    static_assert(st::ContinuousDistribution<st::Normal<double>>);
    static_assert(st::ContinuousDistribution<st::Gamma<double>>);
    static_assert(st::ContinuousDistribution<st::StudentT<double>>);
    static_assert(st::ContinuousDistribution<st::Beta<float>>);
    static_assert(st::ContinuousDistribution<st::VonMises<double>>);
    SUCCEED();
}

TEST_CASE("continuous: f32 path agrees with f64 to ~1e-5", "[v12-h][stats][dist]")
{
    const st::Normal<float> nf(0.5F, 2.0F);
    const st::Normal<double> nd(0.5, 2.0);
    const st::Gamma<float> gf(2.5F, 1.5F);
    const st::Gamma<double> gd(2.5, 1.5);
    for (double x : {0.0, 1.0, 2.5, 4.0})
    {
        CHECK(std::fabs(static_cast<double>(nf.cdf(static_cast<float>(x))) - nd.cdf(x)) < 1e-5);
        CHECK(std::fabs(static_cast<double>(gf.cdf(static_cast<float>(x))) - gd.cdf(x)) < 1e-5);
    }
}
