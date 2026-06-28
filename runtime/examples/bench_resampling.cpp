// v12-o resampling crush bench — wall-time to compute a percentile bootstrap CI of the mean (n=100, B resamples) vs
// scipy.stats.bootstrap / R boot / MATLAB bootci. Cerid wins on native speed + crd-jobs parallelism, AND offers the
// determinism moat (the same seed is bit-reproducible and thread-partition-invariant) that the peers cannot.

#include <crd/hesap/stats/resampling_parallel.hpp>

#include <crd/jobs/jobs.hpp>

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
    crd::jobs::init();
    crd::memory::TlsfAllocator alloc(static_cast<usize>(1) << 28);
    const usize n = 100;
    crd::containers::Array<f64> data(&alloc);
    data.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        data[i] = 2.0 + 0.7 * crd::math::sin(0.30 * static_cast<f64>(i));
    }
    const auto d = ConstSpan<f64>{data.data(), n};
    const auto meanstat = [](ConstSpan<f64> x) {
        f64 s = 0;
        for (f64 v : x)
        {
            s += v;
        }
        return s / static_cast<f64>(x.size());
    };
    const usize B = 100000;
    const int reps = 10;

    auto time_ms = [&](auto fn) {
        fn(); // warmup
        const auto t0 = clk::now();
        for (int i = 0; i < reps; ++i)
        {
            volatile f64 sink = fn();
            (void)sink;
        }
        const auto t1 = clk::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
    };

    const double ser = time_ms([&] {
        const auto ci = bootstrap_ci(d, meanstat, B, BootMethod::Percentile, 0.05, 12345, &alloc);
        return ci.low + ci.high;
    });
    const double par = time_ms([&] {
        const auto ci = bootstrap_ci_parallel(d, meanstat, B, BootMethod::Percentile, 0.05, 12345, &alloc);
        return ci.low + ci.high;
    });
    const auto ci = bootstrap_ci(d, meanstat, B, BootMethod::Percentile, 0.05, 12345, &alloc);

    std::printf("=== Cerid (gcc-release, n=%zu, B=%zu, %d workers) ===\n", n, B, crd::jobs::num_workers());
    std::printf("bootstrap_ci serial    %8.3f ms\n", ser);
    std::printf("bootstrap_ci parallel  %8.3f ms\n", par);
    std::printf("# CI = [%.6f, %.6f]  (serial==parallel, bit-identical; same seed reproducible + thread-invariant)\n",
                ci.low, ci.high);
    crd::jobs::shutdown();
    return 0;
}
