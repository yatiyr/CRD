// v12-l crush bench — per-leapfrog gradient time: one full ∇_θ Σ_i logpdf(x_i | θ) over a FIXED 1M-point dataset,
// the quantity an HMC/NUTS leapfrog step computes. The exponential family precomputes its data-only sufficient
// statistics ONCE (suffstats) ⇒ each gradient is O(1); the rest recompute O(N). Reported as ns per gradient CALL
// (= per leapfrog step), the apples-to-apples metric vs bench_logpdf_grad.py (which reports the same). The params
// are perturbed each call so the compiler can't hoist the result. std:: timing/IO is fine in runtime/examples.

#include <crd/hesap/stats/log_density_grad.hpp>

#include <chrono>
#include <cstdio>

using namespace crd::hesap::stats;

namespace
{
constexpr int kN = 1 << 20; // ~1.05M points
double g_xs[kN];

[[nodiscard]] double ns_per(std::chrono::steady_clock::time_point t0, std::chrono::steady_clock::time_point t1,
                            double calls)
{
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / calls;
}
} // namespace

int main()
{
    for (int i = 0; i < kN; ++i)
    {
        g_xs[i] = 0.5 + static_cast<double>(i % 997) * 0.001;
    }
    std::printf("Cerid per-leapfrog gradient (i9-14900K, 1-thread, %d-point dataset), ns/call:\n", kN);

    // Normal — exponential family ⇒ O(1) per leapfrog from (Σx, Σx²).
    {
        const NormalStats<double> ss = suffstats(Normal<double>{}, g_xs, static_cast<crd::usize>(kN));
        constexpr int kM = 20'000'000;
        volatile double sink = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < kM; ++r)
        {
            const Normal<double> d(1.0 + static_cast<double>(r) * 1e-10, 2.0);
            double g[2];
            loglik_grad(d, ss, crd::containers::Span<double>{g, 2});
            sink += g[0];
        }
        std::printf("Normal    %12.2f ns/call (O(1) suff-stats)\n", ns_per(t0, std::chrono::steady_clock::now(), kM));
        (void)sink;
    }
    // Gamma — exponential family ⇒ O(1) from (Σlog x, Σx); one digamma per call.
    {
        const GammaStats<double> ss = suffstats(Gamma<double>(2.5, 1.3), g_xs, static_cast<crd::usize>(kN));
        constexpr int kM = 20'000'000;
        volatile double sink = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < kM; ++r)
        {
            const Gamma<double> d(2.5 + static_cast<double>(r) * 1e-10, 1.3);
            double g[2];
            loglik_grad(d, ss, crd::containers::Span<double>{g, 2});
            sink += g[0];
        }
        std::printf("Gamma     %12.2f ns/call (O(1) suff-stats)\n", ns_per(t0, std::chrono::steady_clock::now(), kM));
        (void)sink;
    }
    // StudentT — not exponential family (the ν-dependent log1p) ⇒ O(N) reduction per leapfrog.
    {
        constexpr int kM = 300;
        volatile double sink = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < kM; ++r)
        {
            const StudentT<double> d(5.0 + static_cast<double>(r) * 1e-6);
            double g[1];
            loglik_grad(d, g_xs, static_cast<crd::usize>(kN), crd::containers::Span<double>{g, 1});
            sink += g[0];
        }
        std::printf("StudentT  %12.2f ns/call (O(N))\n", ns_per(t0, std::chrono::steady_clock::now(), kM));
        (void)sink;
    }
    return 0;
}
