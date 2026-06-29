// v12-r regression crush bench — per-fit throughput (fits/sec) for OLS / Ridge / Lasso / GLM-logistic / PCA on a
// 200x8 design, vs sklearn + statsmodels (see bench_regression_refs.py). Cerid is native C++ over the shipped dense
// factorizations; the Python peers pay per-call interpreter + numpy-dispatch overhead.

#include <crd/hesap/stats/regression.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

using namespace crd::hesap::stats;
using crd::containers::ConstSpan;
using crd::f64;
using crd::u32;
using crd::u64;
using crd::usize;
using clk = std::chrono::steady_clock;

int main()
{
    crd::memory::TlsfAllocator alloc(static_cast<usize>(1) << 27);
    constexpr usize n = 200;
    constexpr usize p = 8;
    crd::containers::Array<f64> x(&alloc);
    crd::containers::Array<f64> y(&alloc);
    crd::containers::Array<f64> yb(&alloc);
    x.resize(n * p);
    y.resize(n);
    yb.resize(n);
    u64 s = 12345;
    auto rnd = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<f64>(s >> 11) / static_cast<f64>(static_cast<u64>(1) << 53);
    };
    for (usize i = 0; i < n; ++i)
    {
        f64 yi = 0.5;
        for (usize j = 0; j < p; ++j)
        {
            const f64 v = rnd() * 2.0 - 1.0;
            x[i * p + j] = v;
            yi += 0.3 * v;
        }
        y[i] = yi + 0.1 * (rnd() * 2.0 - 1.0);
        yb[i] = yi > 0.5 ? 1.0 : 0.0;
    }
    const auto xs = ConstSpan<f64>{x.data(), n * p};
    const auto ys = ConstSpan<f64>{y.data(), n};
    const auto ybs = ConstSpan<f64>{yb.data(), n};

    auto bench = [&](const char* name, int reps, auto fn) {
        fn(); // warm
        const auto t0 = clk::now();
        for (int r = 0; r < reps; ++r)
        {
            const volatile f64 v = fn();
            (void)v;
        }
        const auto t1 = clk::now();
        const f64 us = std::chrono::duration<f64, std::micro>(t1 - t0).count() / reps;
        std::printf("%-12s %9.2f us/fit   %12.0f fits/s\n", name, us, 1e6 / us);
    };

    bench("OLS", 20000, [&] { return ols(xs, ys, n, p, &alloc).coef[0]; });
    bench("Ridge", 20000, [&] { return ridge(xs, ys, n, p, 1.0, &alloc)[0]; });
    bench("Lasso", 4000, [&] { return lasso(xs, ys, n, p, 0.01, &alloc)[0]; });
    bench("GLM-logit", 4000, [&] { return glm(xs, ybs, n, p, GlmFamily::Logistic, &alloc)[0]; });
    bench("PCA", 20000, [&] { return pca(xs, n, p, &alloc).explained_variance[0]; });
    return 0;
}
