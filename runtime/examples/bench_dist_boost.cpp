// bench_dist_boost.cpp — v12-h/i/j univariate distributions: Cerid vs Boost.Math, pdf/cdf, ns/call (single-thread,
// in-cache). The gap the user caught — Boost was benched for the special functions but not the distribution layer.
// Honest: reports WINs AND losses, with a per-row spot-check (a parameter-convention mismatch prints <<MISMATCH so a
// phantom speed result on wrong values can't sneak through). Plain C arrays only; header-only ⇒ no lib link.

#include <crd/hesap/stats/stats.hpp>

#include <boost/math/distributions/bernoulli.hpp>
#include <boost/math/distributions/beta.hpp>
#include <boost/math/distributions/binomial.hpp>
#include <boost/math/distributions/cauchy.hpp>
#include <boost/math/distributions/chi_squared.hpp>
#include <boost/math/distributions/exponential.hpp>
#include <boost/math/distributions/extreme_value.hpp>
#include <boost/math/distributions/fisher_f.hpp>
#include <boost/math/distributions/gamma.hpp>
#include <boost/math/distributions/geometric.hpp>
#include <boost/math/distributions/lognormal.hpp>
#include <boost/math/distributions/negative_binomial.hpp>
#include <boost/math/distributions/normal.hpp>
#include <boost/math/distributions/pareto.hpp>
#include <boost/math/distributions/poisson.hpp>
#include <boost/math/distributions/students_t.hpp>
#include <boost/math/distributions/weibull.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

namespace st = crd::hesap::stats;
namespace bm = boost::math;

namespace
{
constexpr int kReps = 20000;

// runtime-filled (volatile seed) so the compiler can NOT constant-fold f(x[i]). SANITY #4 (the v12-d trap).
double g_pos[8];   // (0,inf)
double g_real[8];  // (-inf,inf)
double g_unit[8];  // (0,1)
double g_disc[8];  // integers ≥ 0
double g_disc1[8]; // integers ≥ 1 (geometric: Cerid is scipy-trials k≥1, Boost is failures k≥0 ⇒ Boost at k−1)

void fill(double* dst, const double* base)
{
    volatile double j = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        dst[i] = base[i] + static_cast<double>(j);
    }
}

template <class F>
double per_call(F f)
{
    constexpr int n = 8;
    f();
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
void row(const char* name, const double* xin, CF cf, BF bf)
{
    const double a = cf(xin[3]);
    const double b = bf(xin[3]);
    const double rel = std::fabs(a - b) / (std::fabs(b) + 1e-300);
    const char* flag = (rel < 1e-9) ? "" : "  <<MISMATCH";
    const double tc = per_call(
        [&]
        {
            double s = 0;
            for (int i = 0; i < 8; ++i) s += cf(xin[i]);
            return s;
        });
    const double tb = per_call(
        [&]
        {
            double s = 0;
            for (int i = 0; i < 8; ++i) s += bf(xin[i]);
            return s;
        });
    std::printf("%-18s  %8.2f  %8.2f   %5.2fx %s%s\n", name, tc, tb, tb / tc, (tb > tc ? "WIN " : "lose"), flag);
}
} // namespace

int main()
{
    const double pos[] = {0.3, 0.7, 1.2, 2.5, 4.0, 6.0, 9.0, 13.0};
    const double real[] = {-2.0, -1.0, -0.3, 0.5, 1.2, 2.5, 4.0, 6.0};
    const double unit[] = {0.05, 0.15, 0.3, 0.45, 0.6, 0.75, 0.85, 0.95};
    const double disc[] = {0.0, 1.0, 2.0, 3.0, 5.0, 8.0, 12.0, 18.0};
    const double disc1[] = {1.0, 2.0, 3.0, 4.0, 5.0, 8.0, 12.0, 18.0};
    fill(g_pos, pos);
    fill(g_real, real);
    fill(g_unit, unit);
    fill(g_disc, disc);
    fill(g_disc1, disc1);

    std::printf("# v12-h/i/j univariate distributions — Cerid vs Boost.Math, ns/call (single-thread, in-cache)\n");
    std::printf("function            Cerid(ns)  Boost(ns)   speedup\n");

    // ── continuous (pdf + cdf) ──
    const st::Normal<double> cn(0.0, 1.0);
    const bm::normal bn(0.0, 1.0);
    row("normal.pdf", g_real, [&](double x) { return cn.pdf(x); }, [&](double x) { return bm::pdf(bn, x); });
    row("normal.cdf", g_real, [&](double x) { return cn.cdf(x); }, [&](double x) { return bm::cdf(bn, x); });

    const st::LogNormal<double> cln(0.0, 1.0);
    const bm::lognormal bln(0.0, 1.0);
    row("lognormal.pdf", g_pos, [&](double x) { return cln.pdf(x); }, [&](double x) { return bm::pdf(bln, x); });
    row("lognormal.cdf", g_pos, [&](double x) { return cln.cdf(x); }, [&](double x) { return bm::cdf(bln, x); });

    const st::Gamma<double> cg(2.0, 1.5);
    const bm::gamma_distribution<double> bg(2.0, 1.5);
    row("gamma.pdf", g_pos, [&](double x) { return cg.pdf(x); }, [&](double x) { return bm::pdf(bg, x); });
    row("gamma.cdf", g_pos, [&](double x) { return cg.cdf(x); }, [&](double x) { return bm::cdf(bg, x); });

    const st::Beta<double> cb(2.0, 3.0);
    const bm::beta_distribution<double> bb(2.0, 3.0);
    row("beta.pdf", g_unit, [&](double x) { return cb.pdf(x); }, [&](double x) { return bm::pdf(bb, x); });
    row("beta.cdf", g_unit, [&](double x) { return cb.cdf(x); }, [&](double x) { return bm::cdf(bb, x); });

    const st::Exponential<double> ce(2.0); // Cerid scale=2 ⇒ Boost rate λ=1/scale
    const bm::exponential be(0.5);
    row("exponential.pdf", g_pos, [&](double x) { return ce.pdf(x); }, [&](double x) { return bm::pdf(be, x); });
    row("exponential.cdf", g_pos, [&](double x) { return ce.cdf(x); }, [&](double x) { return bm::cdf(be, x); });

    const st::ChiSquared<double> cc(4.0);
    const bm::chi_squared bc(4.0);
    row("chi2.pdf", g_pos, [&](double x) { return cc.pdf(x); }, [&](double x) { return bm::pdf(bc, x); });
    row("chi2.cdf", g_pos, [&](double x) { return cc.cdf(x); }, [&](double x) { return bm::cdf(bc, x); });

    const st::StudentT<double> cs(5.0);
    const bm::students_t bs(5.0);
    row("studentt.pdf", g_real, [&](double x) { return cs.pdf(x); }, [&](double x) { return bm::pdf(bs, x); });
    row("studentt.cdf", g_real, [&](double x) { return cs.cdf(x); }, [&](double x) { return bm::cdf(bs, x); });

    const st::FisherF<double> cf(5.0, 10.0);
    const bm::fisher_f bf(5.0, 10.0);
    row("fisherf.pdf", g_pos, [&](double x) { return cf.pdf(x); }, [&](double x) { return bm::pdf(bf, x); });
    row("fisherf.cdf", g_pos, [&](double x) { return cf.cdf(x); }, [&](double x) { return bm::cdf(bf, x); });

    const st::Cauchy<double> cca(0.0, 1.0);
    const bm::cauchy bca(0.0, 1.0);
    row("cauchy.pdf", g_real, [&](double x) { return cca.pdf(x); }, [&](double x) { return bm::pdf(bca, x); });
    row("cauchy.cdf", g_real, [&](double x) { return cca.cdf(x); }, [&](double x) { return bm::cdf(bca, x); });

    const st::Weibull<double> cw(1.5, 2.0);
    const bm::weibull bw(1.5, 2.0);
    row("weibull.pdf", g_pos, [&](double x) { return cw.pdf(x); }, [&](double x) { return bm::pdf(bw, x); });
    row("weibull.cdf", g_pos, [&](double x) { return cw.cdf(x); }, [&](double x) { return bm::cdf(bw, x); });

    const st::Gumbel<double> cgu(0.0, 1.0);
    const bm::extreme_value bgu(0.0, 1.0);
    row("gumbel.pdf", g_real, [&](double x) { return cgu.pdf(x); }, [&](double x) { return bm::pdf(bgu, x); });
    row("gumbel.cdf", g_real, [&](double x) { return cgu.cdf(x); }, [&](double x) { return bm::cdf(bgu, x); });

    const st::Pareto<double> cpa(2.5, 1.0); // Cerid(shape, scale=xm) ⇒ Boost pareto(scale=xm, shape)
    const bm::pareto bpa(1.0, 2.5);
    row("pareto.pdf", g_pos, [&](double x) { return cpa.pdf(x); }, [&](double x) { return bm::pdf(bpa, x); });
    row("pareto.cdf", g_pos, [&](double x) { return cpa.cdf(x); }, [&](double x) { return bm::cdf(bpa, x); });

    // ── discrete (pmf + cdf) ──
    const st::Binomial<double> cbi(20, 0.4);
    const bm::binomial bbi(20, 0.4);
    row("binomial.pmf", g_disc, [&](double x) { return cbi.pmf(static_cast<crd::i64>(x)); },
        [&](double x) { return bm::pdf(bbi, x); });
    row("binomial.cdf", g_disc, [&](double x) { return cbi.cdf(static_cast<crd::i64>(x)); },
        [&](double x) { return bm::cdf(bbi, x); });

    const st::Poisson<double> cpo(5.0);
    const bm::poisson bpo(5.0);
    row("poisson.pmf", g_disc, [&](double x) { return cpo.pmf(static_cast<crd::i64>(x)); },
        [&](double x) { return bm::pdf(bpo, x); });
    row("poisson.cdf", g_disc, [&](double x) { return cpo.cdf(static_cast<crd::i64>(x)); },
        [&](double x) { return bm::cdf(bpo, x); });

    const st::Geometric<double> cge(0.3);
    const bm::geometric bge(0.3); // Boost = #failures (k≥0); Cerid = scipy #trials (k≥1) ⇒ Boost at k−1 (same value)
    row("geometric.pmf", g_disc1, [&](double x) { return cge.pmf(static_cast<crd::i64>(x)); },
        [&](double x) { return bm::pdf(bge, x - 1.0); });
    row("geometric.cdf", g_disc1, [&](double x) { return cge.cdf(static_cast<crd::i64>(x)); },
        [&](double x) { return bm::cdf(bge, x - 1.0); });

    const st::NegativeBinomial<double> cnb(5.0, 0.4);
    const bm::negative_binomial bnb(5.0, 0.4);
    row("negbinom.pmf", g_disc, [&](double x) { return cnb.pmf(static_cast<crd::i64>(x)); },
        [&](double x) { return bm::pdf(bnb, x); });
    row("negbinom.cdf", g_disc, [&](double x) { return cnb.cdf(static_cast<crd::i64>(x)); },
        [&](double x) { return bm::cdf(bnb, x); });

    return 0;
}
