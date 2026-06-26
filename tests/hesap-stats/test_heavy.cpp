// crd-hesap-stats v12-j — heavy-tail/extreme/noncentral distributions, gated vs scipy.stats (heavy_refs.inc):
// pdf/logpdf/cdf/sf <1e-9, ppf <1e-6, moments+entropy (NaN ref ⇒ not gated) + rvs moments & determinism + concept.

#include <crd/hesap/stats/stats.hpp>

#include "heavy_refs.inc"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace st = crd::hesap::stats;

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

template <class D, std::size_t NX, std::size_t NP>
void gate(const char* name, const D& d, const double (&x)[NX], const double (&pdf)[NX], const double (&lp)[NX],
          const double (&cdf)[NX], const double (&sfv)[NX], const double (&p)[NP], const double (&ppf)[NP],
          const double (&mvsk)[4], double ent)
{
    INFO("distribution = " << name);
    for (std::size_t i = 0; i < NX; ++i)
    {
        INFO("x = " << x[i]);
        CHECK(close(d.pdf(x[i]), pdf[i], 1e-9));
        CHECK(close(d.logpdf(x[i]), lp[i], 1e-9));
        CHECK(close(d.cdf(x[i]), cdf[i], 1e-9));
        CHECK(close(d.sf(x[i]), sfv[i], 1e-9));
    }
    for (std::size_t i = 0; i < NP; ++i)
    {
        INFO("p = " << p[i]);
        CHECK(close(d.ppf(p[i]), ppf[i], 1e-6));
    }
    CHECK(close(d.mean(), mvsk[0], 1e-9));
    CHECK(close(d.var(), mvsk[1], 1e-8));
    CHECK(close(d.skewness(), mvsk[2], 1e-7));
    CHECK(close(d.kurtosis(), mvsk[3], 1e-7));
    CHECK(close(d.entropy(), ent, 1e-7));
}
} // namespace

#define GATE(NM, DIST)                                                                                                 \
    gate(#NM, DIST, ref_##NM##_x, ref_##NM##_pdf, ref_##NM##_logpdf, ref_##NM##_cdf, ref_##NM##_sf, ref_##NM##_p,      \
         ref_##NM##_ppf, ref_##NM##_mvsk, ref_##NM##_entropy)

TEST_CASE("heavy-tail: GEV/GPD/Levy/BetaPrime/ncx2/skewnorm/nct/ncf vs scipy.stats", "[v12-j][stats][dist]")
{
    GATE(gev, st::GEV<double>(0.2, 1.0, 2.0));
    GATE(gpd, st::GPD<double>(0.3, 0.0, 1.5));
    GATE(levy, st::Levy<double>(0.0, 1.5));
    GATE(betaprime, st::BetaPrime<double>(2.5, 5.0));
    GATE(ncx2, st::NoncentralChiSquared<double>(4.0, 3.0));
    GATE(skewnorm, st::SkewNormal<double>(4.0, 1.0, 2.0));
    GATE(nct, st::NoncentralT<double>(6.0, 2.0));
    GATE(ncf, st::NoncentralF<double>(5.0, 10.0, 3.0));
}

TEST_CASE("heavy-tail: ppf round-trips + rvs moments + determinism", "[v12-j][stats][dist][moat]")
{
    for (double p : {0.05, 0.3, 0.5, 0.7, 0.95})
    {
        const st::GPD<double> g(0.25, 0.0, 2.0);
        CHECK(g.cdf(g.ppf(p)) == Catch::Approx(p).epsilon(1e-9));
        const st::GEV<double> e(0.15, 1.0, 2.0);
        CHECK(e.cdf(e.ppf(p)) == Catch::Approx(p).epsilon(1e-9));
    }
    constexpr int n = 200000;
    auto mean_of = [](auto dist, std::uint64_t seed) {
        st::Pcg64Dxsm gg(seed);
        double s = 0;
        for (int i = 0; i < n; ++i)
        {
            s += dist.rvs(gg);
        }
        return s / n;
    };
    const st::NoncentralChiSquared<double> nc(4.0, 3.0);
    CHECK(std::fabs(mean_of(nc, 3) - nc.mean()) < 0.05); // k+λ = 7
    const st::BetaPrime<double> bp(3.0, 5.0);
    CHECK(std::fabs(mean_of(bp, 7) - bp.mean()) < 0.05); // a/(b-1)
    // determinism.
    st::Xoshiro256ss a(99);
    st::Xoshiro256ss b(99);
    const st::GEV<double> gev(0.2, 0.0, 1.0);
    for (int i = 0; i < 3000; ++i)
    {
        REQUIRE(gev.rvs(a) == gev.rvs(b));
    }
}

TEST_CASE("heavy-tail: concept holds", "[v12-j][stats][dist]")
{
    static_assert(st::ContinuousDistribution<st::GEV<double>>);
    static_assert(st::ContinuousDistribution<st::GPD<double>>);
    static_assert(st::ContinuousDistribution<st::Levy<float>>);
    static_assert(st::ContinuousDistribution<st::BetaPrime<double>>);
    static_assert(st::ContinuousDistribution<st::NoncentralChiSquared<double>>);
    static_assert(st::ContinuousDistribution<st::SkewNormal<double>>);
    static_assert(st::ContinuousDistribution<st::NoncentralT<double>>);
    static_assert(st::ContinuousDistribution<st::NoncentralF<float>>);
    SUCCEED();
}

TEST_CASE("heavy-tail: alpha-stable CMS sampler - special cases + determinism", "[v12-j][stats][dist][moat]")
{
    constexpr int n = 300000;
    // α=2 ⇒ N(loc, √2·scale): mean=loc, var=2·scale².
    {
        st::Pcg64Dxsm g(1);
        st::StableSampler<double> s(2.0, 0.0, 1.0, 1.0);
        double sm = 0;
        double s2 = 0;
        for (int i = 0; i < n; ++i)
        {
            const double v = s.sample(g);
            sm += v;
            s2 += v * v;
        }
        const double m = sm / n;
        CHECK(std::fabs(m - 1.0) < 0.02);
        CHECK(std::fabs((s2 / n - m * m) - 2.0) < 0.05); // Var = 2·scale²
    }
    // α=1, β=0 ⇒ standard Cauchy: median 0, IQR via quantiles ≈ 2 (Q3−Q1 = 2·scale).
    {
        st::Pcg64Dxsm g(7);
        int below0 = 0;
        int in_iqr = 0;
        for (int i = 0; i < n; ++i)
        {
            const double v = st::StableSampler<double>(1.0, 0.0, 0.0, 1.0).sample(g);
            if (v < 0.0)
            {
                ++below0;
            }
            if (v > -1.0 && v < 1.0)
            {
                ++in_iqr; // Cauchy: P(|X|<1) = 0.5
            }
        }
        CHECK(std::fabs(static_cast<double>(below0) / n - 0.5) < 0.01);
        CHECK(std::fabs(static_cast<double>(in_iqr) / n - 0.5) < 0.01);
    }
    // determinism.
    st::Xoshiro256ss a(2024);
    st::Xoshiro256ss b(2024);
    const st::StableSampler<double> stbl(1.5, 0.5, 0.0, 1.0);
    for (int i = 0; i < 5000; ++i)
    {
        REQUIRE(stbl.sample(a) == stbl.sample(b));
    }
}
