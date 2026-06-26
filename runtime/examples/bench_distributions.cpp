// v12-h/i distribution throughput — Cerid ns/element for pdf/cdf/ppf over a 1M array, single-thread. vs scipy.stats
// (vectorized) + MATLAB pdf/cdf/icdf (bench_dist_matlab.m). Each distribution reuses the crushing hesap-special CDFs.

#include <crd/hesap/stats/stats.hpp>

#include <chrono>
#include <cstdio>

namespace st = crd::hesap::stats;

namespace
{
constexpr int kN = 1'000'000;
double g_x[kN];  // eval points
double g_p[kN];  // probabilities for ppf
double g_sink = 0.0;

template <class Fn>
double row(const char* name, Fn f)
{
    for (int i = 0; i < 50000; ++i)
    {
        g_sink += f(i % kN);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i)
    {
        g_sink += f(i);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kN;
    std::printf("%-22s %7.3f ns/elem\n", name, ns);
    return ns;
}
} // namespace

int main()
{
    for (int i = 0; i < kN; ++i)
    {
        g_x[i] = -5.0 + 12.0 * static_cast<double>(i) / kN; // [-5, 7)
        g_p[i] = 1e-4 + (1.0 - 2e-4) * static_cast<double>(i) / kN;
    }
    std::printf("# Cerid distribution throughput (single-thread, ns/elem)\n");

    const st::Normal<double> nrm(0.0, 1.0);
    row("normal.pdf", [&](int i) { return nrm.pdf(g_x[i]); });
    row("normal.cdf", [&](int i) { return nrm.cdf(g_x[i]); });
    row("normal.ppf", [&](int i) { return nrm.ppf(g_p[i]); });

    const st::Gamma<double> gam(2.5, 1.5);
    row("gamma.pdf", [&](int i) { return gam.pdf(0.01 + g_x[i] + 5.0); });
    row("gamma.cdf", [&](int i) { return gam.cdf(0.01 + g_x[i] + 5.0); });
    row("gamma.ppf", [&](int i) { return gam.ppf(g_p[i]); });

    const st::Beta<double> bet(2.0, 5.0);
    row("beta.pdf", [&](int i) { return bet.pdf(g_p[i]); });
    row("beta.cdf", [&](int i) { return bet.cdf(g_p[i]); });
    row("beta.ppf", [&](int i) { return bet.ppf(g_p[i]); });

    const st::StudentT<double> stt(7.0);
    row("studentt.pdf", [&](int i) { return stt.pdf(g_x[i]); });
    row("studentt.cdf", [&](int i) { return stt.cdf(g_x[i]); });
    row("studentt.ppf", [&](int i) { return stt.ppf(g_p[i]); });

    const st::Poisson<double> po(8.0);
    row("poisson.pmf", [&](int i) { return po.pmf(i % 25); });
    row("poisson.cdf", [&](int i) { return po.cdf(i % 25); });

    const st::Binomial<double> bi(40, 0.3);
    row("binomial.pmf", [&](int i) { return bi.pmf(i % 41); });
    row("binomial.cdf", [&](int i) { return bi.cdf(i % 41); });

    if (g_sink == 12345.6789)
    {
        std::printf("");
    }
    return 0;
}
