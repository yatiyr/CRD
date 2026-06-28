// v12-p KDE/robust/covariance crush bench — ns/call vs scipy.stats.gaussian_kde / scipy.theilslopes / statsmodels RLM /
// sklearn ledoit_wolf+oas. Native C++ vs interpreted per-call overhead (the metric for repeated estimator calls).

#include <crd/hesap/stats/cov_robust.hpp>
#include <crd/hesap/stats/kde.hpp>
#include <crd/hesap/stats/robust.hpp>

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
    crd::memory::TlsfAllocator alloc(static_cast<usize>(1) << 24);
    const usize n = 100;
    crd::containers::Array<f64> a(&alloc);
    crd::containers::Array<f64> xx(&alloc);
    crd::containers::Array<f64> yy(&alloc);
    crd::containers::Array<f64> mat(&alloc);
    a.resize(n);
    xx.resize(50);
    yy.resize(50);
    mat.resize(50 * 5);
    for (usize i = 0; i < n; ++i)
    {
        a[i] = 2.0 + 0.7 * crd::math::sin(0.30 * static_cast<f64>(i));
    }
    for (usize i = 0; i < 50; ++i)
    {
        xx[i] = static_cast<f64>(i);
        yy[i] = 2.0 * static_cast<f64>(i) + crd::math::sin(static_cast<f64>(i));
    }
    for (usize i = 0; i < 50 * 5; ++i)
    {
        mat[i] = crd::math::sin(0.7 * static_cast<f64>(i) + 1.0) + 0.1 * static_cast<f64>(i % 5);
    }

    const int N = 50000;
    f64 acc = 0;
    auto bench = [&](const char* name, auto fn) {
        for (int i = 0; i < 200; ++i)
        {
            acc += fn(i);
        }
        const auto t0 = clk::now();
        for (int i = 0; i < N; ++i)
        {
            acc += fn(i);
        }
        const auto t1 = clk::now();
        std::printf("%-14s %9.1f ns/call\n", name, std::chrono::duration<double, std::nano>(t1 - t0).count() / N);
    };

    const f64 h = kde_bandwidth_scott(ConstSpan<f64>{a.data(), n});
    bench("kde_eval", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        return kde_eval(ConstSpan<f64>{a.data(), n}, 3.0, h, KdeKernel::Gaussian);
    });
    bench("theil_sen", [&](int i) {
        yy[0] = static_cast<f64>(i & 31) * 0.1;
        return theil_sen(ConstSpan<f64>{xx.data(), 50}, ConstSpan<f64>{yy.data(), 50}, &alloc).slope;
    });
    bench("huber_loc", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        return huber_location(ConstSpan<f64>{a.data(), n}, &alloc);
    });
    bench("ledoit_wolf", [&](int i) {
        mat[0] = static_cast<f64>(i & 31) * 0.01;
        return ledoit_wolf(ConstSpan<f64>{mat.data(), 50 * 5}, 50, 5, &alloc).shrinkage;
    });
    bench("oas", [&](int i) {
        mat[0] = static_cast<f64>(i & 31) * 0.01;
        return oas(ConstSpan<f64>{mat.data(), 50 * 5}, 50, 5, &alloc).shrinkage;
    });

    std::printf("# acc=%g\n", acc);
    return 0;
}
