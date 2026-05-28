// bench_hesap_gemm_parallel — Phase 3.1.6 v0d-parallelism characterization.
//
// Measures GEMM scaling across num_workers = {1, 2, 4, 8, 16, 32} at N = 1024
// for f32 and f64. Reports per-worker GFLOPS and SPEEDUP relative to
// num_workers=1 (serial gemm). The intrinsics-class single-core peak is
// reused from bench_hesap_gemm; multi-core peak = single_core_peak × cores.
//
// Scaling target (intrinsics class — ADR-0082):
//   - 2 workers : >= 1.7× speedup
//   - 4 workers : >= 3.0× speedup
//   - 8 workers : >= 4.5× speedup
//   - 16 workers: >= 6.0× speedup
//
// The bench is exploratory — no failure threshold. Numbers go in the v0d-
// parallelism session log + the perf table in `docs/systems/hesap.md`.

#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace
{
crd::f64 measure_clock_ghz()
{
    const auto t0 = std::chrono::steady_clock::now();
    volatile crd::u64 x = 0;
    for (crd::u64 i = 0; i < 100000000ULL; ++i)
    {
        x += i;
    }
    (void)x;
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<crd::f64, std::milli>(t1 - t0).count();
    return 100.0 / ms;
}

crd::f64 peak_gflops_single_core_f32(crd::f64 clock_ghz)
{
#if CRD_SIMD_HAS_AVX2
    constexpr crd::f64 kFlopsPerCyc = 32.0;
#elif CRD_SIMD_HAS_SSE2
    constexpr crd::f64 kFlopsPerCyc = 8.0;
#else
    constexpr crd::f64 kFlopsPerCyc = 2.0;
#endif
    return kFlopsPerCyc * clock_ghz;
}

crd::f64 peak_gflops_single_core_f64(crd::f64 clock_ghz)
{
#if CRD_SIMD_HAS_AVX2
    constexpr crd::f64 kFlopsPerCyc = 16.0;
#elif CRD_SIMD_HAS_SSE2
    constexpr crd::f64 kFlopsPerCyc = 4.0;
#else
    constexpr crd::f64 kFlopsPerCyc = 2.0;
#endif
    return kFlopsPerCyc * clock_ghz;
}

template <typename T> struct RunResult
{
    crd::f64 elapsed_s;
    int iters;
    crd::f64 gflops;
};

template <typename T> RunResult<T> run_workers(crd::usize n, crd::u32 num_workers, crd::memory::IAllocator* alloc)
{
    using namespace crd::hesap::dense;
    Matrix<T> a(alloc, n, n);
    Matrix<T> b(alloc, n, n);
    Matrix<T> c(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a(i, j) = static_cast<T>(i + 1) * static_cast<T>(0.001) + static_cast<T>(j) * static_cast<T>(0.0002);
            b(i, j) = static_cast<T>(j + 1) * static_cast<T>(0.0007) - static_cast<T>(i) * static_cast<T>(0.0003);
            c(i, j) = T{};
        }
    }

    // Warm-up.
    if (num_workers <= 1)
    {
        gemm<T, Layout::RowMajor>(T{1}, a, b, T{}, c);
    }
    else
    {
        gemm_parallel<T, Layout::RowMajor>(num_workers, T{1}, a, b, T{}, c);
    }
    // crd::jobs::parallel_for stores JobDecl arrays in the per-thread frame arena
    // (1 MB default, never reclaimed until frame_reset). gemm_parallel emits 4
    // parallel_for calls per N=1024 invocation, so ~50 timed iters per size *
    // 5 sizes = 1000+ JobDecls would otherwise exhaust the arena mid-bench.
    crd::jobs::frame_reset();

    const auto t0 = std::chrono::steady_clock::now();
    int iters = 0;
    while (true)
    {
        if (num_workers <= 1)
        {
            gemm<T, Layout::RowMajor>(T{1}, a, b, T{}, c);
        }
        else
        {
            gemm_parallel<T, Layout::RowMajor>(num_workers, T{1}, a, b, T{}, c);
        }
        ++iters;
        crd::jobs::frame_reset();
        const auto elapsed = std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed > 0.5 && iters >= 3)
        {
            break;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const crd::f64 elapsed_s = std::chrono::duration<crd::f64>(t1 - t0).count();
    const crd::f64 n_d = static_cast<crd::f64>(n);
    const crd::f64 flops_per_iter = 2.0 * n_d * n_d * n_d;
    const crd::f64 gflops = (flops_per_iter * iters) / (elapsed_s * 1e9);
    return {elapsed_s, iters, gflops};
}

template <typename T>
void run_size(const char* label, crd::usize n, crd::f64 single_core_peak, crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "==== %s gemm scaling at N=%zu (single-core peak %.1f GFLOPS) ====\n", label,
                 static_cast<std::size_t>(n), single_core_peak);
    const auto baseline = run_workers<T>(n, 1U, alloc);
    std::fprintf(stdout, "  %-4s  workers=%2u  iters=%4d  %.3f s  =>  %7.2f GFLOPS  speedup=1.00x\n", label, 1U,
                 baseline.iters, baseline.elapsed_s, baseline.gflops);
    for (crd::u32 nw : {2U, 4U, 8U, 16U, 32U})
    {
        const auto r = run_workers<T>(n, nw, alloc);
        const crd::f64 speedup = r.gflops / baseline.gflops;
        const crd::f64 multi_peak = single_core_peak * static_cast<crd::f64>(nw);
        const crd::f64 pct = (r.gflops / multi_peak) * 100.0;
        std::fprintf(stdout,
                     "  %-4s  workers=%2u  iters=%4d  %.3f s  =>  %7.2f GFLOPS  speedup=%.2fx  "
                     "(%.1f%% of %.1f-core peak)\n",
                     label, nw, r.iters, r.elapsed_s, r.gflops, speedup, pct, static_cast<crd::f64>(nw));
    }
    std::fprintf(stdout, "\n");
}
} // namespace

int main()
{
    crd::jobs::init();
    const crd::f64 clock_ghz = measure_clock_ghz();
    const crd::f64 peak_f32 = peak_gflops_single_core_f32(clock_ghz);
    const crd::f64 peak_f64 = peak_gflops_single_core_f64(clock_ghz);

    std::fprintf(stdout,
                 "==== bench_hesap_gemm_parallel — v0d-parallelism scaling (ADR-0082) ====\n"
                 "  SIMD backend     : %s\n"
                 "  Coarse clock     : %.2f GHz\n"
                 "  Single-core peak : f32 = %.1f GFLOPS, f64 = %.1f GFLOPS\n"
                 "  Job workers      : %u (incl. main)\n"
                 "  Reference class  : intrinsics-tier (Eigen / Faer / Highway peers)\n\n",
                 crd::math::simd::backend_name(), clock_ghz, peak_f32, peak_f64, crd::jobs::num_workers());

    {
        crd::memory::TlsfAllocator alloc_f32(128 * 1024 * 1024);
        run_size<crd::f32>("f32", 1024U, peak_f32, &alloc_f32);
    }
    {
        crd::memory::TlsfAllocator alloc_f64(128 * 1024 * 1024);
        run_size<crd::f64>("f64", 1024U, peak_f64, &alloc_f64);
    }

    std::fprintf(stdout, "Done.\n");
    crd::jobs::shutdown();
    return 0;
}
