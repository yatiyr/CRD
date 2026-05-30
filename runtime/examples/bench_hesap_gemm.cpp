// bench_hesap_gemm — Phase 3.1.6 v0d-perf honest perf characterization.
//
// Measures GEMM throughput across N = {64, 128, 256, 512, 1024} for f32
// and f64; computes GFLOPS = 2 * N^3 / elapsed; reports actual / peak
// ratio. Peak is computed from CPU detection at runtime (best effort —
// see kHonestCharacterizationNote below). Intrinsics-class reference per
// ADR-0082 (Eigen / Faer / Highway tier, not MKL/BLIS-asm).

#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
// Honest characterization note (ADR-0082):
// - "Peak FLOPS" depends on CPU clock × SIMD width × FMA throughput × cores.
// - Without runtime cpuid we estimate from CRD_SIMD_HAS_AVX2 (8-wide f32 *
//   2 ops/FMA = 16 FLOPS per cycle per FMA port × 2 ports = 32 FLOPS/cyc
//   single-threaded on AVX2 Intel; AMD Zen has 1 FMA port = 16 FLOPS/cyc).
// - For f64 halve the SIMD width: 8 FLOPS/cyc / FMA port.
// - We report PER-CORE peak (single-threaded GEMM) since v0d-perf foundation
//   is single-threaded; outer-loop parallelism + multi-core peak land in the
//   `v0d-parallelism` follow-on.
//
// Clock measurement: probe via a calibration loop measuring known cycle
// count operations. Coarse but adequate for "are we in the right
// ballpark" characterization.

crd::f64 measure_clock_ghz()
{
    // 1e8 dependent integer adds; depends on integer ALU throughput.
    // Modern CPUs run integer chains at ~1 op/cyc => 1e8 cycles ≈ ~30 ms
    // on a 3 GHz core. Used only for coarse GHz estimate, not perf.
    const auto t0 = std::chrono::steady_clock::now();
    volatile crd::u64 x = 0;
    for (crd::u64 i = 0; i < 100000000ULL; ++i)
    {
        x += i;
    }
    (void)x;
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<crd::f64, std::milli>(t1 - t0).count();
    // 1e8 cycles / (ms / 1000) / 1e9 = 1e8 / ms / 1e6 = 100 / ms GHz
    return 100.0 / ms;
}

crd::f64 peak_gflops_single_core_f32(crd::f64 clock_ghz)
{
    // AVX2: 8-wide f32 × 2 FMA ports × 2 ops/FMA = 32 FLOPS/cyc.
    // SSE2: 4-wide f32 × 1 FMA port × 2 ops/FMA = 8 FLOPS/cyc.
    // Scalar: 1 × 2 = 2 FLOPS/cyc.
#if CRD_SIMD_HAS_AVX2
    constexpr crd::f64 flops_per_cyc = 32.0;
#elif CRD_SIMD_HAS_SSE2
    constexpr crd::f64 flops_per_cyc = 8.0;
#else
    constexpr crd::f64 flops_per_cyc = 2.0;
#endif
    return flops_per_cyc * clock_ghz;
}

crd::f64 peak_gflops_single_core_f64(crd::f64 clock_ghz)
{
    // Half the SIMD width: AVX2 f64 = 16 FLOPS/cyc, SSE2 = 4, scalar = 2.
#if CRD_SIMD_HAS_AVX2
    constexpr crd::f64 flops_per_cyc = 16.0;
#elif CRD_SIMD_HAS_SSE2
    constexpr crd::f64 flops_per_cyc = 4.0;
#else
    constexpr crd::f64 flops_per_cyc = 2.0;
#endif
    return flops_per_cyc * clock_ghz;
}

template <typename T>
void run_size(const char* label, crd::usize n, crd::f64 peak_gflops, crd::memory::IAllocator* alloc)
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
    gemm<T, Layout::RowMajor>(T{1}, a, b, T{}, c);

    // Time at least 3 iterations (or 500 ms wall clock).
    const auto t0 = std::chrono::steady_clock::now();
    int iters = 0;
    while (true)
    {
        gemm<T, Layout::RowMajor>(T{1}, a, b, T{}, c);
        ++iters;
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
    const crd::f64 pct = (gflops / peak_gflops) * 100.0;
    std::fprintf(stdout, "  %-4s  N=%5zu  iters=%4d  %.2f s  =>  %7.2f GFLOPS  (%.1f%% of %.1f GFLOPS peak)\n", label,
                 static_cast<std::size_t>(n), iters, elapsed_s, gflops, pct, peak_gflops);
}

// v5a-4 cmod ceiling-spike: time the ACTUAL supernodal-cmod gemm shape — ColMajor,
// C(m×nn) = A(m×k) · B(nn×k)^T (NT, the `am · am1^T` Schur update), full gemm streaming
// from L2/L3 (NOT an L1 single tile). Discriminates the cmod 52%-of-peak wall: if the
// CLEAN shape hits ~90% the in-situ loss is cold/strided descendant-panel reads (lever =
// cache-blocking those reads); if it caps ~55% the NT-ColMajor kernel itself is the wall.
template <typename T, crd::hesap::dense::Layout L>
void run_gemm_shape(const char* tag, crd::usize m, crd::usize nn, crd::usize k, crd::hesap::dense::Trans ta,
                    crd::hesap::dense::Trans tb, crd::f64 peak_gflops, crd::memory::IAllocator* alloc)
{
    using namespace crd::hesap::dense;
    // C(m×nn) = op(A) · op(B). A dims = (m×k) if ta==None else (k×m); B = (k×nn) if tb==None else (nn×k).
    Matrix<T, L> a(alloc, ta == Trans::None ? m : k, ta == Trans::None ? k : m);
    Matrix<T, L> b(alloc, tb == Trans::None ? k : nn, tb == Trans::None ? nn : k);
    Matrix<T, L> c(alloc, m, nn);
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        for (crd::usize j = 0; j < a.cols(); ++j)
        {
            a(i, j) = static_cast<T>(i + 1) * static_cast<T>(0.001) + static_cast<T>(j) * static_cast<T>(0.0002);
        }
    }
    for (crd::usize i = 0; i < b.rows(); ++i)
    {
        for (crd::usize j = 0; j < b.cols(); ++j)
        {
            b(i, j) = static_cast<T>(j + 1) * static_cast<T>(0.0007) - static_cast<T>(i) * static_cast<T>(0.0003);
        }
    }
    for (crd::usize i = 0; i < m * nn; ++i)
    {
        c.data()[i] = T{};
    }
    gemm<T, L>(T{1}, a, b, T{}, c, ta, tb); // warm
    const auto t0 = std::chrono::steady_clock::now();
    int iters = 0;
    while (true)
    {
        gemm<T, L>(T{1}, a, b, T{}, c, ta, tb);
        ++iters;
        const auto el = std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
        if (el > 0.5 && iters >= 3)
        {
            break;
        }
    }
    const crd::f64 elapsed_s = std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
    const crd::f64 flops_per_iter = 2.0 * static_cast<crd::f64>(m) * static_cast<crd::f64>(nn) * static_cast<crd::f64>(k);
    const crd::f64 gflops = (flops_per_iter * iters) / (elapsed_s * 1e9);
    std::fprintf(stdout, "  %-10s M=%-5zu N=%-5zu K=%-4zu  %6.2f GFLOPS  (%.1f%% of %.1f peak)\n", tag,
                 static_cast<std::size_t>(m), static_cast<std::size_t>(nn), static_cast<std::size_t>(k), gflops,
                 gflops / peak_gflops * 100.0, peak_gflops);
}

} // namespace

int main()
{
    const crd::f64 clock_ghz = measure_clock_ghz();
    const crd::f64 peak_f32 = peak_gflops_single_core_f32(clock_ghz);
    const crd::f64 peak_f64 = peak_gflops_single_core_f64(clock_ghz);

    std::fprintf(stdout,
                 "==== bench_hesap_gemm — v0d-perf intrinsics characterization (ADR-0082) ====\n"
                 "  SIMD backend     : %s\n"
                 "  Coarse clock     : %.2f GHz\n"
                 "  Single-core peak : f32 = %.1f GFLOPS, f64 = %.1f GFLOPS\n"
                 "  Reference class  : intrinsics-tier (Eigen / Faer / Highway peers; NOT MKL-asm tier)\n"
                 "  Target           : 80%%-85%% of single-core peak per ADR-0082 §decision\n"
                 "\n",
                 crd::math::simd::backend_name(), clock_ghz, peak_f32, peak_f64);

    crd::memory::TlsfAllocator alloc(256 * 1024 * 1024);
    std::fprintf(stdout, "==== f32 gemm ====\n");
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}, crd::usize{1024}})
    {
        run_size<crd::f32>("f32", n, peak_f32, &alloc);
    }
    std::fprintf(stdout, "==== f64 gemm ====\n");
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}, crd::usize{1024}})
    {
        run_size<crd::f64>("f64", n, peak_f64, &alloc);
    }
    std::fprintf(stdout, "==== f64 cmod-shape (supernodal Schur-update gemm) ====\n");
    using crd::hesap::dense::Trans;
    using crd::hesap::dense::Layout;
    constexpr auto N = Trans::None;
    constexpr auto Tr = Trans::Transpose;
    // LAYOUT/TRANS GRID at K=512 (square RowMajor-NN = 80% is the ceiling): isolate which axis costs.
    // cmod is ColMajor-NT; reinterpreting the ColMajor K-panel as RowMajor makes it RowMajor-TN (zero-copy).
    // If RowMajor-TN hits ~80%, the cmod can reroute (view-flip + transposed scatter), no kernel rewrite.
    run_gemm_shape<crd::f64, Layout::RowMajor>("rm-NN", 512, 512, 512, N, N, peak_f64, &alloc);
    run_gemm_shape<crd::f64, Layout::RowMajor>("rm-TN", 512, 512, 512, Tr, N, peak_f64, &alloc); // cmod reroute target
    run_gemm_shape<crd::f64, Layout::RowMajor>("rm-NT", 512, 512, 512, N, Tr, peak_f64, &alloc);
    run_gemm_shape<crd::f64, Layout::RowMajor>("rm-TT", 512, 512, 512, Tr, Tr, peak_f64, &alloc);
    run_gemm_shape<crd::f64, Layout::ColMajor>("cm-NN", 512, 512, 512, N, N, peak_f64, &alloc);
    run_gemm_shape<crd::f64, Layout::ColMajor>("cm-NT", 512, 512, 512, N, Tr, peak_f64, &alloc); // = cmod, K512
    // In-situ cmod shapes (ColMajor-NT). Tall-M is the DOMINANT real shape — the target metric.
    run_gemm_shape<crd::f64, Layout::ColMajor>("cm-NT", 512, 512, 200, N, Tr, peak_f64, &alloc);  // advisor canonical
    run_gemm_shape<crd::f64, Layout::ColMajor>("cm-NT", 1024, 512, 256, N, Tr, peak_f64, &alloc);
    run_gemm_shape<crd::f64, Layout::ColMajor>("cm-NT", 2048, 512, 200, N, Tr, peak_f64, &alloc); // tall front (dominant)
    // The reroute target at the dominant tall shape (RowMajor-TN, transposed dims k×m / k×n):
    run_gemm_shape<crd::f64, Layout::RowMajor>("rm-TN", 2048, 512, 200, Tr, N, peak_f64, &alloc);
    std::fprintf(stdout, "\nDone.\n");
    return 0;
}
