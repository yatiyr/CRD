// v12-q MCMC crush bench — sampling throughput (samples/sec) and effective-samples/sec for MH / HMC / NUTS on a 10-D
// standard Gaussian, vs PyMC NUTS (see bench_mcmc_refs.py). Cerid is native C++ with no per-draw interpreter overhead.

#include <crd/hesap/stats/mcmc.hpp>
#include <crd/hesap/stats/mcmc_diagnostics.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

using namespace crd::hesap::stats;
using crd::containers::ConstSpan;
using crd::f64;
using crd::usize;
using clk = std::chrono::steady_clock;

int main()
{
    crd::memory::TlsfAllocator alloc(static_cast<usize>(1) << 27);
    constexpr usize d = 10;
    const auto logp = [](ConstSpan<f64> x) {
        f64 s = 0;
        for (f64 v : x)
        {
            s += v * v;
        }
        return -0.5 * s;
    };
    const auto grad = [](ConstSpan<f64> x, crd::containers::Span<f64> g) {
        for (usize i = 0; i < x.size(); ++i)
        {
            g[i] = -x[i];
        }
    };
    crd::containers::Array<f64> x0(&alloc);
    x0.resize(d);
    for (usize i = 0; i < d; ++i)
    {
        x0[i] = 0.0;
    }
    const auto xs = ConstSpan<f64>{x0.data(), d};
    const usize N = 4000;

    auto run = [&](const char* name, auto sample) {
        const auto t0 = clk::now();
        const auto ch = sample();
        const auto t1 = clk::now();
        const f64 sec = std::chrono::duration<f64>(t1 - t0).count();
        // ess of coordinate 0 (extract the column)
        crd::containers::Array<f64> col(&alloc);
        col.resize(N);
        for (usize s = 0; s < N; ++s)
        {
            col[s] = ch[s * d];
        }
        const f64 ess = ess_bulk(ConstSpan<f64>{col.data(), N}, 1, N, &alloc);
        std::printf("%-8s %9.0f samples/s   %9.0f ess/s   (%.0f ESS / %zu draws)\n", name, N / sec, ess / sec, ess, N);
    };

    run("MH", [&] { return metropolis(logp, xs, N, 0.6, 1, &alloc); });
    run("HMC", [&] { return hmc(logp, grad, xs, N, 0.15, 20, 1, &alloc); });
    run("NUTS", [&] { return nuts(logp, grad, xs, N, 0.4, 1, &alloc); });
    return 0;
}
