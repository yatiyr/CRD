// bench_dist_boost.cpp — v12-h/i/j univariate distributions: Cerid vs Boost.Math (its peer library,
// boost_peer_rows.hpp, values compared on every row), pdf/cdf, ns/element (single-thread, in-cache). The gap the user
// caught — Boost was benched for the special functions but not the distribution layer. Honest: reports WINs AND
// losses; a parameter-convention mismatch prints !MISMATCH! and fails the run, so a phantom speed result on wrong
// values can't sneak through. Plain C arrays only.
//
// Build and run at every CPU level: bash scripts/run_bench_boost.sh. The argument is the passes per round (default
// 256) for a smoke run.

#include "boost_peer_rows.hpp"

#include <crd/hesap/stats/stats.hpp>

#include <cstdio>
#include <cstdlib>

namespace st = crd::hesap::stats;
using crd_peer::boost_math::Density;
using crd_peer::boost_math::Distribution;
using crd_peer::boost_math::Parameters;

namespace
{
constexpr int kBaseN = 8;
constexpr int kN = kBaseN * 256;
// The hesap-stats reference gates (tests/numerics/hesap-stats/test_continuous.cpp, test_discrete.cpp).
constexpr boost_rows::Bound kGate{1e-9, 1e-12};

double g_pos[kN];   // (0,inf)
double g_real[kN];  // (-inf,inf)
double g_unit[kN];  // (0,1)
double g_disc[kN];  // integers ≥ 0
double g_disc1[kN]; // integers ≥ 1 (geometric: Cerid is scipy-trials k≥1, Boost is failures k≥0 ⇒ Boost at k−1)
double g_fail[kN];  // g_disc1 − 1, Boost's failures
double g_cerid[kN];
double g_boost[kN];
int g_passes = 256;

void fill(double* dst, const double (&base)[kBaseN])
{
    for (int i = 0; i < kN; ++i)
    {
        dst[i] = base[i % kBaseN];
    }
}

// The pdf (or pmf) and cdf rows of one distribution: Cerid's d over x against Boost's over peer_x.
template <class D>
bool rows(const char* pdf_name, const char* cdf_name, const D& d, const double* x, Distribution peer, Parameters p,
          const double* peer_x)
{
    bool ok = true;
    for (const Density density : {Density::Pdf, Density::Cdf})
    {
        const auto cerid = [&](double* out)
        {
            if (density == Density::Pdf)
            {
                for (int i = 0; i < kN; ++i)
                {
                    if constexpr (st::DiscreteDistribution<D>)
                    {
                        out[i] = d.pmf(static_cast<crd::i64>(x[i]));
                    }
                    else
                    {
                        out[i] = d.pdf(x[i]);
                    }
                }
            }
            else
            {
                for (int i = 0; i < kN; ++i)
                {
                    if constexpr (st::DiscreteDistribution<D>)
                    {
                        out[i] = d.cdf(static_cast<crd::i64>(x[i]));
                    }
                    else
                    {
                        out[i] = d.cdf(x[i]);
                    }
                }
            }
        };
        const auto boost = [&](double* out)
        {
            crd_peer::boost_math::eval(peer, p, density, peer_x, out, kN);
        };
        ok &= boost_rows::compare(density == Density::Pdf ? pdf_name : cdf_name, cerid, boost, kN, g_cerid, g_boost,
                                  kGate, 5, g_passes);
    }
    return ok;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        g_passes = static_cast<int>(std::strtol(argv[1], nullptr, 10));
        if (g_passes < 1)
        {
            std::fprintf(stderr, "usage: bench_dist_boost [passes per round >= 1]\n");
            return 2;
        }
    }
    fill(g_pos, {0.3, 0.7, 1.2, 2.5, 4.0, 6.0, 9.0, 13.0});
    fill(g_real, {-2.0, -1.0, -0.3, 0.5, 1.2, 2.5, 4.0, 6.0});
    fill(g_unit, {0.05, 0.15, 0.3, 0.45, 0.6, 0.75, 0.85, 0.95});
    fill(g_disc, {0.0, 1.0, 2.0, 3.0, 5.0, 8.0, 12.0, 18.0});
    fill(g_disc1, {1.0, 2.0, 3.0, 4.0, 5.0, 8.0, 12.0, 18.0});
    for (int i = 0; i < kN; ++i)
    {
        g_fail[i] = g_disc1[i] - 1.0;
    }

    boost_rows::header("v12-h/i/j univariate distributions, Cerid vs Boost.Math, ns/element (single-thread, in-cache)");
    bool ok = true;
    // ── continuous (pdf + cdf) ──
    ok &= rows("normal.pdf", "normal.cdf", st::Normal<double>(0.0, 1.0), g_real, Distribution::Normal, {0.0, 1.0},
               g_real);
    ok &= rows("lognormal.pdf", "lognormal.cdf", st::LogNormal<double>(0.0, 1.0), g_pos, Distribution::LogNormal,
               {0.0, 1.0}, g_pos);
    ok &= rows("gamma.pdf", "gamma.cdf", st::Gamma<double>(2.0, 1.5), g_pos, Distribution::Gamma, {2.0, 1.5}, g_pos);
    ok &= rows("beta.pdf", "beta.cdf", st::Beta<double>(2.0, 3.0), g_unit, Distribution::Beta, {2.0, 3.0}, g_unit);
    // Cerid scale = 2 ⇒ Boost rate λ = 1/scale.
    ok &= rows("exponential.pdf", "exponential.cdf", st::Exponential<double>(2.0), g_pos, Distribution::Exponential,
               {0.5, 0.0}, g_pos);
    ok &= rows("chi2.pdf", "chi2.cdf", st::ChiSquared<double>(4.0), g_pos, Distribution::ChiSquared, {4.0, 0.0}, g_pos);
    ok &= rows("studentt.pdf", "studentt.cdf", st::StudentT<double>(5.0), g_real, Distribution::StudentsT, {5.0, 0.0},
               g_real);
    ok &= rows("fisherf.pdf", "fisherf.cdf", st::FisherF<double>(5.0, 10.0), g_pos, Distribution::FisherF, {5.0, 10.0},
               g_pos);
    ok &= rows("cauchy.pdf", "cauchy.cdf", st::Cauchy<double>(0.0, 1.0), g_real, Distribution::Cauchy, {0.0, 1.0},
               g_real);
    ok &= rows("weibull.pdf", "weibull.cdf", st::Weibull<double>(1.5, 2.0), g_pos, Distribution::Weibull, {1.5, 2.0},
               g_pos);
    ok &= rows("gumbel.pdf", "gumbel.cdf", st::Gumbel<double>(0.0, 1.0), g_real, Distribution::ExtremeValue, {0.0, 1.0},
               g_real);
    // Cerid (shape, scale = xm) ⇒ Boost pareto(scale = xm, shape).
    ok &=
        rows("pareto.pdf", "pareto.cdf", st::Pareto<double>(2.5, 1.0), g_pos, Distribution::Pareto, {1.0, 2.5}, g_pos);
    // ── discrete (pmf + cdf) ──
    ok &= rows("binomial.pmf", "binomial.cdf", st::Binomial<double>(20, 0.4), g_disc, Distribution::Binomial,
               {20.0, 0.4}, g_disc);
    ok &=
        rows("poisson.pmf", "poisson.cdf", st::Poisson<double>(5.0), g_disc, Distribution::Poisson, {5.0, 0.0}, g_disc);
    ok &= rows("geometric.pmf", "geometric.cdf", st::Geometric<double>(0.3), g_disc1, Distribution::Geometric,
               {0.3, 0.0}, g_fail);
    ok &= rows("negbinom.pmf", "negbinom.cdf", st::NegativeBinomial<double>(5.0, 0.4), g_disc,
               Distribution::NegativeBinomial, {5.0, 0.4}, g_disc);
    return ok ? 0 : 1;
}
