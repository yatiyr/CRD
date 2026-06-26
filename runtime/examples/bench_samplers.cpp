// v12-f sampler throughput — Cerid ns/sample (vs NumPy + MATLAB, timed by the bench scripts). Single-thread.

#include <crd/hesap/stats/stats.hpp>

#include <chrono>
#include <cstdio>

namespace st = crd::hesap::stats;

namespace
{
template <class Fn>
void row(const char* name, Fn draw)
{
    const int n = 5'000'000;
    volatile double sink = 0.0;
    for (int i = 0; i < 100000; ++i)
    {
        sink += draw(); // warm
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i)
    {
        sink += draw();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
    std::printf("%-16s %7.3f ns/sample\n", name, ns);
}
} // namespace

int main()
{
    st::Pcg64Dxsm g(12345);
    std::printf("# Cerid sampler throughput (single-thread, ns/sample)\n");
    row("normal", [&] { return st::standard_normal(g); });
    row("exponential", [&] { return st::standard_exponential(g); });
    row("gamma(2.5)", [&] { return st::gamma_dist(g, 2.5, 1.0); });
    row("beta(2,5)", [&] { return st::beta_dist(g, 2.0, 5.0); });
    row("poisson(4)", [&] { return static_cast<double>(st::poisson(g, 4.0)); });
    row("poisson(30)", [&] { return static_cast<double>(st::poisson(g, 30.0)); });
    // Held sampler = NumPy's amortized model (r.binomial(n,p,size) computes the BTPE/BINV setup once for the array).
    const st::BinomialSampler bin20(20, 0.3);
    row("binomial(20,.3)", [&] { return static_cast<double>(bin20.sample(g)); });
    const st::BinomialSampler bin1000(1000, 0.5);
    row("binomial(1000,.5)", [&] { return static_cast<double>(bin1000.sample(g)); });
    return 0;
}
