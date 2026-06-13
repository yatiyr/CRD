// bench_fft_vs_refs — v10 FFT throughput shootout vs the gold standards (WSL, g++ -O2 -mavx2 -mfma).
// Complex f64 forward FFT, power-of-2 sizes, in-place. Contenders on IDENTICAL data:
//   Cerid crd::hesap::fft::FftPlan  vs  FFTW3 (FFTW_MEASURE)  vs  PocketFFT (scipy/numpy backend)  vs
//   Intel MKL DFTI.
// Reports best-of-reps wall (ms) + GFLOPS (5·n·log2 n / t, the standard FFT flop count) + max |Cerid-FFTW|
// rel error (correctness cross-check — a fast wrong answer is not a win). The beat-MKL verdict on the
// AVX2-level i9-14900K (no AVX-512 ⇒ MKL runs AVX2 here ⇒ level fight). Compile: scripts/run_bench_fft.sh
// (Cerid<-> std::complex<double> share layout {re,im}; reinterpret_cast for ref interop).
//
// Cerid layout (the immutable plan reused across reps): radix-2 in v10-a (the baseline — expected to LOSE
// here); the Stockham radix-8 SIMD codelets land in v10-b and re-run this same bench (measure -> crush).

#include <crd/containers/array.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <fftw3.h>
#include <mkl_dfti.h>

#include <complex>
#include <pocketfft_hdronly.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

using crd::f64;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;
using crd::hesap::Complex;

namespace
{
template <typename Fn> double time_best_ms(int reps, Fn&& fn)
{
    double best = 1e300;
    for (int r = 0; r < reps; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        best = ms < best ? ms : best;
    }
    return best;
}

double gflops(usize n, double ms)
{
    const double flops = 5.0 * static_cast<double>(n) * std::log2(static_cast<double>(n));
    return flops / (ms * 1e6); // ms→s (1e-3) and flops→Gflops (1e-9) ⇒ /1e6
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(2ULL << 30);
    std::printf("=== complex-f64 forward FFT, power-of-2, in-place (best of reps). GFLOPS = 5 n log2(n)/t ===\n");
    std::printf("%-9s | %-22s | %-20s | %-20s | %-20s | maxrel\n", "n", "Cerid (ms / GFLOPS)", "FFTW (ms/GFLOPS)",
                "PocketFFT (ms/GFLOPS)", "MKL (ms/GFLOPS)");

    for (int lg = 10; lg <= 23; ++lg)
    {
        const usize n = usize{1} << lg;
        const int reps = n <= (1U << 16) ? 200 : (n <= (1U << 20) ? 30 : 8);

        // Reference input (deterministic).
        cont::Array<Complex<f64>> base(&alloc);
        base.resize(n);
        crd::u64 s = 0x12345 ^ n;
        for (usize i = 0; i < n; ++i)
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            const double a = static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53) * 2.0 - 1.0;
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            const double b = static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53) * 2.0 - 1.0;
            base[i] = Complex<f64>{a, b};
        }

        cont::Array<Complex<f64>> work(&alloc);
        work.resize(n);
        auto reset = [&] { std::memcpy(work.data(), base.data(), n * sizeof(Complex<f64>)); };

        // ---- Cerid ----
        const fft::FftPlan<f64> plan(&alloc, n);
        const double cms = time_best_ms(reps, [&] { reset(); plan.execute(cont::Span<Complex<f64>>(work.data(), n), fft::FftDirection::Forward); });
        cont::Array<Complex<f64>> cerid_out(&alloc);
        cerid_out.resize(n);
        std::memcpy(cerid_out.data(), work.data(), n * sizeof(Complex<f64>));

        // ---- FFTW (MEASURE) ----
        auto* fin = reinterpret_cast<fftw_complex*>(work.data());
        fftw_plan fp = fftw_plan_dft_1d(static_cast<int>(n), fin, fin, FFTW_FORWARD, FFTW_MEASURE);
        const double fms = time_best_ms(reps, [&] { reset(); fftw_execute(fp); });
        cont::Array<Complex<f64>> fftw_out(&alloc);
        fftw_out.resize(n);
        std::memcpy(fftw_out.data(), work.data(), n * sizeof(Complex<f64>));
        fftw_destroy_plan(fp);

        // ---- PocketFFT ----
        cont::Array<Complex<f64>> pout(&alloc);
        pout.resize(n);
        const pocketfft::shape_t shape{n};
        const pocketfft::stride_t stride{static_cast<std::ptrdiff_t>(sizeof(std::complex<double>))};
        const pocketfft::shape_t axes{0};
        const auto* pin = reinterpret_cast<const std::complex<double>*>(base.data());
        auto* pdst = reinterpret_cast<std::complex<double>*>(pout.data());
        const double pms = time_best_ms(reps, [&] { pocketfft::c2c(shape, stride, stride, axes, pocketfft::FORWARD, pin, pdst, 1.0); });

        // ---- MKL DFTI ----
        DFTI_DESCRIPTOR_HANDLE h = nullptr;
        DftiCreateDescriptor(&h, DFTI_DOUBLE, DFTI_COMPLEX, 1, static_cast<MKL_LONG>(n));
        DftiSetValue(h, DFTI_PLACEMENT, DFTI_INPLACE);
        DftiCommitDescriptor(h);
        const double mms = time_best_ms(reps, [&] { reset(); DftiComputeForward(h, work.data()); });
        DftiFreeDescriptor(&h);

        // correctness cross-check: Cerid vs FFTW.
        double maxrel = 0.0;
        double maxmag = 0.0;
        for (usize i = 0; i < n; ++i)
        {
            maxmag = std::max(maxmag, std::hypot(fftw_out[i].re, fftw_out[i].im));
        }
        for (usize i = 0; i < n; ++i)
        {
            maxrel = std::max(maxrel, std::hypot(cerid_out[i].re - fftw_out[i].re, cerid_out[i].im - fftw_out[i].im));
        }
        maxrel /= (1.0 + maxmag);

        std::printf("%-9zu | %8.3f / %7.2f      | %7.3f / %7.2f    | %7.3f / %7.2f    | %7.3f / %7.2f    | %.1e\n",
                    n, cms, gflops(n, cms), fms, gflops(n, fms), pms, gflops(n, pms), mms, gflops(n, mms), maxrel);
    }
    return 0;
}
