// prof_fft_kernel — isolated Cerid FFT kernel for perf profiling. Runs ONE size in a tight loop so `perf stat`
// attributes uops/IPC/port-pressure/cache-misses to the Cerid butterfly alone (no ref libs in the binary).
// argv: log2N iters.  Build: see scripts/prof_fft_kernel.sh.  Use at an L1-resident N (e.g. 10 => 1024) to
// measure the PURE kernel quality (no memory pressure) — that 0.44× vs MKL is the real wall.

#include <crd/containers/array.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using crd::f64;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;
using crd::hesap::Complex;

int main(int argc, char** argv)
{
    const int lg = argc > 1 ? std::atoi(argv[1]) : 10;
    const long iters = argc > 2 ? std::atol(argv[2]) : 2000000;
    const usize n = usize{1} << lg;

    crd::memory::TlsfAllocator alloc(1ULL << 30);
    cont::Array<Complex<f64>> work(&alloc);
    work.resize(n);
    crd::u64 s = 0x1234;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        work[i] = Complex<f64>{static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53), 0.5};
    }
    const fft::FftPlan<f64> plan(&alloc, n);

    const auto t0 = std::chrono::steady_clock::now();
    for (long it = 0; it < iters; ++it)
    {
        plan.execute(cont::Span<Complex<f64>>(work.data(), n), fft::FftDirection::Forward);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double per = secs / static_cast<double>(iters);
    const double gf = 5.0 * static_cast<double>(n) * std::log2(static_cast<double>(n)) / (per * 1e9);
    std::printf("N=%zu iters=%ld  %.3f ns/fft  %.2f GFLOPS  (sink=%.3f)\n", n, iters, per * 1e9, gf,
                work[0].re + work[n / 2].im);
#ifdef CRD_FFT_PROFILE
    fft::prof::dump();
#endif
    return 0;
}
